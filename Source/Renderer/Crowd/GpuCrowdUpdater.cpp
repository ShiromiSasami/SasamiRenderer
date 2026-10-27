#include "Renderer/Crowd/GpuCrowdUpdater.h"

#include "Renderer/Resources/ShaderCompilationService.h"
#include "Renderer/Voxel/GpuVoxelBuffers.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Crowd
{
    namespace
    {
        using namespace Voxel::GpuVoxelBuffers;

        constexpr uint32_t kThreadsPerGroup = 64;
        constexpr uint32_t kMaxGroupsX = 65535;
        constexpr uint32_t kBindingCount = 2;

        // Mirrors CrowdAgent in CrowdUpdate_CS.hlsl (structured element, 32 bytes).
        struct CrowdAgentGpu
        {
            float posX;
            float posY;
            float posZ;
            float animTime;
            float velX;
            float velZ;
            float animRate;
            uint32_t clip;
        };
        static_assert(sizeof(CrowdAgentGpu) == 32, "CrowdAgentGpu must match the HLSL CrowdAgent layout");

        // Mirrors CrowdParams in CrowdUpdate_CS.hlsl (structured element, 64 bytes).
        struct CrowdParamsGpu
        {
            float boundsMinX;
            float boundsMinZ;
            float boundsMaxX;
            float boundsMaxZ;
            float deltaTime;
            uint32_t agentCount;
            uint32_t clipCount;
            uint32_t threadsPerRow;
            float clipDurations[CROWD_MAX_CLIPS];
        };
        static_assert(sizeof(CrowdParamsGpu) == 64, "CrowdParamsGpu must match the HLSL CrowdParams layout");

        struct CrowdDispatchSize
        {
            uint32_t groupsX = 0;
            uint32_t groupsY = 0;
        };

        CrowdParamsGpu BuildCrowdParams(const CrowdSimParams& params, uint32_t agentCount, CrowdDispatchSize& outDispatch)
        {
            const uint32_t groups = (agentCount + kThreadsPerGroup - 1) / kThreadsPerGroup;
            outDispatch.groupsX = (std::min)(groups, kMaxGroupsX);
            outDispatch.groupsY = (groups + outDispatch.groupsX - 1) / outDispatch.groupsX;

            CrowdParamsGpu gpuParams{};
            gpuParams.boundsMinX = params.boundsMinX;
            gpuParams.boundsMinZ = params.boundsMinZ;
            gpuParams.boundsMaxX = params.boundsMaxX;
            gpuParams.boundsMaxZ = params.boundsMaxZ;
            gpuParams.deltaTime = params.deltaTime;
            gpuParams.agentCount = agentCount;
            gpuParams.clipCount = params.clipCount;
            gpuParams.threadsPerRow = outDispatch.groupsX * kThreadsPerGroup;
            std::memcpy(gpuParams.clipDurations, params.clipDurations, sizeof(gpuParams.clipDurations));
            return gpuParams;
        }
    }

    bool GpuCrowdUpdater::Initialize(IRhiDevice& device, std::string& outError)
    {
        if (m_device != nullptr)
        {
            return true;
        }
        if (device.GetRhiBackendApi() != RhiBackendApi::DirectX12)
        {
            outError = "GpuCrowdUpdater: only the D3D12 backend is supported";
            return false;
        }

        std::vector<uint8_t> bytecode;
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc(
                "Crowd", L"Compute/Crowd/CrowdUpdate_CS.hlsl", "CSMain", "cs_6_0", bytecode)
            || bytecode.empty())
        {
            outError = "GpuCrowdUpdater: failed to compile CrowdUpdate_CS.hlsl";
            return false;
        }

        RhiShaderModuleDesc shaderDesc{};
        shaderDesc.bytecode = bytecode.data();
        shaderDesc.bytecodeSize = bytecode.size();
        shaderDesc.entryPoint = "CSMain";
        shaderDesc.stage = RhiShaderStageFlags::Compute;
        if (!device.CreateRhiShaderModule(shaderDesc).IsValid())
        {
            outError = "GpuCrowdUpdater: failed to create the compute shader module";
            return false;
        }

        RhiBindingRangeDesc bindings[kBindingCount]{};
        for (uint32_t i = 0; i < kBindingCount; ++i)
        {
            bindings[i].type = (i < 1) ? RhiBindingType::ShaderResource : RhiBindingType::UnorderedAccess;
            bindings[i].visibility = RhiShaderStageFlags::Compute;
            bindings[i].baseRegister = 0;
            bindings[i].registerSpace = 0;
            bindings[i].descriptorCount = 1;
            bindings[i].inlineRootDescriptor = false;
            bindings[i].bufferResource = true;
        }

        RhiPipelineLayoutDesc layoutDesc{};
        layoutDesc.bindings = bindings;
        layoutDesc.bindingCount = kBindingCount;
        layoutDesc.allowInputAssembler = false;
        const RhiPipelineLayoutHandle layout = device.CreateRhiPipelineLayout(layoutDesc);
        if (!layout.IsValid())
        {
            outError = "GpuCrowdUpdater: failed to create the pipeline layout";
            return false;
        }

        RhiComputePipelineDesc pipelineDesc{};
        pipelineDesc.layout = layout;
        pipelineDesc.shader = shaderDesc;
        const RhiPipelineHandle pipeline = device.CreateRhiComputePipeline(pipelineDesc);
        if (!pipeline.IsValid())
        {
            outError = "GpuCrowdUpdater: failed to create the compute pipeline";
            return false;
        }

        const RhiPipelineLayoutHandle bindableLayout = device.GetBindablePipelineLayoutHandle(layout);
        const RhiPipelineHandle bindablePipeline = device.GetBindablePipelineHandle(pipeline);
        if (!bindableLayout.IsValid() || !bindablePipeline.IsValid())
        {
            outError = "GpuCrowdUpdater: failed to resolve bindable pipeline handles";
            return false;
        }

        const RhiDescriptorAllocation descriptors =
            device.AllocateRhiDescriptors(RhiDescriptorHeapType::CbvSrvUav, kBindingCount, true);
        if (!descriptors.IsValid() || descriptors.heap.id == 0)
        {
            outError = "GpuCrowdUpdater: failed to allocate shader-visible descriptors";
            return false;
        }

        RhiBindingRangeDesc inlineBindings[kBindingCount]{};
        for (uint32_t i = 0; i < kBindingCount; ++i)
        {
            inlineBindings[i].type = (i < 1) ? RhiBindingType::ShaderResource : RhiBindingType::UnorderedAccess;
            inlineBindings[i].visibility = RhiShaderStageFlags::Compute;
            inlineBindings[i].baseRegister = 0;
            inlineBindings[i].registerSpace = 0;
            inlineBindings[i].descriptorCount = 1;
            inlineBindings[i].inlineRootDescriptor = true;
            inlineBindings[i].bufferResource = true;
        }

        RhiPipelineLayoutDesc inlineLayoutDesc{};
        inlineLayoutDesc.bindings = inlineBindings;
        inlineLayoutDesc.bindingCount = kBindingCount;
        inlineLayoutDesc.allowInputAssembler = false;
        const RhiPipelineLayoutHandle inlineLayout = device.CreateRhiPipelineLayout(inlineLayoutDesc);
        if (!inlineLayout.IsValid())
        {
            outError = "GpuCrowdUpdater: failed to create the inline pipeline layout";
            return false;
        }

        RhiComputePipelineDesc inlinePipelineDesc{};
        inlinePipelineDesc.layout = inlineLayout;
        inlinePipelineDesc.shader = shaderDesc;
        const RhiPipelineHandle inlinePipeline = device.CreateRhiComputePipeline(inlinePipelineDesc);
        if (!inlinePipeline.IsValid())
        {
            outError = "GpuCrowdUpdater: failed to create the inline compute pipeline";
            return false;
        }

        const RhiPipelineLayoutHandle bindableInlineLayout = device.GetBindablePipelineLayoutHandle(inlineLayout);
        const RhiPipelineHandle bindableInlinePipeline = device.GetBindablePipelineHandle(inlinePipeline);
        if (!bindableInlineLayout.IsValid() || !bindableInlinePipeline.IsValid())
        {
            outError = "GpuCrowdUpdater: failed to resolve bindable inline pipeline handles";
            return false;
        }

        const RhiBufferHandle paramsRing = device.CreateRhiBuffer(
            MakeBufferDesc(static_cast<uint64_t>(kParamsRingSize) * sizeof(CrowdParamsGpu), sizeof(CrowdParamsGpu),
                           RhiBufferUsageFlags::ShaderResource | RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::CpuToGpu, RhiResourceState::ShaderResource),
            nullptr);
        if (!paramsRing.IsValid())
        {
            outError = "GpuCrowdUpdater: failed to create the params ring";
            return false;
        }
        const RhiGpuAddress paramsRingAddress = device.GetRhiBufferGpuAddress(paramsRing);
        if (paramsRingAddress == 0)
        {
            device.DestroyRhiResource(paramsRing);
            outError = "GpuCrowdUpdater: the params ring has no GPU address";
            return false;
        }

        m_bindableLayout = bindableLayout;
        m_bindablePipeline = bindablePipeline;
        m_inlineLayout = bindableInlineLayout;
        m_inlinePipeline = bindableInlinePipeline;
        m_paramsRing = paramsRing;
        m_paramsRingAddress = paramsRingAddress;
        m_paramsRingCursor = 0;
        m_descriptors = descriptors;
        m_device = &device;
        return true;
    }

    void GpuCrowdUpdater::Shutdown()
    {
        if (m_device != nullptr)
        {
            if (m_agentBuffer.IsValid())
            {
                m_device->DestroyRhiResource(m_agentBuffer);
            }
            if (m_retiredAgentBuffer.IsValid())
            {
                m_device->DestroyRhiResource(m_retiredAgentBuffer);
            }
            if (m_paramsBuffer.IsValid())
            {
                m_device->DestroyRhiResource(m_paramsBuffer);
            }
            if (m_paramsRing.IsValid())
            {
                m_device->DestroyRhiResource(m_paramsRing);
            }
        }
        m_device = nullptr;
        m_bindableLayout = {};
        m_bindablePipeline = {};
        m_inlineLayout = {};
        m_inlinePipeline = {};
        m_paramsRing = {};
        m_paramsRingAddress = 0;
        m_paramsRingCursor = 0;
        m_descriptors = {};
        m_agentBuffer = {};
        m_retiredAgentBuffer = {};
        m_paramsBuffer = {};
        m_agentCount = 0;
    }

    bool GpuCrowdUpdater::Upload(const CrowdStore& store, std::string& outError)
    {
        if (m_device == nullptr)
        {
            outError = "GpuCrowdUpdater: not initialized";
            return false;
        }

        const uint32_t count = store.Count();
        if (count == 0)
        {
            outError = "GpuCrowdUpdater: the crowd store is empty";
            return false;
        }
        if (store.posY.size() != count || store.posZ.size() != count || store.velX.size() != count
            || store.velZ.size() != count || store.animTime.size() != count
            || store.animRate.size() != count || store.clip.size() != count)
        {
            outError = "GpuCrowdUpdater: the crowd store arrays have mismatched sizes";
            return false;
        }

        std::vector<CrowdAgentGpu> agents(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            CrowdAgentGpu& agent = agents[i];
            agent.posX = store.posX[i];
            agent.posY = store.posY[i];
            agent.posZ = store.posZ[i];
            agent.animTime = store.animTime[i];
            agent.velX = store.velX[i];
            agent.velZ = store.velZ[i];
            agent.animRate = store.animRate[i];
            agent.clip = store.clip[i];
        }

        if (m_retiredAgentBuffer.IsValid())
        {
            m_device->DestroyRhiResource(m_retiredAgentBuffer);
        }
        m_retiredAgentBuffer = m_agentBuffer;
        m_agentBuffer = {};
        m_agentCount = 0;

        const uint64_t agentBytes = static_cast<uint64_t>(count) * sizeof(CrowdAgentGpu);
        const RhiBufferHandle agentBuffer = m_device->CreateRhiBuffer(
            MakeBufferDesc(agentBytes, sizeof(CrowdAgentGpu),
                           RhiBufferUsageFlags::UnorderedAccess | RhiBufferUsageFlags::CopySource
                               | RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::GpuOnly, RhiResourceState::UnorderedAccess),
            agents.data());
        if (!agentBuffer.IsValid())
        {
            outError = "GpuCrowdUpdater: failed to create the agent buffer";
            return false;
        }

        const uint64_t uavSlot = m_descriptors.cpu.ptr + m_descriptors.increment;
        if (!m_device->CreateRhiBufferUnorderedAccessView(
                agentBuffer, MakeStructuredView(agentBytes, sizeof(CrowdAgentGpu)), { uavSlot }))
        {
            m_device->DestroyRhiResource(agentBuffer);
            outError = "GpuCrowdUpdater: failed to create the agent buffer view";
            return false;
        }

        m_agentBuffer = agentBuffer;
        m_agentCount = count;
        return true;
    }

    bool GpuCrowdUpdater::Step(const CrowdSimParams& params, uint32_t steps, std::string& outError)
    {
        if (m_device == nullptr)
        {
            outError = "GpuCrowdUpdater: not initialized";
            return false;
        }
        if (steps == 0)
        {
            return true;
        }
        if (!m_agentBuffer.IsValid() || m_agentCount == 0)
        {
            outError = "GpuCrowdUpdater: no agents uploaded";
            return false;
        }

        CrowdDispatchSize dispatchSize{};
        const CrowdParamsGpu gpuParams = BuildCrowdParams(params, m_agentCount, dispatchSize);
        const uint32_t groupsX = dispatchSize.groupsX;
        const uint32_t groupsY = dispatchSize.groupsY;

        if (!m_paramsBuffer.IsValid())
        {
            const RhiBufferHandle paramsBuffer = m_device->CreateRhiBuffer(
                MakeBufferDesc(sizeof(CrowdParamsGpu), sizeof(CrowdParamsGpu),
                               RhiBufferUsageFlags::ShaderResource | RhiBufferUsageFlags::Structured,
                               RhiMemoryUsage::CpuToGpu, RhiResourceState::ShaderResource),
                &gpuParams);
            if (!paramsBuffer.IsValid())
            {
                outError = "GpuCrowdUpdater: failed to create the params buffer";
                return false;
            }
            if (!m_device->CreateRhiBufferShaderResourceView(
                    paramsBuffer, MakeStructuredView(sizeof(CrowdParamsGpu), sizeof(CrowdParamsGpu)),
                    { m_descriptors.cpu.ptr }))
            {
                m_device->DestroyRhiResource(paramsBuffer);
                outError = "GpuCrowdUpdater: failed to create the params buffer view";
                return false;
            }
            m_paramsBuffer = paramsBuffer;
        }
        else if (!m_device->UpdateRhiBuffer(m_paramsBuffer, 0, &gpuParams, sizeof(CrowdParamsGpu)))
        {
            outError = "GpuCrowdUpdater: failed to update the params buffer";
            return false;
        }

        std::unique_ptr<IRhiCommandEncoder> encoder = m_device->CreateCommandEncoder(RhiQueueType::Graphics);
        if (!encoder)
        {
            outError = "GpuCrowdUpdater: failed to create a command encoder";
            return false;
        }

        encoder->BeginDebugLabel("Crowd.Update");
        encoder->SetDescriptorHeap(m_descriptors.heap);
        encoder->SetComputePipelineLayout(m_bindableLayout);
        encoder->SetComputePipeline(m_bindablePipeline);
        for (uint32_t i = 0; i < kBindingCount; ++i)
        {
            encoder->SetComputeDescriptorTable(
                i, RhiGpuDescriptorHandle{ m_descriptors.gpu.ptr + i * m_descriptors.increment });
        }

        RhiDispatchDesc dispatch{};
        dispatch.groupCountX = groupsX;
        dispatch.groupCountY = groupsY;
        dispatch.groupCountZ = 1;
        for (uint32_t step = 0; step < steps; ++step)
        {
            encoder->Dispatch(dispatch);
            encoder->UavBarrier(&m_agentBuffer, 1);
        }
        encoder->EndDebugLabel();

        if (!m_device->SubmitCommandEncoder(*encoder, RhiQueueType::Graphics))
        {
            outError = "GpuCrowdUpdater: failed to submit the command encoder";
            return false;
        }
        if (!m_device->WaitForRhiIdle())
        {
            outError = "GpuCrowdUpdater: waiting for the GPU failed";
            return false;
        }
        return true;
    }

    bool GpuCrowdUpdater::RecordStep(IRhiCommandEncoder& encoder, const CrowdSimParams& params, std::string& outError)
    {
        if (m_device == nullptr)
        {
            outError = "GpuCrowdUpdater: not initialized";
            return false;
        }
        if (!m_agentBuffer.IsValid() || m_agentCount == 0)
        {
            outError = "GpuCrowdUpdater: no agents uploaded";
            return false;
        }
        if (!m_paramsRing.IsValid() || m_paramsRingAddress == 0)
        {
            outError = "GpuCrowdUpdater: the params ring is unavailable";
            return false;
        }
        const RhiGpuAddress agentAddress = m_device->GetRhiBufferGpuAddress(m_agentBuffer);
        if (agentAddress == 0)
        {
            outError = "GpuCrowdUpdater: the agent buffer has no GPU address";
            return false;
        }

        CrowdDispatchSize dispatchSize{};
        const CrowdParamsGpu gpuParams = BuildCrowdParams(params, m_agentCount, dispatchSize);

        const uint32_t slot = m_paramsRingCursor;
        const uint64_t slotOffset = static_cast<uint64_t>(slot) * sizeof(CrowdParamsGpu);
        if (!m_device->UpdateRhiBuffer(m_paramsRing, slotOffset, &gpuParams, sizeof(CrowdParamsGpu)))
        {
            outError = "GpuCrowdUpdater: failed to update the params ring";
            return false;
        }
        m_paramsRingCursor = (slot + 1) % kParamsRingSize;

        encoder.BeginDebugLabel("Crowd.Step");
        encoder.SetComputePipelineLayout(m_inlineLayout);
        encoder.SetComputePipeline(m_inlinePipeline);
        encoder.SetComputeShaderResourceView(0, m_paramsRingAddress + slotOffset);
        encoder.SetComputeUnorderedAccessView(1, agentAddress);

        RhiDispatchDesc dispatch{};
        dispatch.groupCountX = dispatchSize.groupsX;
        dispatch.groupCountY = dispatchSize.groupsY;
        dispatch.groupCountZ = 1;
        encoder.Dispatch(dispatch);
        encoder.UavBarrier(&m_agentBuffer, 1);
        encoder.EndDebugLabel();
        return true;
    }

    bool GpuCrowdUpdater::Readback(CrowdStore& store, std::string& outError)
    {
        if (m_device == nullptr)
        {
            outError = "GpuCrowdUpdater: not initialized";
            return false;
        }
        if (!m_agentBuffer.IsValid() || m_agentCount == 0)
        {
            outError = "GpuCrowdUpdater: no agents uploaded";
            return false;
        }
        if (store.Count() != m_agentCount)
        {
            outError = "GpuCrowdUpdater: the store agent count differs from the uploaded count";
            return false;
        }

        const uint64_t agentBytes = static_cast<uint64_t>(m_agentCount) * sizeof(CrowdAgentGpu);

        BufferGuard guard(*m_device);
        const RhiBufferHandle readbackBuffer = guard.Create(
            MakeBufferDesc(agentBytes, 0, RhiBufferUsageFlags::CopyDest,
                           RhiMemoryUsage::GpuToCpu, RhiResourceState::CopyDest),
            nullptr);
        if (!readbackBuffer.IsValid())
        {
            outError = "GpuCrowdUpdater: failed to create the readback buffer";
            return false;
        }

        std::unique_ptr<IRhiCommandEncoder> encoder = m_device->CreateCommandEncoder(RhiQueueType::Graphics);
        if (!encoder)
        {
            outError = "GpuCrowdUpdater: failed to create a command encoder";
            return false;
        }

        encoder->BeginDebugLabel("Crowd.Readback");
        RhiResourceTransitionDesc toCopy{};
        toCopy.resource = m_agentBuffer;
        toCopy.before = RhiResourceState::UnorderedAccess;
        toCopy.after = RhiResourceState::CopySource;
        encoder->TransitionResources(&toCopy, 1);

        RhiBufferCopyDesc copy{};
        copy.source = m_agentBuffer;
        copy.destination = readbackBuffer;
        copy.sizeInBytes = agentBytes;
        encoder->CopyBuffer(copy);

        RhiResourceTransitionDesc toUav{};
        toUav.resource = m_agentBuffer;
        toUav.before = RhiResourceState::CopySource;
        toUav.after = RhiResourceState::UnorderedAccess;
        encoder->TransitionResources(&toUav, 1);
        encoder->EndDebugLabel();

        if (!m_device->SubmitCommandEncoder(*encoder, RhiQueueType::Graphics))
        {
            outError = "GpuCrowdUpdater: failed to submit the command encoder";
            return false;
        }
        if (!m_device->WaitForRhiIdle())
        {
            outError = "GpuCrowdUpdater: waiting for the GPU failed";
            return false;
        }

        std::vector<CrowdAgentGpu> agents(m_agentCount);
        if (!m_device->ReadRhiBuffer(readbackBuffer, 0, agents.data(), agentBytes))
        {
            outError = "GpuCrowdUpdater: failed to read back the agents";
            return false;
        }

        for (uint32_t i = 0; i < m_agentCount; ++i)
        {
            const CrowdAgentGpu& agent = agents[i];
            store.posX[i] = agent.posX;
            store.posY[i] = agent.posY;
            store.posZ[i] = agent.posZ;
            store.animTime[i] = agent.animTime;
            store.velX[i] = agent.velX;
            store.velZ[i] = agent.velZ;
            store.animRate[i] = agent.animRate;
            store.clip[i] = agent.clip;
        }
        return true;
    }
}
