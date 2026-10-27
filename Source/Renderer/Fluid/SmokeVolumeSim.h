#pragma once

#include "Renderer/Fluid/GpuBufferReadbackRing.h"
#include "Renderer/RHI/GraphicsDevice.h"

#include <cstdint>
#include <vector>
#include <wrl.h>

namespace SasamiEngine::Render
{
    // =========================================================================
    // SmokeVolumeSim
    //
    // Eulerian smoke on a 3D grid, following Stam, "Stable Fluids" (SIGGRAPH 1999)
    // and its GPU form in GPU Gems chapter 38 (Harris, "Fast Fluid Dynamics
    // Simulation on the GPU"). One step is:
    //
    //   1. advect velocity and density along the velocity field, semi-Lagrangian
    //      (trace backwards, sample trilinearly -- unconditionally stable, which
    //      is the whole point of the method)
    //   2. add sources: density and velocity at the emitter, plus buoyancy
    //      (smoke is hotter than air, so density drives an upward force)
    //   3. compute the divergence of the advected velocity
    //   4. solve the Poisson equation for pressure with Jacobi iterations
    //   5. subtract the pressure gradient, leaving a divergence-free field
    //
    // Storage mirrors FluidHeightfieldSim deliberately: plain structured buffers
    // with ping-pong and inline root descriptors, not 3D textures. Textures would
    // need descriptor tables and give hardware trilinear sampling; buffers keep
    // the binding model identical to the code already here, at the cost of doing
    // the trilinear fetch by hand in the shader.
    //
    // Cell index is x + y * countX + z * countX * countY throughout -- C++,
    // compute shader and the raymarch shader all assume it.
    // =========================================================================
    class SmokeVolumeSim
    {
    public:
        // Mirrors SmokeUpdateCB (b0) in SmokeVolume_CS.hlsl byte-for-byte.
        struct alignas(256) SmokeUpdateCBData
        {
            float    originX = -2.0f;      // world-space min corner of the volume
            float    originY = 0.0f;
            float    originZ = -2.0f;
            float    cellSize = 0.0625f;

            uint32_t countX = 64u;
            uint32_t countY = 64u;
            uint32_t countZ = 64u;
            float    dt = 0.0f;

            float    emitterX = 0.0f;      // world-space centre of the source
            float    emitterY = 0.2f;
            float    emitterZ = 0.0f;
            float    emitterRadius = 0.25f;

            float    emitterDensity = 8.0f;   // density injected per second
            float    emitterVelocity = 2.0f;  // upward velocity injected per second
            float    buoyancy = 6.0f;         // upward force per unit density
            float    dissipation = 0.35f;     // density lost per second

            float    velocityDamping = 0.15f; // velocity lost per second
            // Vorticity confinement strength (Fedkiw et al. 2001). 0 disables it. Advection
            // dissipates swirl, so without this the plume rises as a straight column; this puts
            // the lost rotation back where it was lost.
            float    vorticity = 3.0f;
            uint32_t emitterActive = 1u;
            // Nonzero = the solid mask (t2) holds valid data and obstacle boundaries apply.
        uint32_t solidActive = 0u;

            // Ambient air movement. A plume rising in perfectly still air is the giveaway that a
            // simulation is running in a box -- real smoke is always being pushed by something.
            // Applied as a force toward the wind velocity rather than as a velocity override, so
            // the plume bends into it instead of snapping to it.
            float    windX = 0.0f;
            float    windY = 0.0f;
            float    windZ = 0.0f;
            float    windStrength = 0.0f;   // 0 disables

            // Buoyancy model (BuoyancyMode). Density keeps the original behaviour, where smoke
            // density stands in for heat. Temperature is Fedkiw, Stam & Jensen 2001 eq. 8:
            // f = (-densityWeight * rho + temperatureBuoyancy * (T - ambientTemperature)) * y.
            uint32_t buoyancyMode = 0u;
            float    temperatureBuoyancy = 6.0f;  // beta: upward force per unit of excess temperature
            float    densityWeight = 0.5f;        // alpha: downward pull per unit of density
            float    ambientTemperature = 0.0f;

