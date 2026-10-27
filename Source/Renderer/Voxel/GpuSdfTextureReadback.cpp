#include "Renderer/Voxel/GpuSdfTextureReadback.h"

#include "Renderer/Resources/ShaderCompilationService.h"
#include "Renderer/Voxel/GpuVoxelBuffers.h"

#include <memory>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        using namespace GpuVoxelBuffers;

        constexpr uint32_t kGroupSize = 4;
        constexpr uint32_t kBindingCount = 3;

        // Mirrors ReadbackParams in SdfReadback_CS.hlsl (structured element, 16 bytes).
        struct ReadbackParamsGpu
        {
            uint32_t dim[3];
            uint32_t pad;
        };
        static_assert(sizeof(ReadbackParamsGpu) == 16, "ReadbackParamsGpu must match the HLSL ReadbackParams layout");
    }

    bool GpuSdfTextureReadback::Initialize(IRhiDevice& device, std::string& outError)
    {
        if (m_device != nullptr)
        {
            return true;
        }
        if (device.GetRhiBackendApi() != RhiBackendApi::DirectX12)
        {
            outError = "GpuSdfTextureReadback: only the D3D12 backend is supported";
            return false;
        }

        std::vector<uint8_t> bytecode;
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc(
                "Voxel", L"Compute/Voxel/SdfReadback_CS.hlsl", "CSMain", "cs_6_0", bytecode)
            || bytecode.empty())
        {
            outError = "GpuSdfTextureReadback: failed to compile SdfReadback_CS.hlsl";
            return false;
        }

        RhiShaderModuleDesc shaderDesc{};
        shaderDesc.bytecode = bytecode.data();
        shaderDesc.bytecodeSize = bytecode.size();
        shaderDesc.entryPoint = "CSMain";
        shaderDesc.stage = RhiShaderStageFlags::Compute;
        if (!device.CreateRhiShaderModule(shaderDesc).IsValid())
        {
            outError = "GpuSdfTextureReadback: failed to create the compute shader module";
            return false;
        }

        // t0 is a texture, t1 and u0 are structured buffers.
        RhiBindingRangeDesc bindings[kBindingCount]{};
        for (uint32_t i = 0; i < kBindingCount; ++i)
        {
            bindings[i].type = (i < 2) ? RhiBindingType::ShaderResource : RhiBindingType::UnorderedAccess;
            bindings[i].visibility = RhiShaderStageFlags::Compute;
            bindings[i].baseRegister = (i < 2) ? i : 0;
            bindings[i].registerSpace = 0;
            bindings[i].descriptorCount = 1;
            bindings[i].inlineRootDescriptor = false;
            bindings[i].bufferResource = i != 0;
        }

        RhiPipelineLayoutDesc layoutDesc{};
        layoutDesc.bindings = bindings;
        layoutDesc.bindingCount = kBindingCount;
        layoutDesc.allowInputAssembler = false;
        const RhiPipelineLayoutHandle layout = device.CreateRhiPipelineLayout(layoutDesc);
        if (!layout.IsValid())
        {
            outError = "GpuSdfTextureReadback: failed to create the pipeline layout";
            return false;
        }

        RhiComputePipelineDesc pipelineDesc{};
        pipelineDesc.layout = layout;
        pipelineDesc.shader = shaderDesc;
        const RhiPipelineHandle pipeline = device.CreateRhiComputePipeline(pipelineDesc);
        if (!pipeline.IsValid())
        {
            outError = "GpuSdfTextureReadback: failed to create the compute pipeline";
            return false;
        }

        const RhiPipelineLayoutHandle bindableLayout = device.GetBindablePipelineLayoutHandle(layout);
        const RhiPipelineHandle bindablePipeline = device.GetBindablePipelineHandle(pipeline);
        if (!bindableLayout.IsValid() || !bindablePipeline.IsValid())
        {
            outError = "GpuSdfTextureReadback: failed to resolve bindable pipeline handles";
            return false;
        }

        const RhiDescriptorAllocation descriptors =
            device.AllocateRhiDescriptors(RhiDescriptorHeapType::CbvSrvUav, kBindingCount, true);
        if (!descriptors.IsValid() || descriptors.heap.id == 0)
        {
            outError = "GpuSdfTextureReadback: failed to allocate shader-visible descriptors";
            return false;
        }

        m_bindableLayout = bindableLayout;
        m_bindablePipeline = bindablePipeline;
        m_descriptors = descriptors;
        m_device = &device;
        return true;
    }

    bool GpuSdfTextureReadback::Readback(RhiTextureHandle texture, uint32_t width, uint32_t height, uint32_t depth,
                                         std::vector<float>& outTexels, std::string& outError)
    {
        if (m_device == nullptr)
        {
            outError = "GpuSdfTextureReadback: not initialized";
            return false;
        }
        if (!texture.IsValid() || width == 0 || height == 0 || depth == 0)
        {
            outError = "GpuSdfTextureReadback: invalid texture or size";
            return false;
        }

        const uint64_t texelCount = static_cast<uint64_t>(width) * height * depth;
        const uint64_t outputBytes = texelCount * sizeof(float);
        const ReadbackParamsGpu params{ { width, height, depth }, 0 };

        BufferGuard guard(*m_device);
        const RhiBufferHandle paramsBuffer = guard.Create(
            MakeBufferDesc(sizeof(ReadbackParamsGpu), sizeof(ReadbackParamsGpu),
                           RhiBufferUsageFlags::ShaderResource | RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::GpuOnly, RhiResourceState::ShaderResource),
            &params);
        const RhiBufferHandle outputBuffer = guard.Create(
            MakeBufferDesc(outputBytes, sizeof(float),
                           RhiBufferUsageFlags::UnorderedAccess | RhiBufferUsageFlags::CopySource
                               | RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::GpuOnly, RhiResourceState::UnorderedAccess),
            nullptr);
        const RhiBufferHandle readbackBuffer = guard.Create(
            MakeBufferDesc(outputBytes, 0, RhiBufferUsageFlags::CopyDest,
                           RhiMemoryUsage::GpuToCpu, RhiResourceState::CopyDest),
            nullptr);
        if (!paramsBuffer.IsValid() || !outputBuffer.IsValid() || !readbackBuffer.IsValid())
        {
            outError = "GpuSdfTextureReadback: failed to create GPU buffers";
            return false;
        }

        RhiTextureViewDesc textureView{};
        textureView.format = RhiFormat::R32Float;
        textureView.dimension = RhiTextureViewDimension::Texture3D;
        textureView.baseMipLevel = 0;
        textureView.mipLevelCount = 1;

        const uint64_t cpuBase = m_descriptors.cpu.ptr;
        const uint64_t increment = m_descriptors.increment;
        const bool viewsOk =
            m_device->CreateRhiShaderResourceView(texture, textureView, { cpuBase })
            && m_device->CreateRhiBufferShaderResourceView(
                paramsBuffer, MakeStructuredView(sizeof(ReadbackParamsGpu), sizeof(ReadbackParamsGpu)),
                { cpuBase + increment })
            && m_device->CreateRhiBufferUnorderedAccessView(
                outputBuffer, MakeStructuredView(outputBytes, sizeof(float)), { cpuBase + 2 * increment });
        if (!viewsOk)
        {
            outError = "GpuSdfTextureReadback: failed to create views";
            return false;
        }

        std::unique_ptr<IRhiCommandEncoder> encoder = m_device->CreateCommandEncoder(RhiQueueType::Graphics);
        if (!encoder)
        {
            outError = "GpuSdfTextureReadback: failed to create a command encoder";
            return false;
        }

        encoder->BeginDebugLabel("Voxel.SdfReadback");
        encoder->SetDescriptorHeap(m_descriptors.heap);
        encoder->SetComputePipelineLayout(m_bindableLayout);
        encoder->SetComputePipeline(m_bindablePipeline);
        for (uint32_t i = 0; i < kBindingCount; ++i)
        {
            encoder->SetComputeDescriptorTable(i, RhiGpuDescriptorHandle{ m_descriptors.gpu.ptr + i * increment });
        }

        RhiDispatchDesc dispatch{};
        dispatch.groupCountX = (width + kGroupSize - 1) / kGroupSize;
        dispatch.groupCountY = (height + kGroupSize - 1) / kGroupSize;
        dispatch.groupCountZ = (depth + kGroupSize - 1) / kGroupSize;
        encoder->Dispatch(dispatch);

        encoder->UavBarrier(&outputBuffer, 1);

        RhiResourceTransitionDesc toCopy{};
        toCopy.resource = outputBuffer;
        toCopy.before = RhiResourceState::UnorderedAccess;
        toCopy.after = RhiResourceState::CopySource;
        encoder->TransitionResources(&toCopy, 1);

        RhiBufferCopyDesc copy{};
        copy.source = outputBuffer;
        copy.destination = readbackBuffer;
        copy.sizeInBytes = outputBytes;
        encoder->CopyBuffer(copy);
        encoder->EndDebugLabel();

        if (!m_device->SubmitCommandEncoder(*encoder, RhiQueueType::Graphics))
        {
            outError = "GpuSdfTextureReadback: failed to submit the command encoder";
            return false;
        }
        if (!m_device->WaitForRhiIdle())
        {
            outError = "GpuSdfTextureReadback: waiting for the GPU failed";
            return false;
        }

        outTexels.resize(static_cast<size_t>(texelCount));
        if (!m_device->ReadRhiBuffer(readbackBuffer, 0, outTexels.data(), outputBytes))
        {
            outError = "GpuSdfTextureReadback: failed to read back the texels";
            return false;
        }
        return true;
    }
}
