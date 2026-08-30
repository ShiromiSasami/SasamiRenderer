#pragma once

#include "Renderer/RHI/GraphicsDevice.h"

#include <cstdint>
#include <string>
#include <wrl.h>

namespace SasamiRenderer
{
    // =========================================================================
    // ParticleSystem
    //
    // GPU-driven fixed-capacity particle simulation (single fountain-style
    // emitter, gravity + linear drag integration). Unlike FluidHeightfieldSim's
    // ping-pong buffers, this uses a single RWStructuredBuffer<Particle> that a
    // single compute dispatch both emits into and integrates in place:
    //
    //   - Emission uses a host-tracked ring cursor (m_emitCursor): each Update()
    //     computes how many particles to spawn this frame from an accumulated
    //     emission-rate*dt budget, and the shader respawns particle slots in the
    //     range [emitCursor, emitCursor + emitCount) (mod capacity).
    //   - All other slots with life > 0 are integrated (gravity, drag, lifetime
    //     decay); slots with life <= 0 are left dead (undrawn — ParticleRenderPass
    //     clips them via a degenerate SV_POSITION in the vertex shader).
    //
    // No atomics or CPU->GPU per-particle upload are needed: the ring window is
    // entirely deterministic from (emitCursor, emitCount, capacity).
    //
    // Integration: GetParticleBufferGpuVA() -> bind as an inline SRV (t0) in
    // ParticleBillboard_VS for per-instance position/size/color.
    // =========================================================================
    class ParticleSystem
    {
    public:
        // Mirrors Particle in ParticleUpdate_CS.hlsl / ParticleBillboard_VS.hlsl byte-for-byte.
        struct Particle
        {
            float    position[3] = { 0.0f, 0.0f, 0.0f };
            float    life = 0.0f;       // remaining seconds; <= 0 means dead
            float    velocity[3] = { 0.0f, 0.0f, 0.0f };
            float    maxLife = 0.0f;
            float    color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            float    size = 0.0f;
            float    pad[3] = { 0.0f, 0.0f, 0.0f };
        };
        static_assert(sizeof(Particle) == 64u);

        // Mirrors ParticleUpdateCB (b0) in ParticleUpdate_CS.hlsl byte-for-byte.
        struct alignas(256) ParticleUpdateCBData
        {
            float    deltaTime = 0.0f;
            float    gravity = -9.8f;
            float    drag = 0.1f;
            uint32_t capacity = 0u;

            uint32_t emitCursor = 0u;
            uint32_t emitCount = 0u;
            float    lifeMin = 1.0f;
            float    lifeMax = 2.0f;

            float    sizeMin = 0.05f;
            float    sizeMax = 0.15f;
            float    emitSpeed = 3.0f;
            float    emitSpread = 0.3f;   // 0..1 fraction of a hemisphere cone around +Y

            float    emitOriginX = 0.0f;
            float    emitOriginY = 0.0f;
            float    emitOriginZ = 0.0f;
            float    emitRadius = 0.25f;

            float    colorStart[4] = { 1.0f, 0.9f, 0.4f, 1.0f };
            float    colorEnd[4]   = { 1.0f, 0.2f, 0.05f, 0.0f };

            uint32_t frameSeed = 0u;
            float    pad1[3] = { 0.0f, 0.0f, 0.0f };
        };
        static_assert(sizeof(ParticleUpdateCBData) == 256u);

        ParticleSystem();
        ~ParticleSystem();

        ParticleSystem(const ParticleSystem&) = delete;
        ParticleSystem& operator=(const ParticleSystem&) = delete;

        // Call once at startup.
        bool Initialize(IRHIDevice& device);

