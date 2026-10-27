#include "Renderer/Crowd/CrowdGpuCuller.h"

#include "Renderer/Resources/ShaderCompilationService.h"
#include "Renderer/Voxel/GpuVoxelBuffers.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Crowd
{
    namespace
    {
        using namespace Voxel::GpuVoxelBuffers;

        constexpr uint32_t kThreadsPerGroup = 64;
        constexpr uint32_t kMaxGroupsX = 65535;
        constexpr uint32_t kBindingCount = 4;

        // Mirrors CullParams in CrowdCull_CS.hlsl (structured element).
        struct CullParamsGpu
        {
            float planes[6][4];
            float cameraPos[3];
            uint32_t agentCount;
            float sphereCenterY;
            float sphereRadius;
            float modelScale;
            float modelYOffset;
            float lodDistance1;
            float lodDistance2;
            uint32_t lodCount;
            int32_t forceLod;
            uint32_t capacity;
            uint32_t frustumEnabled;
            uint32_t threadsPerRow;
            uint32_t pad0;
            uint32_t lodIndexCount[4];
            uint32_t lodFirstIndex[4];
        };
        static_assert(sizeof(CullParamsGpu) == 192, "CullParamsGpu must match the HLSL CullParams layout");
        static_assert(sizeof(CullParamsGpu) <= CrowdGpuCuller::kParamsStrideBytes,
                      "CullParamsGpu must fit one ring slot");

        // Gribb & Hartmann 2001. m is row-major and used as clip = mul(float4(p, 1), m), so column j
        // holds the coefficients of clip_j. D3D clip volume: -w <= x,y <= w, 0 <= z <= w.
        void ExtractFrustumPlanes(const float* m, float outPlanes[6][4])
        {
            auto column = [m](int j, float out[4])
            {
                out[0] = m[0 * 4 + j];
                out[1] = m[1 * 4 + j];
                out[2] = m[2 * 4 + j];
                out[3] = m[3 * 4 + j];
            };
            float c0[4], c1[4], c2[4], c3[4];
            column(0, c0);
            column(1, c1);
            column(2, c2);
            column(3, c3);

            for (int k = 0; k < 4; ++k)
            {
                outPlanes[0][k] = c3[k] + c0[k];
                outPlanes[1][k] = c3[k] - c0[k];
                outPlanes[2][k] = c3[k] + c1[k];
                outPlanes[3][k] = c3[k] - c1[k];
                outPlanes[4][k] = c2[k];
                outPlanes[5][k] = c3[k] - c2[k];
            }

            for (int i = 0; i < 6; ++i)
            {
                float* plane = outPlanes[i];
                const float length = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
                if (length < 1e-6f)
                {
                    plane[0] = 0.0f;
                    plane[1] = 0.0f;
                    plane[2] = 0.0f;
                    plane[3] = 1.0f;
                    continue;
                }
                const float inv = 1.0f / length;
                plane[0] *= inv;
                plane[1] *= inv;
                plane[2] *= inv;
                plane[3] *= inv;
            }
        }

        bool CompilePipeline(IRhiDevice& device, const char* entry, RhiPipelineLayoutHandle layout,
                             RhiPipelineHandle& outBindable, std::string& outError)
        {
            std::vector<uint8_t> bytecode;
            if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc(
                    "CrowdCull", L"Compute/Crowd/CrowdCull_CS.hlsl", entry, "cs_6_0", bytecode)
                || bytecode.empty())
            {
                outError = std::string("CrowdGpuCuller: failed to compile CrowdCull_CS.hlsl ") + entry;
                return false;
            }

            RhiShaderModuleDesc shaderDesc{};
            shaderDesc.bytecode = bytecode.data();
            shaderDesc.bytecodeSize = bytecode.size();
            shaderDesc.entryPoint = entry;
            shaderDesc.stage = RhiShaderStageFlags::Compute;
            if (!device.CreateRhiShaderModule(shaderDesc).IsValid())
            {
                outError = std::string("CrowdGpuCuller: failed to create the shader module ") + entry;
                return false;
            }

            RhiComputePipelineDesc pipelineDesc{};
            pipelineDesc.layout = layout;
            pipelineDesc.shader = shaderDesc;
            const RhiPipelineHandle pipeline = device.CreateRhiComputePipeline(pipelineDesc);
            if (!pipeline.IsValid())
            {
                outError = std::string("CrowdGpuCuller: failed to create the compute pipeline ") + entry;
                return false;
            }
            outBindable = device.GetBindablePipelineHandle(pipeline);
            if (!outBindable.IsValid())
            {
                outError = std::string("CrowdGpuCuller: failed to resolve the bindable pipeline ") + entry;
                return false;
            }
            return true;
        }
    }

    bool CrowdGpuCuller::Initialize(IRhiDevice& device, std::string& outError)
    {
        if (m_device != nullptr)
        {
            return true;
        }
        if (device.GetRhiBackendApi() != RhiBackendApi::DirectX12)
        {
            outError = "CrowdGpuCuller: only the D3D12 backend is supported";
            return false;
        }

        RhiBindingRangeDesc bindings[kBindingCount]{};
        for (uint32_t i = 0; i < kBindingCount; ++i)
        {
            bindings[i].type = (i == 0) ? RhiBindingType::ShaderResource : RhiBindingType::UnorderedAccess;
            bindings[i].visibility = RhiShaderStageFlags::Compute;
            bindings[i].baseRegister = (i == 0) ? 0 : i - 1;
            bindings[i].registerSpace = 0;
            bindings[i].descriptorCount = 1;
            bindings[i].inlineRootDescriptor = true;
            bindings[i].bufferResource = true;
        }

        RhiPipelineLayoutDesc layoutDesc{};
        layoutDesc.bindings = bindings;
        layoutDesc.bindingCount = kBindingCount;
        layoutDesc.allowInputAssembler = false;
        const RhiPipelineLayoutHandle layout = device.CreateRhiPipelineLayout(layoutDesc);
        if (!layout.IsValid())
        {
            outError = "CrowdGpuCuller: failed to create the pipeline layout";
            return false;
        }

        m_layout = device.GetBindablePipelineLayoutHandle(layout);
        if (!m_layout.IsValid())
        {
            outError = "CrowdGpuCuller: failed to resolve the bindable pipeline layout";
            Shutdown();
            return false;
        }
        if (!CompilePipeline(device, "CSReset", layout, m_resetPipeline, outError)
            || !CompilePipeline(device, "CSCull", layout, m_cullPipeline, outError))
        {
            Shutdown();
            return false;
        }

        m_device = &device;
        m_argsBuffer = device.CreateRhiBuffer(
            MakeBufferDesc(static_cast<uint64_t>(CROWD_MAX_LODS) * kArgsStrideBytes, 0,
                           RhiBufferUsageFlags::UnorderedAccess | RhiBufferUsageFlags::CopySource,
                           RhiMemoryUsage::GpuOnly, RhiResourceState::UnorderedAccess),
            nullptr);
        if (!m_argsBuffer.IsValid())
        {
            outError = "CrowdGpuCuller: failed to create the args buffer";
            Shutdown();
            return false;
        }

        m_paramsRing = device.CreateRhiBuffer(
            MakeBufferDesc(static_cast<uint64_t>(kParamsRingSize) * kParamsStrideBytes, sizeof(CullParamsGpu),
                           RhiBufferUsageFlags::ShaderResource | RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::CpuToGpu, RhiResourceState::ShaderResource),
            nullptr);
        if (!m_paramsRing.IsValid())
        {
            outError = "CrowdGpuCuller: failed to create the params ring";
            Shutdown();
            return false;
        }

        m_argsAddress = device.GetRhiBufferGpuAddress(m_argsBuffer);
        m_paramsRingAddress = device.GetRhiBufferGpuAddress(m_paramsRing);
        if (m_argsAddress == 0 || m_paramsRingAddress == 0)
        {
            outError = "CrowdGpuCuller: failed to resolve GPU addresses";
            Shutdown();
            return false;
        }

        m_paramsRingCursor = 0;
        return true;
    }

    void CrowdGpuCuller::Shutdown()
    {
        if (m_device != nullptr)
        {
            if (m_argsBuffer.IsValid())
            {
                m_device->DestroyRhiResource(m_argsBuffer);
            }
            if (m_visibleBuffer.IsValid())
            {
                m_device->DestroyRhiResource(m_visibleBuffer);
            }
            if (m_paramsRing.IsValid())
            {
                m_device->DestroyRhiResource(m_paramsRing);
            }
        }
        m_layout = {};
        m_resetPipeline = {};
        m_cullPipeline = {};
        m_argsBuffer = {};
        m_argsAddress = 0;
        m_visibleBuffer = {};
        m_visibleAddress = 0;
        m_paramsRing = {};
        m_paramsRingAddress = 0;
        m_paramsRingCursor = 0;
        m_capacity = 0;
        m_recordedLodCount = 0;
        m_inDrawState = false;
        m_device = nullptr;
    }

    bool CrowdGpuCuller::EnsureCapacity(uint32_t agentCount, std::string& outError)
    {
        if (m_device == nullptr)
        {
            outError = "CrowdGpuCuller: not initialized";
            return false;
        }
        if (agentCount == 0)
        {
            return true;
        }
        if (agentCount <= m_capacity && m_visibleBuffer.IsValid())
        {
            return true;
        }

        if (m_visibleBuffer.IsValid())
        {
            m_device->DestroyRhiResource(m_visibleBuffer);
            m_visibleBuffer = {};
            m_visibleAddress = 0;
            m_capacity = 0;
        }

        const uint64_t bytes = static_cast<uint64_t>(CROWD_MAX_LODS) * agentCount * sizeof(uint32_t);
        const RhiBufferHandle visible = m_device->CreateRhiBuffer(
            MakeBufferDesc(bytes, sizeof(uint32_t),
                           RhiBufferUsageFlags::UnorderedAccess | RhiBufferUsageFlags::ShaderResource
                               | RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::GpuOnly, RhiResourceState::UnorderedAccess),
            nullptr);
        if (!visible.IsValid())
        {
            outError = "CrowdGpuCuller: failed to create the visible buffer";
            return false;
        }
        const RhiGpuAddress address = m_device->GetRhiBufferGpuAddress(visible);
        if (address == 0)
        {
            m_device->DestroyRhiResource(visible);
            outError = "CrowdGpuCuller: failed to resolve the visible buffer address";
            return false;
        }

        m_visibleBuffer = visible;
        m_visibleAddress = address;
        m_capacity = agentCount;
        return true;
    }

    bool CrowdGpuCuller::Record(IRhiCommandEncoder& encoder, const CrowdCullInputs& inputs,
                                const CrowdCullSettings& settings, std::string& outError)
    {
        if (m_device == nullptr)
        {
            outError = "CrowdGpuCuller: not initialized";
            return false;
        }
        if (inputs.asset == nullptr || inputs.asset->LodCount() == 0)
        {
            outError = "CrowdGpuCuller: the asset has no LODs";
            return false;
        }
        if (inputs.agentCount == 0 || inputs.agentCount > m_capacity || !m_visibleBuffer.IsValid())
        {
            outError = "CrowdGpuCuller: agent count is zero or exceeds the culler capacity";
            return false;
        }
        if (inputs.viewProj == nullptr || inputs.cameraPos == nullptr)
        {
            outError = "CrowdGpuCuller: viewProj / cameraPos is null";
            return false;
        }
        const RhiGpuAddress agentAddress = m_device->GetRhiBufferGpuAddress(inputs.agentBuffer);
        if (agentAddress == 0)
        {
            outError = "CrowdGpuCuller: the agent buffer has no GPU address";
            return false;
        }

        if (m_inDrawState)
        {
            RestoreAfterDraw(encoder);
        }

        const uint32_t lodCount = (std::min)(inputs.asset->LodCount(), static_cast<uint32_t>(CROWD_MAX_LODS));
        const uint32_t groups = (inputs.agentCount + kThreadsPerGroup - 1) / kThreadsPerGroup;
        const uint32_t groupsX = (std::min)(groups, kMaxGroupsX);
        const uint32_t groupsY = (groups + groupsX - 1) / groupsX;

        CullParamsGpu params{};
        ExtractFrustumPlanes(inputs.viewProj, params.planes);
        std::memcpy(params.cameraPos, inputs.cameraPos, sizeof(params.cameraPos));
        params.agentCount = inputs.agentCount;
        params.sphereCenterY = inputs.asset->SphereCenterY();
        params.sphereRadius = inputs.asset->SphereRadius();
        params.modelScale = inputs.modelScale;
        params.modelYOffset = inputs.modelYOffset;
        params.lodDistance1 = settings.lodDistance[0];
        params.lodDistance2 = settings.lodDistance[1];
        params.lodCount = lodCount;
        params.forceLod = (settings.forceLod >= 0)
            ? (std::min)(settings.forceLod, static_cast<int>(lodCount) - 1)
            : -1;
        params.capacity = m_capacity;
        params.frustumEnabled = settings.frustum ? 1u : 0u;
        params.threadsPerRow = groupsX * kThreadsPerGroup;
        for (uint32_t i = 0; i < lodCount; ++i)
        {
            const CrowdLodRange lod = inputs.asset->Lod(i);
            params.lodIndexCount[i] = lod.indexCount;
            params.lodFirstIndex[i] = lod.firstIndex;
        }

        const uint32_t slot = m_paramsRingCursor;
        if (!m_device->UpdateRhiBuffer(m_paramsRing, static_cast<uint64_t>(slot) * kParamsStrideBytes,
                                       &params, sizeof(CullParamsGpu)))
        {
            outError = "CrowdGpuCuller: failed to update the params ring";
            return false;
        }
        m_paramsRingCursor = (slot + 1) % kParamsRingSize;

        const RhiBufferHandle argsAndVisible[2] = { m_argsBuffer, m_visibleBuffer };

        encoder.BeginDebugLabel("Crowd.Cull");
        encoder.SetComputePipelineLayout(m_layout);
        encoder.SetComputeShaderResourceView(0, m_paramsRingAddress + static_cast<uint64_t>(slot) * kParamsStrideBytes);
        encoder.SetComputeUnorderedAccessView(1, agentAddress);
        encoder.SetComputeUnorderedAccessView(2, m_argsAddress);
        encoder.SetComputeUnorderedAccessView(3, m_visibleAddress);

        encoder.SetComputePipeline(m_resetPipeline);
        RhiDispatchDesc reset{};
        reset.groupCountX = 1;
        reset.groupCountY = 1;
        reset.groupCountZ = 1;
        encoder.Dispatch(reset);
        encoder.UavBarrier(&argsAndVisible[0], 1);

        encoder.SetComputePipeline(m_cullPipeline);
        RhiDispatchDesc cull{};
        cull.groupCountX = groupsX;
        cull.groupCountY = groupsY;
        cull.groupCountZ = 1;
        encoder.Dispatch(cull);
        encoder.UavBarrier(argsAndVisible, 2);

        RhiResourceTransitionDesc transitions[2]{};
        transitions[0].resource = m_argsBuffer;
        transitions[0].before = RhiResourceState::UnorderedAccess;
        transitions[0].after = RhiResourceState::IndirectArgument;
        transitions[1].resource = m_visibleBuffer;
        transitions[1].before = RhiResourceState::UnorderedAccess;
        transitions[1].after = RhiResourceState::ShaderResource;
        encoder.TransitionResources(transitions, 2);
        encoder.EndDebugLabel();

        m_recordedLodCount = lodCount;
        m_inDrawState = true;
        return true;
    }

    void CrowdGpuCuller::RestoreAfterDraw(IRhiCommandEncoder& encoder)
    {
        if (!m_inDrawState)
        {
            return;
        }

        RhiResourceTransitionDesc transitions[2]{};
        transitions[0].resource = m_argsBuffer;
        transitions[0].before = RhiResourceState::IndirectArgument;
        transitions[0].after = RhiResourceState::UnorderedAccess;
        transitions[1].resource = m_visibleBuffer;
        transitions[1].before = RhiResourceState::ShaderResource;
        transitions[1].after = RhiResourceState::UnorderedAccess;
        encoder.TransitionResources(transitions, 2);
        m_inDrawState = false;
    }

    bool CrowdGpuCuller::ReadVisibleCounts(uint32_t outCounts[CROWD_MAX_LODS], std::string& outError)
    {
        for (uint32_t i = 0; i < CROWD_MAX_LODS; ++i)
        {
            outCounts[i] = 0;
        }
        if (m_device == nullptr)
        {
            outError = "CrowdGpuCuller: not initialized";
            return false;
        }
        if (m_inDrawState)
        {
            outError = "CrowdGpuCuller: call after RestoreAfterDraw";
            return false;
        }

        const uint64_t argsBytes = static_cast<uint64_t>(CROWD_MAX_LODS) * kArgsStrideBytes;

        BufferGuard guard(*m_device);
        const RhiBufferHandle readbackBuffer = guard.Create(
            MakeBufferDesc(argsBytes, 0, RhiBufferUsageFlags::CopyDest,
                           RhiMemoryUsage::GpuToCpu, RhiResourceState::CopyDest),
            nullptr);
        if (!readbackBuffer.IsValid())
        {
            outError = "CrowdGpuCuller: failed to create the readback buffer";
            return false;
        }

        std::unique_ptr<IRhiCommandEncoder> encoder = m_device->CreateCommandEncoder(RhiQueueType::Graphics);
        if (!encoder)
        {
            outError = "CrowdGpuCuller: failed to create a command encoder";
            return false;
        }

        encoder->BeginDebugLabel("Crowd.Cull.Readback");
        RhiResourceTransitionDesc toCopy{};
        toCopy.resource = m_argsBuffer;
        toCopy.before = RhiResourceState::UnorderedAccess;
        toCopy.after = RhiResourceState::CopySource;
        encoder->TransitionResources(&toCopy, 1);

        RhiBufferCopyDesc copy{};
        copy.source = m_argsBuffer;
        copy.destination = readbackBuffer;
        copy.sizeInBytes = argsBytes;
        encoder->CopyBuffer(copy);

        RhiResourceTransitionDesc toUav{};
        toUav.resource = m_argsBuffer;
        toUav.before = RhiResourceState::CopySource;
        toUav.after = RhiResourceState::UnorderedAccess;
        encoder->TransitionResources(&toUav, 1);
        encoder->EndDebugLabel();

        if (!m_device->SubmitCommandEncoder(*encoder, RhiQueueType::Graphics))
        {
            outError = "CrowdGpuCuller: failed to submit the command encoder";
            return false;
        }
        if (!m_device->WaitForRhiIdle())
        {
            outError = "CrowdGpuCuller: waiting for the GPU failed";
            return false;
        }

        uint32_t records[CROWD_MAX_LODS * 5]{};
        if (!m_device->ReadRhiBuffer(readbackBuffer, 0, records, argsBytes))
        {
            outError = "CrowdGpuCuller: failed to read back the args";
            return false;
        }
        for (uint32_t i = 0; i < m_recordedLodCount && i < CROWD_MAX_LODS; ++i)
        {
            outCounts[i] = records[i * 5 + 1];
        }
        return true;
    }
}
