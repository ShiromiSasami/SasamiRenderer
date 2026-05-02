#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/RayTracing/GpuSoftwareRayTracer.h"

#include <cstdint>
#include <wrl.h>

namespace SasamiRenderer
{
    // =========================================================================
    // IrradianceProbeGrid
    //
    // Manages a world-space grid of irradiance probes.  Each probe stores
    // L2 Spherical Harmonics (9 float4 = 144 bytes) and is updated
    // incrementally each frame via a compute shader that traces rays
    // through the SWRT BVH.
    //
    // Update model:
    //   - Up to kProbesPerFrame probes are updated per call to UpdateProbes().
    //   - A round-robin counter cycles through all probes so the full grid
    //     refreshes every ceil(totalProbes / kProbesPerFrame) frames.
    //   - Per-probe SH is blended with an exponential moving average (EMA)
    //     controlled by m_emaAlpha, giving smooth temporal convergence.
    //
    // Integration:
    //   - GetProbeDataSrv() → bind to t10 in PBR_PS (read-only)
    //   - GetProbeGridCbGpuAddress() → bind to b2 in PBR_PS (GIProbeGridCB)
    // =========================================================================
    class IrradianceProbeGrid
    {
    public:
        // CPU-side CB that mirrors GIProbeGridCB in GI_Common.hlsli
        struct alignas(256) GIProbeGridCBData
        {
            float    probeOrigin[3];
            float    giIntensity;
            float    probeSpacing[3];
            float    giEnabled;         // 1.0f = enabled
            uint32_t probeCountX;
            uint32_t probeCountY;
            uint32_t probeCountZ;
            uint32_t probeTotalCount;
        };
        static_assert(sizeof(GIProbeGridCBData) == 256u); // alignas(256) pads to 256; raw data is 48 bytes

        // CPU-side CB that mirrors GIUpdateCB in GI_ProbeUpdate_CS.hlsl
        struct alignas(256) GIUpdateCBData
        {
            float    probeOrigin[3];
            float    pad0;
            float    probeSpacing[3];
            float    pad1;
            uint32_t probeCountX;
            uint32_t probeCountY;
            uint32_t probeCountZ;
            uint32_t baseProbeIndex;

            float    emaAlpha;
            float    maxTraceDistance;
            float    shadowBias;
            uint32_t frameIndex;

            float    dirLightDir[3];
            float    dirLightIntensity;
            float    dirLightColor[3];
            float    ambientIntensity;

            float    ambientColor[3];
            uint32_t probesThisDispatch;
        };
        static_assert(sizeof(GIUpdateCBData) == 256u); // alignas(256) pads to 256; raw data is 112 bytes

        struct UpdateDesc
        {
            // Directional light data (same as SWRT reflection)
            float dirLightDir[3]   = { 0.0f, -1.0f, 0.0f };
            float dirLightIntensity = 1.0f;
            float dirLightColor[3] = { 1.0f, 1.0f, 1.0f };
            float ambientColor[3]  = { 0.1f, 0.1f, 0.1f };
            float ambientIntensity = 1.0f;
            float shadowBias       = 0.005f;
            uint32_t frameIndex    = 0;
        };

        IrradianceProbeGrid();
        ~IrradianceProbeGrid();

        IrradianceProbeGrid(const IrradianceProbeGrid&) = delete;
        IrradianceProbeGrid& operator=(const IrradianceProbeGrid&) = delete;

        // Call once at startup (after GpuSoftwareRayTracer is initialised).
        bool Initialize(IRHIDevice& device);

        // Configure probe grid extents.  Safe to call before Initialize().
        void SetGridOrigin(float x, float y, float z)  { m_originX=x; m_originY=y; m_originZ=z; }
        void SetGridSpacing(float x, float y, float z) { m_spacingX=x; m_spacingY=y; m_spacingZ=z; }
        void SetGridCount(uint32_t cx, uint32_t cy, uint32_t cz);

        // Auto-fit the grid to cover the given world AABB with a margin.
        void FitToSceneBounds(float bMinX, float bMinY, float bMinZ,
                               float bMaxX, float bMaxY, float bMaxZ,
                               float margin = 2.0f);