            float    emitterTemperature = 8.0f;   // temperature injected per second at the source
            float    coolingRate = 0.6f;          // fraction of excess temperature lost per second (exp decay)
            float    reserved0 = 0.0f;
            float    reserved1 = 0.0f;
        };
        static_assert(sizeof(SmokeUpdateCBData) == 256u);

        // One cell of the state buffer. 16 bytes so the stride matches float4.
        struct StateCell
        {
            float velocityX = 0.0f;
            float velocityY = 0.0f;
            float velocityZ = 0.0f;
            float density = 0.0f;
        };
        static_assert(sizeof(StateCell) == 16u);

        // One cell of the pressure buffer. Divergence is written once per step and
        // read by every Jacobi iteration, so the two live together.
        struct PressureCell
        {
            float divergence = 0.0f;
            float pressure = 0.0f;
        };
        static_assert(sizeof(PressureCell) == 8u);

        ~SmokeVolumeSim();

        bool Initialize(GraphicsDevice& device);
        void Release();
        bool IsInitialized() const { return m_initialized; }

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // Grid resolution. Reallocates on the next PrepareFrame; clamped to
        // kMaxDimension per axis so a typo cannot ask for gigabytes.
        static constexpr uint32_t kMaxDimension = 128u;
        void SetGridSize(uint32_t countX, uint32_t countY, uint32_t countZ);
        uint32_t GetCountX() const { return m_pending.countX; }
        uint32_t GetCountY() const { return m_pending.countY; }
        uint32_t GetCountZ() const { return m_pending.countZ; }

        void SetOrigin(float x, float y, float z);
        void SetCellSize(float cellSize);
        void SetEmitter(float x, float y, float z, float radius);
        void SetEmitterActive(bool active) { m_pending.emitterActive = active ? 1u : 0u; }
        bool IsEmitterActive() const { return m_pending.emitterActive != 0u; }
        void SetBuoyancy(float buoyancy) { m_pending.buoyancy = buoyancy; }

        // Which quantity drives buoyancy. Density is the legacy model and stays the default so
        // existing scenes are unchanged; Temperature carries a separate advected scalar.
        enum class BuoyancyMode : uint32_t { Density = 0u, Temperature = 1u };
        void SetBuoyancyMode(BuoyancyMode mode) { m_pending.buoyancyMode = static_cast<uint32_t>(mode); }
        BuoyancyMode GetBuoyancyMode() const { return static_cast<BuoyancyMode>(m_pending.buoyancyMode); }
        // Temperature-mode coefficients; negative inputs are clamped to 0 by the setter.
        void SetTemperatureParams(float temperatureBuoyancy, float densityWeight,
                                  float emitterTemperature, float coolingRate);
        float GetTemperatureBuoyancy() const { return m_pending.temperatureBuoyancy; }
        float GetDensityWeight() const { return m_pending.densityWeight; }
        float GetEmitterTemperature() const { return m_pending.emitterTemperature; }
        float GetCoolingRate() const { return m_pending.coolingRate; }
        void SetDissipation(float dissipation) { m_pending.dissipation = dissipation; }
        // 0 disables confinement. Large values inject energy faster than dissipation removes it
        // and the field becomes noise, so this is clamped by the setter.
        void SetVorticity(float vorticity);
        // Direction is normalised by the setter; strength is the force applied toward it.
        void SetWind(float x, float y, float z, float strength);
        float GetWindStrength() const { return m_pending.windStrength; }
        float GetVorticity() const { return m_pending.vorticity; }

        // Jacobi iterations per step. More is closer to incompressible; GPU Gems 38
        // notes 40-80 for quality and that 20 already looks convincing for smoke.
        void SetJacobiIterations(uint32_t iterations);
        uint32_t GetJacobiIterations() const { return m_jacobiIterations; }

        // Allocates or resizes buffers. Call once per frame before the render graph
        // records anything that reads them, exactly as FluidHeightfieldSim does.
        bool PrepareFrame(GraphicsDevice& device);

        // Records one full solver step. Leaves the current state buffer in
        // NON_PIXEL_SHADER_RESOURCE so the raymarch pass can read it.
        bool Update(CommandList& cmdList, float dt);

