#pragma once

#include "Renderer/RHI/GraphicsDevice.h"

#include <cstdint>
#include <string>
#include <wrl.h>

namespace SasamiRenderer
{
    // =========================================================================
    // FluidHeightfieldSim
    //
    // GPU heightfield / shallow-water ripple simulation using the discrete 2D
    // wave equation (Muller & Fischer, GDC 2008, "Fast Water Simulation for
    // Games Using Height Fields"):
    //
    //   force    = waveSpeedSq * (sum of 4 neighbor heights - 4 * center height) / cellSize^2
    //   velocity += force * dt
    //   velocity *= damping
    //   height   += velocity * dt
    //
    // A ping-pong pair of GPU buffers holds (height, velocity) per grid cell.
    // Each Update() dispatch reads one buffer and writes the other, then flips
    // which buffer is considered "current" for rendering. Grid edges use
    // clamped (Neumann-style) neighbor sampling, implemented in the compute
    // shader — no data-dependent branching is needed on the C++ side.
    //
    // Integration: GetCurrentHeightGpuVA() -> bind as an inline SRV (t0) in
    // FluidSurface_VS for per-vertex height displacement.
    // =========================================================================
    class FluidHeightfieldSim
    {
    public:
        // Mirrors FluidUpdateCB (b0) in FluidHeightfield_CS.hlsl byte-for-byte.
        struct alignas(256) FluidUpdateCBData
        {
            float    originX = 0.0f;
            float    originZ = 0.0f;
            float    cellSize = 0.25f;
            float    invCellSizeSq = 16.0f;   // 1 / cellSize^2, precomputed for the shader
            uint32_t countX = 0u;
            uint32_t countZ = 0u;
            float    waveSpeedSq = 16.0f;
            float    damping = 0.995f;
            float    dt = 0.0f;
            uint32_t splashActive = 0u;       // 0 or 1
            float    splashX = 0.0f;
            float    splashZ = 0.0f;
            float    splashRadius = 0.5f;
            float    splashStrength = 1.0f;
            float    surfaceHeight = 0.0f;    // world-space Y offset of the rest surface; read by FluidSurface_VS
            float    pad1 = 0.0f;
        };
        static_assert(sizeof(FluidUpdateCBData) == 256u);

        struct SplashDesc
        {
            float worldX = 0.0f;
            float worldZ = 0.0f;
            float radius = 0.5f;
            float strength = 1.0f;
        };

        FluidHeightfieldSim();
        ~FluidHeightfieldSim();

        FluidHeightfieldSim(const FluidHeightfieldSim&) = delete;
        FluidHeightfieldSim& operator=(const FluidHeightfieldSim&) = delete;

        // Call once at startup.
        bool Initialize(IRHIDevice& device);

        // Configure grid extents. Safe to call before Initialize(); triggers a
        // buffer reallocation (state reset to flat/at-rest) on the next Update().
        void SetGridOrigin(float x, float z) { m_originX = x; m_originZ = z; }
        void SetCellSize(float s);
        void SetGridCount(uint32_t cx, uint32_t cz);
        void FitToSceneBounds(float bMinX, float bMinZ, float bMaxX, float bMaxZ, float margin = 1.0f);

        float GetSurfaceHeight() const { return m_surfaceHeight; }
        void  SetSurfaceHeight(float y) { m_surfaceHeight = y; }
        float GetWaveSpeed() const { return m_waveSpeed; }
        void  SetWaveSpeed(float v) { m_waveSpeed = (v > 0.0f) ? v : 1.0f; }
        float GetDamping() const { return m_damping; }
        void  SetDamping(float d) { m_damping = (d >= 0.0f && d <= 1.0f) ? d : 0.995f; }
        bool  GetEnabled() const { return m_enabled; }
        void  SetEnabled(bool e) { m_enabled = e; }

        // Injects a single splash impulse at the given world-space XZ, consumed
        // by (and cleared after) the next Update() call.
        void InjectSplash(const SplashDesc& splash) { m_pendingSplash = splash; m_hasPendingSplash = true; }

