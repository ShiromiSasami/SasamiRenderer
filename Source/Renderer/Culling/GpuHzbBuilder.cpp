#include "Renderer/Culling/GpuHzbBuilder.h"

#include "Renderer/Resources/ShaderCompilationService.h"
#include "Renderer/Voxel/GpuVoxelBuffers.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Culling
{
    namespace
    {
        using namespace Voxel::GpuVoxelBuffers;

        constexpr uint32_t kParamsStrideBytes = 256;
        constexpr uint32_t kBindingCount = 2;
        constexpr uint32_t kGroupSize = 8;
        constexpr uint32_t kMaxDimension = 8192;

        // Mirrors HzbLevelParams in HzbDownsample_CS.hlsl (structured element).
        struct HzbLevelParamsGpu
        {
            uint32_t srcOffset;
            uint32_t srcWidth;
            uint32_t srcHeight;
            uint32_t dstOffset;
            uint32_t dstWidth;
            uint32_t dstHeight;
            uint32_t pad0;
            uint32_t pad1;
        };
        static_assert(sizeof(HzbLevelParamsGpu) == 32, "HzbLevelParamsGpu must match the HLSL HzbLevelParams layout");
        static_assert(sizeof(HzbLevelParamsGpu) <= kParamsStrideBytes, "HzbLevelParamsGpu must fit one params slot");

        bool CompilePipeline(IRhiDevice& device, RhiPipelineLayoutHandle layout,
                             RhiPipelineHandle& outBindable, std::string& outError)
        {
            const char* entry = "CSDownsample";
            std::vector<uint8_t> bytecode;
            if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc(
                    "HzbDownsample", L"Compute/Culling/HzbDownsample_CS.hlsl", entry, "cs_6_0", bytecode)
                || bytecode.empty())
            {
                outError = std::string("GpuHzbBuilder: failed to compile HzbDownsample_CS.hlsl ") + entry;
                return false;
            }

            RhiShaderModuleDesc shaderDesc{};
            shaderDesc.bytecode = bytecode.data();
            shaderDesc.bytecodeSize = bytecode.size();
            shaderDesc.entryPoint = entry;
            shaderDesc.stage = RhiShaderStageFlags::Compute;
            if (!device.CreateRhiShaderModule(shaderDesc).IsValid())
            {
                outError = std::string("GpuHzbBuilder: failed to create the shader module ") + entry;
                return false;
            }

            RhiComputePipelineDesc pipelineDesc{};
            pipelineDesc.layout = layout;
            pipelineDesc.shader = shaderDesc;
            const RhiPipelineHandle pipeline = device.CreateRhiComputePipeline(pipelineDesc);
            if (!pipeline.IsValid())
            {
                outError = std::string("GpuHzbBuilder: failed to create the compute pipeline ") + entry;
                return false;
            }
            outBindable = device.GetBindablePipelineHandle(pipeline);
            if (!outBindable.IsValid())
            {
                outError = std::string("GpuHzbBuilder: failed to resolve the bindable pipeline ") + entry;
                return false;
            }
            return true;
        }
    }

    std::vector<GpuHzbLevel> GpuHzbBuilder::ComputeLayout(uint32_t width, uint32_t height, uint32_t& outTotalFloats)
    {
        std::vector<GpuHzbLevel> levels;
        outTotalFloats = 0;
        if (width == 0 || height == 0)
        {
            return levels;
        }

        GpuHzbLevel level{};
        level.width = width;
        level.height = height;
        level.offset = 0;
        levels.push_back(level);
        outTotalFloats = width * height;

        while (levels.back().width > 1 || levels.back().height > 1)
        {
            GpuHzbLevel next{};
            next.width = (std::max)(1u, levels.back().width / 2);
            next.height = (std::max)(1u, levels.back().height / 2);
            next.offset = outTotalFloats;
            outTotalFloats += next.width * next.height;
            levels.push_back(next);
        }
        return levels;
    }

    bool GpuHzbBuilder::Initialize(IRhiDevice& device, std::string& outError)
    {
        if (m_device != nullptr)
        {
            return true;
        }
        if (device.GetRhiBackendApi() != RhiBackendApi::DirectX12)
        {
            outError = "GpuHzbBuilder: only the D3D12 backend is supported";
            return false;
        }

        RhiBindingRangeDesc bindings[kBindingCount]{};
        for (uint32_t i = 0; i < kBindingCount; ++i)
        {
            bindings[i].type = (i == 0) ? RhiBindingType::ShaderResource : RhiBindingType::UnorderedAccess;
            bindings[i].visibility = RhiShaderStageFlags::Compute;
            bindings[i].baseRegister = 0;
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
            outError = "GpuHzbBuilder: failed to create the pipeline layout";
            return false;
        }

        m_layout = device.GetBindablePipelineLayoutHandle(layout);
        if (!m_layout.IsValid())
        {
            outError = "GpuHzbBuilder: failed to resolve the bindable pipeline layout";
            Shutdown();
            return false;
        }
        if (!CompilePipeline(device, layout, m_pipeline, outError))
        {
            Shutdown();
            return false;
        }

        m_device = &device;
        return true;
    }

    void GpuHzbBuilder::Shutdown()
    {
        m_layout = {};
        m_pipeline = {};
        m_device = nullptr;
    }

    bool GpuHzbBuilder::BuildAndReadback(const float* depth, uint32_t width, uint32_t height,
                                         std::vector<float>& outTexels, std::vector<GpuHzbLevel>& outLevels,
                                         std::string& outError)
    {
        if (m_device == nullptr)
        {
            outError = "GpuHzbBuilder: not initialized";
            return false;
        }
        if (depth == nullptr)
        {
            outError = "GpuHzbBuilder: depth is null";
            return false;
        }
        if (width < 1 || width > kMaxDimension || height < 1 || height > kMaxDimension)
        {
            outError = "GpuHzbBuilder: width/height must be in 1..8192";
            return false;
        }

        uint32_t totalFloats = 0;
        const std::vector<GpuHzbLevel> layout = ComputeLayout(width, height, totalFloats);
        const uint32_t levelCount = static_cast<uint32_t>(layout.size());

        std::vector<float> pyramidInit(totalFloats, 0.0f);
        std::memcpy(pyramidInit.data(), depth, static_cast<size_t>(width) * height * sizeof(float));

        std::vector<uint8_t> paramsInit(static_cast<size_t>(levelCount) * kParamsStrideBytes, 0);
        for (uint32_t i = 1; i < levelCount; ++i)
        {
            HzbLevelParamsGpu params{};
            params.srcOffset = layout[i - 1].offset;
            params.srcWidth = layout[i - 1].width;
            params.srcHeight = layout[i - 1].height;
            params.dstOffset = layout[i].offset;
            params.dstWidth = layout[i].width;
            params.dstHeight = layout[i].height;
            std::memcpy(paramsInit.data() + static_cast<size_t>(i - 1) * kParamsStrideBytes, &params, sizeof(params));
        }

        const uint64_t pyramidBytes = static_cast<uint64_t>(totalFloats) * sizeof(float);

        BufferGuard guard(*m_device);
        const RhiBufferHandle pyramid = guard.Create(
            MakeBufferDesc(pyramidBytes, sizeof(float),
                           RhiBufferUsageFlags::UnorderedAccess | RhiBufferUsageFlags::CopySource
                               | RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::GpuOnly, RhiResourceState::UnorderedAccess),
            pyramidInit.data());
        if (!pyramid.IsValid())
        {
            outError = "GpuHzbBuilder: failed to create the pyramid buffer";
            return false;
        }
        const RhiBufferHandle paramsBuffer = guard.Create(
            MakeBufferDesc(paramsInit.size(), sizeof(HzbLevelParamsGpu),
                           RhiBufferUsageFlags::ShaderResource | RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::GpuOnly, RhiResourceState::ShaderResource),
            paramsInit.data());
        if (!paramsBuffer.IsValid())
        {
            outError = "GpuHzbBuilder: failed to create the params buffer";
            return false;
        }
        const RhiBufferHandle readbackBuffer = guard.Create(
            MakeBufferDesc(pyramidBytes, 0, RhiBufferUsageFlags::CopyDest,
                           RhiMemoryUsage::GpuToCpu, RhiResourceState::CopyDest),
            nullptr);
        if (!readbackBuffer.IsValid())
        {
            outError = "GpuHzbBuilder: failed to create the readback buffer";
            return false;
        }

        const RhiGpuAddress pyramidAddress = m_device->GetRhiBufferGpuAddress(pyramid);
        const RhiGpuAddress paramsAddress = m_device->GetRhiBufferGpuAddress(paramsBuffer);
        if (pyramidAddress == 0 || paramsAddress == 0)
        {
            outError = "GpuHzbBuilder: failed to resolve GPU addresses";
            return false;
        }

        std::unique_ptr<IRhiCommandEncoder> encoder = m_device->CreateCommandEncoder(RhiQueueType::Graphics);
        if (!encoder)
        {
            outError = "GpuHzbBuilder: failed to create a command encoder";
            return false;
        }

        encoder->BeginDebugLabel("Hzb.Build");
        encoder->SetComputePipelineLayout(m_layout);
        encoder->SetComputePipeline(m_pipeline);
        encoder->SetComputeUnorderedAccessView(1, pyramidAddress);
        for (uint32_t i = 1; i < levelCount; ++i)
        {
            encoder->SetComputeShaderResourceView(0, paramsAddress + static_cast<uint64_t>(i - 1) * kParamsStrideBytes);
            RhiDispatchDesc dispatch{};
            dispatch.groupCountX = (layout[i].width + kGroupSize - 1) / kGroupSize;
            dispatch.groupCountY = (layout[i].height + kGroupSize - 1) / kGroupSize;
            dispatch.groupCountZ = 1;
            encoder->Dispatch(dispatch);
            encoder->UavBarrier(&pyramid, 1);
        }

        RhiResourceTransitionDesc toCopy{};
        toCopy.resource = pyramid;
        toCopy.before = RhiResourceState::UnorderedAccess;
        toCopy.after = RhiResourceState::CopySource;
        encoder->TransitionResources(&toCopy, 1);

        RhiBufferCopyDesc copy{};
        copy.source = pyramid;
        copy.destination = readbackBuffer;
        copy.sizeInBytes = pyramidBytes;
        encoder->CopyBuffer(copy);
        encoder->EndDebugLabel();

        if (!m_device->SubmitCommandEncoder(*encoder, RhiQueueType::Graphics))
        {
            outError = "GpuHzbBuilder: failed to submit the command encoder";
            return false;
        }
        if (!m_device->WaitForRhiIdle())
        {
            outError = "GpuHzbBuilder: waiting for the GPU failed";
            return false;
        }

        outTexels.resize(totalFloats);
        if (!m_device->ReadRhiBuffer(readbackBuffer, 0, outTexels.data(), pyramidBytes))
        {
            outError = "GpuHzbBuilder: failed to read back the pyramid";
            return false;
        }
        outLevels = layout;
        return true;
    }
}