        // GPU address of the state buffer the renderer should read this frame.
        D3D12_GPU_VIRTUAL_ADDRESS GetCurrentStateGpuVA() const;
        D3D12_GPU_VIRTUAL_ADDRESS GetUpdateCBGpuVA() const;

        uint32_t GetTotalCellCount() const;

        // Static obstacles: one bit per cell, bit index = cell index, word = index >> 5,
        // bit = index & 31 (BuildSimGridSolidMask in Renderer/Voxel/SimGridSolidMask.h). The
        // compute shader treats a set cell as a zero-velocity wall (GPU Gems 3 ch.30). words
        // must hold exactly (GetTotalCellCount() + 31) / 32 entries, and the buffers must already
        // be sized for the current grid (PrepareFrame has run since the last SetGridSize);
        // returns false otherwise. Blocks until the GPU is idle. A grid, origin or cell size
        // change drops the mask, since its bits describe the old cells. clearField=false keeps the
        // current smoke (for refreshing walls after objects moved).
        bool SetSolidMask(const uint32_t* words, size_t wordCount, bool clearField = true);
        void ClearSolidMask();

        // Copies the density of the current state buffer (StateCell::density, x fastest, then y, z) to
        // the CPU. Debug/one-shot path: waits for the GPU, records a copy into a readback buffer on the
        // graphics queue, and waits again. DX12-only like the rest of the sim. Empty on failure.
        bool ReadbackDensity(std::vector<float>& outDensity);

        // Per-frame, non-blocking variant: while on, Update() ends by copying the state buffer into a
        // GpuBufferReadbackRing, and ReadStreamedDensity() returns the density of the newest copy that is
        // READ_LAG frames old (false until one exists). outRecord numbers the copies so a caller can skip
        // one it has seen. Turning it off waits for the GPU and frees the ring. DX12-only.
        void SetDensityStreaming(bool enabled);
        bool IsDensityStreaming() const { return m_densityStreaming; }
        bool ReadStreamedDensity(std::vector<float>& outDensity, uint64_t& outRecord) const;
        bool IsSolidActive() const { return m_pending.solidActive != 0u; }
        float GetOriginX() const { return m_pending.originX; }
        float GetOriginY() const { return m_pending.originY; }
        float GetOriginZ() const { return m_pending.originZ; }
        float GetCellSize() const { return m_pending.cellSize; }

    private:
        // The state buffer alternates; pressure needs its own pair for Jacobi.
        Resource* GetStateResource(int index);
        Resource* GetPressureResource(int index);
        Resource* GetTemperatureResource(int index);

        bool CreateBuffers(GraphicsDevice& device, uint32_t totalCells);
        bool CreateSolidMaskBuffer(GraphicsDevice& device, uint32_t totalCells);
        bool CreatePipelines(GraphicsDevice& device);
        void FillUpdateCB(float dt, SmokeUpdateCBData& out) const;

        GraphicsDevice* m_device = nullptr;
        bool m_initialized = false;
        bool m_enabled = false;

        SmokeUpdateCBData m_pending{};
        uint32_t m_jacobiIterations = 24u;

        Resource m_state[2];
        Resource m_pressure[2];
        // Temperature is advected but not projected, so unlike the state pair it genuinely
        // ping-pongs: Advect reads [m_temperatureReadIndex] and writes the other, then the index flips.
        Resource m_temperature[2];
        int m_temperatureReadIndex = 0;
        Resource m_constantBuffer;
        void* m_cbMapped = nullptr;
        // Upload heap, persistently mapped, always bound at t2 so the root SRV never carries a
        // null address; all-zero unless SetSolidMask filled it.
        Resource m_solidMask;
        uint32_t* m_solidMaskMapped = nullptr;
        size_t m_solidMaskWords = 0u;
        uint32_t m_bufferCapacity = 0u;
        int m_readIndex = 0;
        // Buffers come back from CreateCommittedResource with undefined content, and advection feeds
        // on its own output -- one garbage float becomes a permanent NaN. Cleared once per allocation.
        bool m_needsClear = false;
        GpuBufferReadbackRing m_densityStream;
        bool m_densityStreaming = false;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_clearPso;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_advectPso;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_divergencePso;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_jacobiPso;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_projectPso;
        bool m_psoMissingLogged = false;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
