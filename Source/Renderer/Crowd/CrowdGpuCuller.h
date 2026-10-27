#pragma once

#include "Renderer/Crowd/CrowdVatGpuAsset.h"
#include "Renderer/RHI/RhiDevice.h"

#include <cstdint>
#include <string>

namespace SasamiEngine::Render::Crowd
{
    // Tunables of the GPU crowd culling (TODO stage 8d). Pattern switches stay runtime toggles.
    struct CrowdCullSettings
    {
        bool  enabled = true;          // false: CrowdRenderPass falls back to one DrawIndexed of every agent
        bool  frustum = true;          // false: skip the plane test, only pick LODs
        // Camera distance (metres, to the agent's sphere centre) at which LOD i+1 starts.
        float lodDistance[CROWD_MAX_LODS - 1] = { 20.0f, 50.0f };
        int   forceLod = -1;           // >= 0 pins every agent to that LOD (clamped to LodCount()-1)
    };

    // What the culler needs from the frame and the crowd. viewProj is the row-major camera
    // matrix the crowd VS multiplies with (mul(float4(p,1), M), D3D clip z in [0,w], reverse-Z).
    struct CrowdCullInputs
    {
        const float* viewProj = nullptr;   // 16 floats, row-major
        const float* cameraPos = nullptr;  // 3 floats
        float modelScale = 1.0f;
        float modelYOffset = 0.0f;
        uint32_t agentCount = 0;
        RhiBufferHandle agentBuffer{};     // in UnorderedAccess; read as root UAV, not written
        const CrowdVatGpuAsset* asset = nullptr;
    };

    // GPU frustum culling + distance LOD selection for the crowd, writing D3D12 DrawIndexed
    // indirect arguments (one record per LOD) and per-LOD compacted visible agent lists.
    // Shaders/Compute/Crowd/CrowdCull_CS.hlsl: CSReset (1 thread) writes
    // {lod.indexCount, 0, lod.firstIndex, 0, 0} per LOD, then CSCull (64 threads per group, one per
    // agent) tests the agent's bounding sphere against the 6 planes extracted from viewProj
    // (Gribb & Hartmann 2001), picks the LOD from the camera distance, InterlockedAdds the LOD's
    // InstanceCount and stores the agent index into visible[lod * capacity + slot].
    // References: Wihlidal, "Optimizing the Graphics Pipeline with Compute" (GDC 2016);
    // Microsoft D3D12ExecuteIndirect sample; GPU Gems 3 ch.2 (crowd LOD).
    // Root layout (inline root descriptors only, so the frame's descriptor heap is untouched):
    //   t0 root SRV params (ring slot), u0 root UAV agents (read-only use), u1 root UAV args (raw),
    //   u2 root UAV visible (StructuredBuffer<uint>).
    // Resource states: both buffers rest in UnorderedAccess. Record leaves args in
    // IndirectArgument and visible in ShaderResource; RestoreAfterDraw puts them back.
    // DX12 only (needs GPU virtual addresses and ExecuteIndirect).
    class CrowdGpuCuller
    {
    public:
        static constexpr uint32_t kArgsStrideBytes = 20; // D3D12_DRAW_INDEXED_ARGUMENTS
        static constexpr uint32_t kParamsRingSize = 8;   // assumes < 8 frames in flight (unverified)
        static constexpr uint32_t kParamsStrideBytes = 256;

        bool Initialize(IRhiDevice& device, std::string& outError);
        bool IsInitialized() const { return m_device != nullptr; }
        void Shutdown();

        // (Re)creates the visible buffer for at least agentCount agents per LOD. Call only while
        // the GPU is idle (CrowdSystem::Spawn does it after its WaitForRhiIdle).
        bool EnsureCapacity(uint32_t agentCount, std::string& outError);
        uint32_t Capacity() const { return m_capacity; }

        // Records reset + cull into the frame encoder (no submit). Fails without recording when
        // capacity < inputs.agentCount or the asset has no LODs. If the previous Record was never
        // followed by RestoreAfterDraw, it restores the states first.
        bool Record(IRhiCommandEncoder& encoder, const CrowdCullInputs& inputs,
                    const CrowdCullSettings& settings, std::string& outError);
        // Puts args / visible back into UnorderedAccess. No-op unless Record succeeded since.
        void RestoreAfterDraw(IRhiCommandEncoder& encoder);
        bool IsInDrawState() const { return m_inDrawState; }

        RhiBufferHandle ArgsBuffer() const { return m_argsBuffer; }
        // LOD i's list starts at VisibleAddress() + i * Capacity() * 4.
        RhiGpuAddress VisibleAddress() const { return m_visibleAddress; }
        uint32_t RecordedLodCount() const { return m_recordedLodCount; }

        // Debug only: waits for the GPU, copies the args back and returns each LOD's InstanceCount
        // (entries >= RecordedLodCount() are 0). Blocking.
        bool ReadVisibleCounts(uint32_t outCounts[CROWD_MAX_LODS], std::string& outError);

    private:
        IRhiDevice* m_device = nullptr;
        RhiPipelineLayoutHandle m_layout{};        // bindable
        RhiPipelineHandle m_resetPipeline{};       // bindable
        RhiPipelineHandle m_cullPipeline{};        // bindable
        RhiBufferHandle m_argsBuffer{};            // GpuOnly, CROWD_MAX_LODS * 20 bytes, raw
        RhiGpuAddress m_argsAddress = 0;
        RhiBufferHandle m_visibleBuffer{};         // GpuOnly, CROWD_MAX_LODS * capacity uint
        RhiGpuAddress m_visibleAddress = 0;
        RhiBufferHandle m_paramsRing{};            // CpuToGpu, kParamsRingSize * kParamsStrideBytes
        RhiGpuAddress m_paramsRingAddress = 0;
        uint32_t m_paramsRingCursor = 0;
        uint32_t m_capacity = 0;
        uint32_t m_recordedLodCount = 0;
        bool m_inDrawState = false;
    };
}

namespace SasamiEngine { using namespace Render; }