        // Configure emitter. All safe to call before or after Initialize().
        void SetCapacity(uint32_t capacity); // triggers a buffer reallocation (all particles reset dead) on next Update()
        void SetEmissionRate(float particlesPerSecond) { m_emissionRate = (particlesPerSecond >= 0.0f) ? particlesPerSecond : 0.0f; }
        void SetGravity(float g) { m_gravity = g; }
        void SetDrag(float d) { m_drag = (d >= 0.0f) ? d : 0.0f; }
        void SetLifeRange(float lifeMin, float lifeMax) { m_lifeMin = lifeMin; m_lifeMax = (lifeMax >= lifeMin) ? lifeMax : lifeMin; }
        void SetSizeRange(float sizeMin, float sizeMax) { m_sizeMin = sizeMin; m_sizeMax = (sizeMax >= sizeMin) ? sizeMax : sizeMin; }
        void SetEmitSpeed(float speed) { m_emitSpeed = speed; }
        void SetEmitSpread(float spread01) { m_emitSpread = (spread01 < 0.0f) ? 0.0f : (spread01 > 1.0f ? 1.0f : spread01); }
        void SetEmitOrigin(float x, float y, float z) { m_emitOriginX = x; m_emitOriginY = y; m_emitOriginZ = z; }
        void SetEmitRadius(float r) { m_emitRadius = (r >= 0.0f) ? r : 0.0f; }
        void SetColorStart(float r, float g, float b, float a) { m_colorStart[0] = r; m_colorStart[1] = g; m_colorStart[2] = b; m_colorStart[3] = a; }
        void SetColorEnd(float r, float g, float b, float a) { m_colorEnd[0] = r; m_colorEnd[1] = g; m_colorEnd[2] = b; m_colorEnd[3] = a; }

        float GetEmissionRate() const { return m_emissionRate; }
        float GetGravity() const { return m_gravity; }
        float GetDrag() const { return m_drag; }
        uint32_t GetCapacity() const { return m_capacity; }

        bool GetEnabled() const { return m_enabled; }
        void SetEnabled(bool e) { m_enabled = e; }

        // Reallocates the particle buffer if the capacity changed since the last
        // allocation. Must be called before the render graph records any reads of
        // GetParticleBufferGpuVA() into the frame's command list (i.e. before
        // RegisterPassesToRenderGraph/Execute), so a reallocation never destroys a
        // buffer already referenced by an open, unexecuted command list.
        // No-op (returns true) if disabled, uninitialized, or capacity already matches.
        bool PrepareFrame(IRHIDevice& device);

        // Dispatches the emit+integrate compute update, advancing the sim by dt.
        // No-op (returns true) if disabled or the pipeline failed to compile.
        bool Update(float dt, IRHIDevice& device, CommandList& cmdList);

        // GPU VA of the particle buffer (RWStructuredBuffer<Particle> / StructuredBuffer<Particle>).
        // Valid after Initialize(); ParticleRenderPass binds this directly (t0) for per-instance
        // vertex fetch, same one-frame-lag convention as FluidHeightfieldSim::GetCurrentHeightGpuVA().
        D3D12_GPU_VIRTUAL_ADDRESS GetParticleBufferGpuVA() const;

        bool IsInitialized() const { return m_initialized; }
        const std::string& GetLastPipelineError() const { return m_lastPipelineError; }

    private:
        bool CreatePipeline(IRHIDevice& device);
        bool AllocateBuffer(IRHIDevice& device);
        Resource* GetBufferResource() const;
        Resource* GetConstantBufferResource() const;
        void FillUpdateCB(float dt, uint32_t emitCount, ParticleUpdateCBData& out);

        uint32_t m_capacity = 4096u;
        float    m_emissionRate = 200.0f; // particles per second
        float    m_emitAccumulator = 0.0f;
        uint32_t m_emitCursor = 0u;
        uint32_t m_frameCounter = 0u;

        float m_gravity = -9.8f;
        float m_drag    = 0.1f;
        float m_lifeMin = 1.0f;
        float m_lifeMax = 2.0f;
        float m_sizeMin = 0.05f;
        float m_sizeMax = 0.15f;
        float m_emitSpeed  = 3.0f;
        float m_emitSpread = 0.3f;
        float m_emitOriginX = 0.0f;
        float m_emitOriginY = 0.0f;
        float m_emitOriginZ = 0.0f;
        float m_emitRadius  = 0.25f;
        float m_colorStart[4] = { 1.0f, 0.9f, 0.4f, 1.0f };
        float m_colorEnd[4]   = { 1.0f, 0.2f, 0.05f, 0.0f };
        bool  m_enabled = true;

        // Single buffer: RWStructuredBuffer<Particle> during Dispatch, StructuredBuffer<Particle>
        // (steady-state combined read state) for ParticleRenderPass's vertex-shader SRV fetch.
        Resource        m_buffer;
        RhiBufferHandle m_bufferHandle{};
        Resource*       m_bufferCompat = nullptr;
        uint32_t        m_bufferCapacity = 0u; // allocated element count

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
