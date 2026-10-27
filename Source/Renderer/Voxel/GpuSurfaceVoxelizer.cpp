#include "Renderer/Voxel/GpuSurfaceVoxelizer.h"

#include "Renderer/Resources/ShaderCompilationService.h"
#include "Renderer/Voxel/GpuVoxelBuffers.h"

#include <algorithm>
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

        // Mirrors VoxelizeParams in SurfaceVoxelize_CS.hlsl (structured element, 80 bytes).
        struct VoxelizeParamsGpu
        {
            float origin[3];
            float voxelSize;
            uint32_t dim[3];
            uint32_t triangleCount;
            uint32_t vertexCount;
            uint32_t positionStride;
            uint32_t threadsPerRow;
            uint32_t pad;
            uint32_t regionMin[3];
            uint32_t pad1;
            uint32_t regionDim[3];
            uint32_t pad2;
        };
        static_assert(sizeof(VoxelizeParamsGpu) == 80, "VoxelizeParamsGpu must match the HLSL VoxelizeParams layout");
    }

    bool GpuSurfaceVoxelizer::Initialize(IRhiDevice& device, std::string& outError)
    {
        if (m_device != nullptr)
        {
            return true;
        }
        if (device.GetRhiBackendApi() != RhiBackendApi::DirectX12)
        {
            outError = "GpuSurfaceVoxelizer: only the D3D12 backend is supported";
            return false;
        }

        std::vector<uint8_t> bytecode;
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc(
                "Voxel", L"Compute/Voxel/SurfaceVoxelize_CS.hlsl", "CSMain", "cs_6_0", bytecode)
            || bytecode.empty())
        {
            outError = "GpuSurfaceVoxelizer: failed to compile SurfaceVoxelize_CS.hlsl";
            return false;
        }

        RhiShaderModuleDesc shaderDesc{};
        shaderDesc.bytecode = bytecode.data();
        shaderDesc.bytecodeSize = bytecode.size();
        shaderDesc.entryPoint = "CSMain";
        shaderDesc.stage = RhiShaderStageFlags::Compute;
        if (!device.CreateRhiShaderModule(shaderDesc).IsValid())
        {
            outError = "GpuSurfaceVoxelizer: failed to create the compute shader module";
            return false;
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
            outError = "GpuSurfaceVoxelizer: failed to create the pipeline layout";
            return false;
        }

        RhiComputePipelineDesc pipelineDesc{};
        pipelineDesc.layout = layout;
        pipelineDesc.shader = shaderDesc;
        const RhiPipelineHandle pipeline = device.CreateRhiComputePipeline(pipelineDesc);
        if (!pipeline.IsValid())
        {
            outError = "GpuSurfaceVoxelizer: failed to create the compute pipeline";
            return false;
        }

        const RhiPipelineLayoutHandle bindableLayout = device.GetBindablePipelineLayoutHandle(layout);
        const RhiPipelineHandle bindablePipeline = device.GetBindablePipelineHandle(pipeline);
        if (!bindableLayout.IsValid() || !bindablePipeline.IsValid())
        {
            outError = "GpuSurfaceVoxelizer: failed to resolve bindable pipeline handles";
            return false;
        }

        const RhiDescriptorAllocation descriptors =
            device.AllocateRhiDescriptors(RhiDescriptorHeapType::CbvSrvUav, kBindingCount, true);
        if (!descriptors.IsValid() || descriptors.heap.id == 0)
        {
            outError = "GpuSurfaceVoxelizer: failed to allocate shader-visible descriptors";
            return false;
        }

        m_bindableLayout = bindableLayout;
        m_bindablePipeline = bindablePipeline;
        m_descriptors = descriptors;
        m_device = &device;
        return true;
    }

    bool GpuSurfaceVoxelizer::Voxelize(const VoxelGridDesc& desc,
                                       const VoxelizerMeshInput& mesh,
                                       OccupancyBitGrid& outGrid,
                                       std::string& outError)
    {
        return VoxelizeRegion(desc, FullVoxelRegion(desc), mesh, outGrid, outError);
    }

    bool GpuSurfaceVoxelizer::VoxelizeRegion(const VoxelGridDesc& desc,
                                             const VoxelRegion& region,
                                             const VoxelizerMeshInput& mesh,
                                             OccupancyBitGrid& outGrid,
                                             std::string& outError)
    {
        if (m_device == nullptr)
        {
            outError = "GpuSurfaceVoxelizer: not initialized";
            return false;
        }
        if (desc.dimX == 0 || desc.dimY == 0 || desc.dimZ == 0 || !(desc.voxelSize > 0.0f))
        {
            outError = "GpuSurfaceVoxelizer: invalid grid description";
            return false;
        }
        if (mesh.positions == nullptr || mesh.positionStrideInFloats < 3)
        {
            outError = "GpuSurfaceVoxelizer: invalid mesh input";
            return false;
        }

        const uint32_t gridDim[3] = { desc.dimX, desc.dimY, desc.dimZ };
        bool regionValid = !region.IsEmpty();
        for (int axis = 0; axis < 3 && regionValid; ++axis)
        {
            regionValid = static_cast<uint64_t>(region.min[axis]) + region.size[axis] <= gridDim[axis];
        }
        if (!regionValid)
        {
            outError = "GpuSurfaceVoxelizer: region is empty or outside the grid";
            return false;
        }

        outGrid.Reset(region.size[0], region.size[1], region.size[2]);

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

        const uint32_t groups = (triangleCount + kThreadsPerGroup - 1) / kThreadsPerGroup;
        const uint32_t groupsX = (std::min)(groups, kMaxGroupsX);
        const uint32_t groupsY = (groups + groupsX - 1) / groupsX;

        VoxelizeParamsGpu params{};
        std::memcpy(params.origin, desc.origin, sizeof(params.origin));
        params.voxelSize = desc.voxelSize;
        params.dim[0] = desc.dimX;
        params.dim[1] = desc.dimY;
        params.dim[2] = desc.dimZ;
        params.triangleCount = triangleCount;
        params.vertexCount = mesh.vertexCount;
        params.positionStride = mesh.positionStrideInFloats;
        params.threadsPerRow = groupsX * kThreadsPerGroup;
        params.pad = 0;
        params.pad1 = 0;
        params.pad2 = 0;
        for (int axis = 0; axis < 3; ++axis)
        {
            params.regionMin[axis] = region.min[axis];
            params.regionDim[axis] = region.size[axis];
        }

        const uint64_t voxelCount = outGrid.VoxelCount();
        const uint64_t wordCount = (voxelCount + 31) / 32;
        const uint64_t bitsBytes = wordCount * sizeof(uint32_t);
        const uint64_t positionBytes =
            static_cast<uint64_t>(mesh.vertexCount) * mesh.positionStrideInFloats * sizeof(float);
        const uint64_t indexBytes = static_cast<uint64_t>(indices.size()) * sizeof(uint32_t);
        if (positionBytes == 0)
        {
            outError = "GpuSurfaceVoxelizer: mesh has no vertices";
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
            MakeBufferDesc(sizeof(VoxelizeParamsGpu), sizeof(VoxelizeParamsGpu),
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
            outError = "GpuSurfaceVoxelizer: failed to create GPU buffers";
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
                paramsBuffer, MakeStructuredView(sizeof(VoxelizeParamsGpu), sizeof(VoxelizeParamsGpu)),
                { cpuBase + 2 * increment })
            && m_device->CreateRhiBufferUnorderedAccessView(
                bitsBuffer, MakeStructuredView(bitsBytes, sizeof(uint32_t)), { cpuBase + 3 * increment });
        if (!viewsOk)
        {
            outError = "GpuSurfaceVoxelizer: failed to create buffer views";
            return false;
        }

        std::unique_ptr<IRhiCommandEncoder> encoder = m_device->CreateCommandEncoder(RhiQueueType::Graphics);
        if (!encoder)
        {
            outError = "GpuSurfaceVoxelizer: failed to create a command encoder";
            return false;
        }

        encoder->BeginDebugLabel("Voxel.SurfaceVoxelize");
        encoder->SetDescriptorHeap(m_descriptors.heap);
        encoder->SetComputePipelineLayout(m_bindableLayout);
        encoder->SetComputePipeline(m_bindablePipeline);
        for (uint32_t i = 0; i < kBindingCount; ++i)
        {
            encoder->SetComputeDescriptorTable(i, RhiGpuDescriptorHandle{ m_descriptors.gpu.ptr + i * increment });
        }

        RhiDispatchDesc dispatch{};
        dispatch.groupCountX = groupsX;
        dispatch.groupCountY = groupsY;
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
            outError = "GpuSurfaceVoxelizer: failed to submit the command encoder";
            return false;
        }
        if (!m_device->WaitForRhiIdle())
        {
            outError = "GpuSurfaceVoxelizer: waiting for the GPU failed";
            return false;
        }

        std::vector<uint32_t> words(static_cast<size_t>(wordCount), 0u);
        if (!m_device->ReadRhiBuffer(readbackBuffer, 0, words.data(), bitsBytes))
        {
            outError = "GpuSurfaceVoxelizer: failed to read back the voxel grid";
            return false;
        }

        const uint64_t dimX = region.size[0];
        const uint64_t dimXY = dimX * region.size[1];
        for (size_t w = 0; w < words.size(); ++w)
        {
            uint32_t bits = words[w];
            while (bits != 0)
            {
                uint32_t bit = 0;
                while (((bits >> bit) & 1u) == 0)
                {
                    ++bit;
                }
                bits &= bits - 1;

                const uint64_t linear = static_cast<uint64_t>(w) * 32 + bit;
                if (linear >= voxelCount)
                {
                    continue;
                }
                const uint32_t z = static_cast<uint32_t>(linear / dimXY);
                const uint64_t rem = linear - static_cast<uint64_t>(z) * dimXY;
                const uint32_t y = static_cast<uint32_t>(rem / dimX);
                const uint32_t x = static_cast<uint32_t>(rem - static_cast<uint64_t>(y) * dimX);
                outGrid.Set(x, y, z);
            }
        }
        return true;
    }
}