        // Reallocates the ping-pong buffers if the grid cell count changed since the
        // last allocation. Must be called before the render graph records any reads
        // of GetCurrentHeightGpuVA()/GetUpdateCBGpuVA() into the frame's command list
        // (i.e. before RegisterPassesToRenderGraph/Execute), so a reallocation never
        // destroys a buffer already referenced by an open, unexecuted command list.
        // No-op (returns true) if disabled, uninitialized, or capacity already matches.
        bool PrepareFrame(IRHIDevice& device);

        // Dispatches the wave-equation compute update, advancing the sim by dt.
        // No-op (returns true) if disabled or the pipeline failed to compile.
        bool Update(float dt, IRHIDevice& device, CommandList& cmdList);

        // GPU VA of the buffer holding the latest simulated state. Valid after
        // Initialize(); reads as flat/zero before the first Update().
        D3D12_GPU_VIRTUAL_ADDRESS GetCurrentHeightGpuVA() const;

        // GPU VA of the update CB (FluidUpdateCBData), refreshed once per Update() call.
        // FluidSurfaceRenderPass binds this directly (b1) for grid-info fields (origin,
        // cellSize, countX/Z, surfaceHeight) instead of owning a duplicate CB — the pass
        // reads it with a 1-frame lag, same as GetCurrentHeightGpuVA().
        D3D12_GPU_VIRTUAL_ADDRESS GetUpdateCBGpuVA() const;

        bool IsInitialized() const { return m_initialized; }
        const std::string& GetLastPipelineError() const { return m_lastPipelineError; }

        uint32_t GetTotalCellCount() const { return m_countX * m_countZ; }
        uint32_t GetCountX() const { return m_countX; }
        uint32_t GetCountZ() const { return m_countZ; }
        float GetOriginX() const { return m_originX; }
        float GetOriginZ() const { return m_originZ; }
        float GetCellSize() const { return m_cellSize; }

    private:
        bool CreatePipeline(IRHIDevice& device);
        bool AllocateBuffers(IRHIDevice& device);
        Resource* GetBufferResource(int index) const;
        Resource* GetConstantBufferResource() const;
        void FillUpdateCB(float dt, FluidUpdateCBData& out) const;

        // Grid parameters.
        float    m_originX = -8.0f;
        float    m_originZ = -8.0f;
        float    m_cellSize = 0.25f;
        uint32_t m_countX = 64u;
        uint32_t m_countZ = 64u;
        float    m_surfaceHeight = 0.0f;

        float m_waveSpeed = 4.0f;
        float m_damping   = 0.995f;
        bool  m_enabled   = true;

        SplashDesc m_pendingSplash;
        bool       m_hasPendingSplash = false;

        // Ping-pong (height, velocity) buffers: RWStructuredBuffer<float2> per cell.
        // Steady-state resource state is a combined read state (PIXEL_SHADER_RESOURCE |
        // NON_PIXEL_SHADER_RESOURCE) so either buffer can be sampled by FluidSurface_VS
        // (t0) without a transition; only the buffer selected as this dispatch's write
        // target bounces out to UNORDERED_ACCESS and back inside Update().
        Resource        m_buffer[2];
        RhiBufferHandle m_bufferHandle[2]{};
        Resource*       m_bufferCompat[2] = { nullptr, nullptr };
        uint32_t        m_bufferCapacity = 0u; // allocated cell count
        int             m_readIndex = 0;       // index holding the latest valid state

        Resource        m_cbBuffer;
        RhiBufferHandle m_cbBufferHandle{};
        Resource*       m_cbBufferCompat = nullptr;
        uint8_t*        m_cbMapped = nullptr;

        IRHIDevice* m_device = nullptr;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;

        bool m_initialized      = false;
        bool m_psoMissingLogged = false;
        std::string m_lastPipelineError;
    };

} // namespace SasamiRenderer
