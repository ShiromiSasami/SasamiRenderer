#include "Renderer/Voxel/GpuSolidVoxelizer.h"

#include "Renderer/Resources/ShaderCompilationService.h"
#include "Renderer/Voxel/GpuVoxelBuffers.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        using namespace GpuVoxelBuffers;

        constexpr uint32_t kThreadsPerGroup = 64;
        constexpr uint32_t kMaxGroupsX = 65535;
        constexpr uint32_t kBindingCount = 4;

        // Mirrors SolidVoxelizeParams in SolidVoxelize_CS.hlsl (structured element, 64 bytes).
        struct SolidVoxelizeParamsGpu
        {
            float origin[3];
            float voxelSize;
            uint32_t dim[3];
            uint32_t triangleCount;
            uint32_t vertexCount;
            uint32_t positionStride;
            uint32_t rowWords;
            uint32_t rowCount;
            uint32_t crossingsThreadsPerRow;
            uint32_t prefixThreadsPerRow;
            uint32_t pad0;
            uint32_t pad1;
        };
        static_assert(sizeof(SolidVoxelizeParamsGpu) == 64,
                      "SolidVoxelizeParamsGpu must match the HLSL SolidVoxelizeParams layout");

        struct DispatchShape
        {
            uint32_t groupsX = 0;
            uint32_t groupsY = 0;
        };

        // One thread per item, 64 threads per group, wrapped into Y once X hits the 65535 limit.
        DispatchShape MakeDispatchShape(uint64_t itemCount)
        {
            const uint64_t groups = (itemCount + kThreadsPerGroup - 1) / kThreadsPerGroup;
            DispatchShape shape;
            shape.groupsX = static_cast<uint32_t>((std::min)(groups, static_cast<uint64_t>(kMaxGroupsX)));
            shape.groupsY = static_cast<uint32_t>((groups + shape.groupsX - 1) / shape.groupsX);
            return shape;
        }
    }

    bool GpuSolidVoxelizer::Initialize(IRhiDevice& device, std::string& outError)
    {
        if (m_device != nullptr)
        {
            return true;
        }
        if (device.GetRhiBackendApi() != RhiBackendApi::DirectX12)
        {
            outError = "GpuSolidVoxelizer: only the D3D12 backend is supported";
            return false;
        }

        const wchar_t* const shaderPath = L"Compute/Voxel/SolidVoxelize_CS.hlsl";
        const char* const entryNames[2] = { "CSCrossings", "CSPrefix" };
        std::vector<uint8_t> bytecodes[2];
        RhiShaderModuleDesc shaderDescs[2]{};
        for (uint32_t i = 0; i < 2; ++i)
        {
            if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc(
                    "Voxel", shaderPath, entryNames[i], "cs_6_0", bytecodes[i])
                || bytecodes[i].empty())
            {
                outError = "GpuSolidVoxelizer: failed to compile SolidVoxelize_CS.hlsl";
                return false;
            }

            shaderDescs[i].bytecode = bytecodes[i].data();
            shaderDescs[i].bytecodeSize = bytecodes[i].size();
            shaderDescs[i].entryPoint = entryNames[i];
            shaderDescs[i].stage = RhiShaderStageFlags::Compute;
            if (!device.CreateRhiShaderModule(shaderDescs[i]).IsValid())
            {
                outError = std::string("GpuSolidVoxelizer: failed to create the ") + entryNames[i]
                    + " compute shader module";
                return false;
            }
        }

        RhiBindingRangeDesc bindings[kBindingCount]{};
        for (uint32_t i = 0; i < kBindingCount; ++i)
        {
            bindings[i].type = (i < 3) ? RhiBindingType::ShaderResource : RhiBindingType::UnorderedAccess;
            bindings[i].visibility = RhiShaderStageFlags::Compute;
            bindings[i].baseRegister = (i < 3) ? i : 0;
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
            outError = "GpuSolidVoxelizer: failed to create the pipeline layout";
            return false;
        }

        RhiPipelineHandle pipelines[2]{};
        for (uint32_t i = 0; i < 2; ++i)
        {
            RhiComputePipelineDesc pipelineDesc{};
            pipelineDesc.layout = layout;
            pipelineDesc.shader = shaderDescs[i];
            pipelines[i] = device.CreateRhiComputePipeline(pipelineDesc);
            if (!pipelines[i].IsValid())
            {
                outError = std::string("GpuSolidVoxelizer: failed to create the ") + entryNames[i]
                    + " compute pipeline (the device may lack double-precision shader support)";
                return false;
            }
        }

        const RhiPipelineLayoutHandle bindableLayout = device.GetBindablePipelineLayoutHandle(layout);
        const RhiPipelineHandle bindableCrossings = device.GetBindablePipelineHandle(pipelines[0]);
        const RhiPipelineHandle bindablePrefix = device.GetBindablePipelineHandle(pipelines[1]);
        if (!bindableLayout.IsValid() || !bindableCrossings.IsValid() || !bindablePrefix.IsValid())
        {
            outError = "GpuSolidVoxelizer: failed to resolve bindable pipeline handles";
            return false;
        }

        const RhiDescriptorAllocation descriptors =
            device.AllocateRhiDescriptors(RhiDescriptorHeapType::CbvSrvUav, kBindingCount, true);
        if (!descriptors.IsValid() || descriptors.heap.id == 0)
        {
            outError = "GpuSolidVoxelizer: failed to allocate shader-visible descriptors";
            return false;
        }

        m_bindableLayout = bindableLayout;
        m_bindableCrossingsPipeline = bindableCrossings;
        m_bindablePrefixPipeline = bindablePrefix;
        m_descriptors = descriptors;
        m_device = &device;
        return true;
    }

    bool GpuSolidVoxelizer::Voxelize(const VoxelGridDesc& desc,
                                     const VoxelizerMeshInput& mesh,
                                     OccupancyBitGrid& outGrid,
                                     std::string& outError)
    {
        if (m_device == nullptr)
        {
            outError = "GpuSolidVoxelizer: not initialized";
            return false;
        }
        if (desc.dimX == 0 || desc.dimY == 0 || desc.dimZ == 0 || !(desc.voxelSize > 0.0f))
        {
            outError = "GpuSolidVoxelizer: invalid grid description";
            return false;
        }
        if (mesh.positions == nullptr || mesh.positionStrideInFloats < 3)
        {
            outError = "GpuSolidVoxelizer: invalid mesh input";
            return false;
        }

        outGrid.Reset(desc.dimX, desc.dimY, desc.dimZ);

        const uint32_t triangleCount = (mesh.indices != nullptr ? mesh.indexCount : mesh.vertexCount) / 3;
        if (triangleCount == 0)
        {
            return true;
        }

        std::vector<uint32_t> indices(static_cast<size_t>(triangleCount) * 3);
        if (mesh.indices != nullptr)
        {
            std::copy(mesh.indices, mesh.indices + indices.size(), indices.begin());
        }
        else
        {
            for (size_t i = 0; i < indices.size(); ++i)
            {
                indices[i] = static_cast<uint32_t>(i);
            }
        }

        const uint32_t rowWords = (desc.dimX + 31) / 32;
        const uint64_t rowCount = static_cast<uint64_t>(desc.dimY) * desc.dimZ;
        if (rowCount > UINT32_MAX)
        {
            outError = "GpuSolidVoxelizer: grid has too many rows";
            return false;
        }
        const uint64_t wordCount = rowCount * rowWords;

        const DispatchShape crossingsShape = MakeDispatchShape(triangleCount);
        const DispatchShape prefixShape = MakeDispatchShape(rowCount);

        SolidVoxelizeParamsGpu params{};
        std::memcpy(params.origin, desc.origin, sizeof(params.origin));
        params.voxelSize = desc.voxelSize;
        params.dim[0] = desc.dimX;
        params.dim[1] = desc.dimY;
        params.dim[2] = desc.dimZ;
        params.triangleCount = triangleCount;
        params.vertexCount = mesh.vertexCount;
        params.positionStride = mesh.positionStrideInFloats;
        params.rowWords = rowWords;
        params.rowCount = static_cast<uint32_t>(rowCount);
        params.crossingsThreadsPerRow = crossingsShape.groupsX * kThreadsPerGroup;
        params.prefixThreadsPerRow = prefixShape.groupsX * kThreadsPerGroup;
        params.pad0 = 0;
        params.pad1 = 0;

        const uint64_t bitsBytes = wordCount * sizeof(uint32_t);
        const uint64_t positionBytes =
            static_cast<uint64_t>(mesh.vertexCount) * mesh.positionStrideInFloats * sizeof(float);
        const uint64_t indexBytes = static_cast<uint64_t>(indices.size()) * sizeof(uint32_t);
        if (positionBytes == 0)
        {
            outError = "GpuSolidVoxelizer: mesh has no vertices";
            return false;
        }

        const std::vector<uint32_t> zeroWords(static_cast<size_t>(wordCount), 0u);

        BufferGuard guard(*m_device);
        const RhiBufferHandle positionsBuffer = guard.Create(
            MakeBufferDesc(positionBytes, sizeof(float),
                           RhiBufferUsageFlags::ShaderResource | RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::GpuOnly, RhiResourceState::ShaderResource),
            mesh.positions);
        const RhiBufferHandle indicesBuffer = guard.Create(
            MakeBufferDesc(indexBytes, sizeof(uint32_t),
                           RhiBufferUsageFlags::ShaderResource | RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::GpuOnly, RhiResourceState::ShaderResource),
            indices.data());
        const RhiBufferHandle paramsBuffer = guard.Create(
            MakeBufferDesc(sizeof(SolidVoxelizeParamsGpu), sizeof(SolidVoxelizeParamsGpu),
                           RhiBufferUsageFlags::ShaderResource | RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::GpuOnly, RhiResourceState::ShaderResource),
            &params);
        const RhiBufferHandle bitsBuffer = guard.Create(
            MakeBufferDesc(bitsBytes, sizeof(uint32_t),
                           RhiBufferUsageFlags::UnorderedAccess | RhiBufferUsageFlags::CopySource
                               | RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::GpuOnly, RhiResourceState::UnorderedAccess),
            zeroWords.data());
        const RhiBufferHandle readbackBuffer = guard.Create(
            MakeBufferDesc(bitsBytes, 0, RhiBufferUsageFlags::CopyDest,
                           RhiMemoryUsage::GpuToCpu, RhiResourceState::CopyDest),
            nullptr);
        if (!positionsBuffer.IsValid() || !indicesBuffer.IsValid() || !paramsBuffer.IsValid()
            || !bitsBuffer.IsValid() || !readbackBuffer.IsValid())
        {
            outError = "GpuSolidVoxelizer: failed to create GPU buffers";
            return false;
        }

        const uint64_t cpuBase = m_descriptors.cpu.ptr;
        const uint64_t increment = m_descriptors.increment;
        const bool viewsOk =
            m_device->CreateRhiBufferShaderResourceView(
                positionsBuffer, MakeStructuredView(positionBytes, sizeof(float)), { cpuBase })
            && m_device->CreateRhiBufferShaderResourceView(
                indicesBuffer, MakeStructuredView(indexBytes, sizeof(uint32_t)), { cpuBase + increment })
            && m_device->CreateRhiBufferShaderResourceView(
                paramsBuffer, MakeStructuredView(sizeof(SolidVoxelizeParamsGpu), sizeof(SolidVoxelizeParamsGpu)),
                { cpuBase + 2 * increment })
            && m_device->CreateRhiBufferUnorderedAccessView(
                bitsBuffer, MakeStructuredView(bitsBytes, sizeof(uint32_t)), { cpuBase + 3 * increment });
        if (!viewsOk)
        {
            outError = "GpuSolidVoxelizer: failed to create buffer views";
            return false;
        }

        std::unique_ptr<IRhiCommandEncoder> encoder = m_device->CreateCommandEncoder(RhiQueueType::Graphics);
        if (!encoder)
        {
            outError = "GpuSolidVoxelizer: failed to create a command encoder";
            return false;
        }

        const auto bindDescriptorTables = [&]()
        {
            for (uint32_t i = 0; i < kBindingCount; ++i)
            {
                encoder->SetComputeDescriptorTable(i, RhiGpuDescriptorHandle{ m_descriptors.gpu.ptr + i * increment });
            }
        };

        encoder->BeginDebugLabel("Voxel.SolidVoxelize");
        encoder->SetDescriptorHeap(m_descriptors.heap);
        encoder->SetComputePipelineLayout(m_bindableLayout);
        encoder->SetComputePipeline(m_bindableCrossingsPipeline);
        bindDescriptorTables();

        RhiDispatchDesc dispatch{};
        dispatch.groupCountX = crossingsShape.groupsX;
        dispatch.groupCountY = crossingsShape.groupsY;
        dispatch.groupCountZ = 1;
        encoder->Dispatch(dispatch);

        encoder->UavBarrier(&bitsBuffer, 1);

        encoder->SetComputePipeline(m_bindablePrefixPipeline);
        bindDescriptorTables();

        dispatch.groupCountX = prefixShape.groupsX;
        dispatch.groupCountY = prefixShape.groupsY;
        dispatch.groupCountZ = 1;
        encoder->Dispatch(dispatch);

        encoder->UavBarrier(&bitsBuffer, 1);

        RhiResourceTransitionDesc toCopy{};
        toCopy.resource = bitsBuffer;
        toCopy.before = RhiResourceState::UnorderedAccess;
        toCopy.after = RhiResourceState::CopySource;
        encoder->TransitionResources(&toCopy, 1);

        RhiBufferCopyDesc copy{};
        copy.source = bitsBuffer;
        copy.destination = readbackBuffer;
        copy.sizeInBytes = bitsBytes;
        encoder->CopyBuffer(copy);
        encoder->EndDebugLabel();

        if (!m_device->SubmitCommandEncoder(*encoder, RhiQueueType::Graphics))
        {
            outError = "GpuSolidVoxelizer: failed to submit the command encoder";
            return false;
        }
        if (!m_device->WaitForRhiIdle())
        {
            outError = "GpuSolidVoxelizer: waiting for the GPU failed";
            return false;
        }

        std::vector<uint32_t> words(static_cast<size_t>(wordCount), 0u);
        if (!m_device->ReadRhiBuffer(readbackBuffer, 0, words.data(), bitsBytes))
        {
            outError = "GpuSolidVoxelizer: failed to read back the voxel grid";
            return false;
        }

        for (uint32_t z = 0; z < desc.dimZ; ++z)
        {
            for (uint32_t y = 0; y < desc.dimY; ++y)
            {
                const uint64_t row = static_cast<uint64_t>(y) + static_cast<uint64_t>(z) * desc.dimY;
                const uint32_t* rowData = words.data() + row * rowWords;
                for (uint32_t w = 0; w < rowWords; ++w)
                {
                    if (rowData[w] == 0)
                    {
                        continue;
                    }
                    const uint32_t xBegin = w * 32;
                    const uint32_t xEnd = (std::min)(xBegin + 32, desc.dimX);
                    for (uint32_t x = xBegin; x < xEnd; ++x)
                    {
                        if (((rowData[w] >> (x & 31)) & 1u) != 0)
                        {
                            outGrid.Set(x, y, z);
                        }
                    }
                }
            }
        }
        return true;
    }
}