        float GetGiIntensity()     const { return m_giIntensity; }
        void  SetGiIntensity(float v)   { m_giIntensity = (v >= 0.0f) ? v : 0.0f; }
        bool  GetEnabled()         const { return m_enabled; }
        void  SetEnabled(bool e)         { m_enabled = e; }
        float GetEmaAlpha()        const { return m_emaAlpha; }
        void  SetEmaAlpha(float a)       { m_emaAlpha = (a > 0.0f && a <= 1.0f) ? a : 0.1f; }
        float GetMaxTraceDistance()const { return m_maxTraceDistance; }
        void  SetMaxTraceDistance(float d) { m_maxTraceDistance = (d > 0.0f) ? d : 50.0f; }

        // Dispatch the probe update compute shader.
        // bvhAddrs must be valid (obtained from GpuSoftwareRayTracer::GetBvhGpuAddresses).
        bool UpdateProbes(const UpdateDesc& desc,
                          const GpuSoftwareRayTracer::BvhGpuAddresses& bvhAddrs,
                          IRHIDevice& device,
                          CommandList& cmdList);

        // Bindings for PBR_PS (valid after Initialize).
        D3D12_GPU_VIRTUAL_ADDRESS GetProbeDataGpuVA()       const;
        D3D12_GPU_VIRTUAL_ADDRESS GetProbeGridCbGpuAddress() const;

        bool IsInitialized() const { return m_initialized; }
        uint32_t GetTotalProbeCount() const { return m_countX * m_countY * m_countZ; }
        uint32_t GetCountX() const { return m_countX; }
        uint32_t GetCountY() const { return m_countY; }
        uint32_t GetCountZ() const { return m_countZ; }
        float GetOriginX() const { return m_originX; }
        float GetOriginY() const { return m_originY; }
        float GetOriginZ() const { return m_originZ; }
        float GetSpacingX() const { return m_spacingX; }

        // Re-allocates the probe buffer after FitToSceneBounds changes count at runtime.
        // No-op if count and buffer size have not changed.
        bool ReallocProbeBuffer(IRHIDevice& device) { return AllocateProbeBuffer(device); }

        // Writes current grid parameters (origin, spacing, counts) to the persistently-mapped
        // GPU constant buffer immediately.  Call after any parameter change to ensure the
        // debug visualization reflects the new layout without waiting for UpdateProbes().
        // Declared const so it can be called from DebugProbeGridRenderNode::Execute (which is const).
        void FlushGridCB() const;

    private:
        bool CreatePipeline(IRHIDevice& device);
        bool AllocateProbeBuffer(IRHIDevice& device);
        void FillProbeGridCB(GIProbeGridCBData& out) const;
        void FillUpdateCB(const UpdateDesc& desc, uint32_t baseIdx,
                          uint32_t count, GIUpdateCBData& out) const;

        // Grid parameters
        float    m_originX  = 0.0f, m_originY = 0.0f,  m_originZ = 0.0f;
        float    m_spacingX = 2.0f, m_spacingY = 2.0f, m_spacingZ = 2.0f;
        uint32_t m_countX   = 8u,   m_countY   = 4u,   m_countZ   = 8u;

        float    m_giIntensity      = 1.0f;
        float    m_emaAlpha         = 0.1f;   // 10% new sample each update
        float    m_maxTraceDistance = 50.0f;
        bool     m_enabled          = true;

        // Probe data GPU buffer  (RWStructuredBuffer<float4>, 9 float4 per probe)
        Resource m_probeBuffer;
        uint32_t m_probeBufferCapacity = 0u;  // allocated probe count

        // Descriptor heap: [0] SRV (probe data, for PBR_PS), [1] UAV (for update CS)
        DescriptorHeap m_descHeap;
        UINT           m_descStride = 0u;
        GpuDescriptorHandle m_probeSrv{};     // slot 0: t10 in PBR_PS
        GpuDescriptorHandle m_probeUav{};     // slot 1: u0 in update CS

        // Persistently-mapped upload CB:
        //   [0]: GIProbeGridCBData (48 bytes, padded to 256)
        //   [1]: GIUpdateCBData    (96 bytes, padded to 256)
        Resource m_cbBuffer;
        mutable uint8_t* m_cbMapped = nullptr;

        // Compute pipeline
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

        // Round-robin update state
        uint32_t m_nextProbeIdx = 0u;
        static constexpr uint32_t kProbesPerFrame = 32u;

        bool m_initialized = false;
    };

} // namespace SasamiRenderer
