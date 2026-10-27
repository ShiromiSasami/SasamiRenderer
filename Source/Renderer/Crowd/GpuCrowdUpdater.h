#pragma once

#include "Renderer/Crowd/CrowdStore.h"
#include "Renderer/RHI/RhiDevice.h"

#include <string>

namespace SasamiEngine::Render::Crowd
{
    // GPU counterpart of StepCrowdRange (CrowdSimulation.h); Shaders/Compute/Crowd/CrowdUpdate_CS.hlsl,
    // one thread per agent. The agent state stays resident on the GPU between Step calls, which is
    // what the instanced crowd pass (TODO stage 8c) will read; Upload / Readback exist for setup and
    // for validation against the CPU path.
    // Built only through IRhiDevice / IRhiCommandEncoder like GpuSurfaceVoxelizer; the shader is
    // compiled to DXIL, so Initialize fails on non-DX12 backends.
    class GpuCrowdUpdater
    {
    public:
        bool Initialize(IRhiDevice& device, std::string& outError);
        bool IsInitialized() const { return m_device != nullptr; }
        void Shutdown();

        // (Re)creates the GPU agent buffer sized to store.Count() and uploads the state.
        bool Upload(const CrowdStore& store, std::string& outError);
        // Records `steps` dispatches (with UAV barriers between them), submits and waits.
        bool Step(const CrowdSimParams& params, uint32_t steps, std::string& outError);
        // Records one dispatch into a frame encoder the caller submits (TODO stage 8d), with no
        // submit and no wait. Uses a second pipeline layout with inline root descriptors (t0 root
        // SRV params, u0 root UAV agents) so the frame's descriptor heap is not touched, and a
        // ring of kParamsRingSize param slots so an in-flight frame's params are not overwritten
        // (assumes fewer than kParamsRingSize frames in flight; unverified). Same shader, so the
        // result is bit-identical to Step. Leaves the agent buffer in UnorderedAccess after a UAV
        // barrier. Returns false (and records nothing) without agents.
        bool RecordStep(IRhiCommandEncoder& encoder, const CrowdSimParams& params, std::string& outError);
        // Copies the GPU state back into store (must already have Count() == uploaded count).
        bool Readback(CrowdStore& store, std::string& outError);

        uint32_t AgentCount() const { return m_agentCount; }
        // The resident agent buffer (StructuredBuffer<CrowdAgentGpu>, 32-byte stride). It rests in
        // UnorderedAccess between calls; a reader transitions it and restores that state. Upload
        // recreates it, so re-query the handle every frame.
        RhiBufferHandle GetAgentBuffer() const { return m_agentBuffer; }

    private:
        IRhiDevice* m_device = nullptr;
        RhiPipelineLayoutHandle m_bindableLayout{};
        RhiPipelineHandle m_bindablePipeline{};
        RhiDescriptorAllocation m_descriptors{};
        RhiBufferHandle m_agentBuffer{};
        // The buffer replaced by the last Upload. The frame command list that is still open may
        // reference it (CrowdSystem::Update runs after the graph is recorded), so it is destroyed
        // one Upload later, after the caller's WaitForRhiIdle has drained that frame.
        RhiBufferHandle m_retiredAgentBuffer{};
        RhiBufferHandle m_paramsBuffer{};
        uint32_t m_agentCount = 0;

        static constexpr uint32_t kParamsRingSize = 8;
        RhiPipelineLayoutHandle m_inlineLayout{};   // bindable
        RhiPipelineHandle m_inlinePipeline{};       // bindable
        RhiBufferHandle m_paramsRing{};             // CpuToGpu, kParamsRingSize * 64 bytes
        RhiGpuAddress m_paramsRingAddress = 0;
        uint32_t m_paramsRingCursor = 0;
    };
}

namespace SasamiEngine { using namespace Render; }
