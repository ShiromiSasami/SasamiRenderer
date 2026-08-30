// Dx12GraphicsDeviceResource.cpp
// RHI resource creation: textures, buffers, shaders, pipeline layouts.
#include "Renderer/Backends/DirectX12/Dx12GraphicsDevice.h"
#include "Renderer/Backends/DirectX12/Dx12ConversionUtils.h"
#include "Renderer/Backends/DirectX11/Dx11GraphicsDevice.h"
#include "Renderer/Backends/OpenGL/OpenGLGraphicsDevice.h"
#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"
#include <windows.h>
#include <debugapi.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstring>
#include <utility>

#include "Foundation/Tools/DebugOutput.h"


namespace SasamiRenderer
{
#if RHI_DIRECTX12


    RhiTextureHandle Dx12GraphicsDevice::CreateRhiTexture(const RhiTextureDesc& desc)
    {
        if (!m_device || desc.extent.width == 0 || desc.extent.height == 0 || desc.extent.depth == 0) {
            return {};
        }

        D3D12_RESOURCE_DESC dxDesc{};
        switch (desc.dimension) {
        case RhiResourceDimension::Texture1D:
            dxDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
            break;
        case RhiResourceDimension::Texture3D:
            dxDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            break;
        case RhiResourceDimension::Texture2D:
        default:
            dxDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            break;
        }
        dxDesc.Width = desc.extent.width;
        dxDesc.Height = desc.extent.height;
        dxDesc.DepthOrArraySize = desc.dimension == RhiResourceDimension::Texture3D
            ? static_cast<UINT16>(desc.extent.depth)
            : static_cast<UINT16>(desc.arrayLayers);
        dxDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
        dxDesc.Format = ToDxgiFormat(desc.format);
        dxDesc.SampleDesc.Count = 1;
        dxDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        dxDesc.Flags = desc.memoryUsage == RhiMemoryUsage::GpuOnly
            ? ToDx12TextureFlags(desc.usage)
            : D3D12_RESOURCE_FLAG_NONE;

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = ToDx12HeapType(desc.memoryUsage);

        Resource resource;
        const HRESULT hr = CreateCommittedResource(&heap,
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &dxDesc,
                                                   ToDx12State(desc.initialState),
                                                   nullptr,
                                                   resource);
        if (FAILED(hr)) {
            return {};
        }
        return StoreRhiResource(std::move(resource));
    }

    RhiTextureHandle Dx12GraphicsDevice::CreateRhiTexture2DFromRgba8(uint32_t width,
                                                                     uint32_t height,
                                                                     const void* pixels,
                                                                     uint32_t rowPitchBytes)
    {
        if (!m_device || width == 0 || height == 0 || !pixels || rowPitchBytes < width * 4u) {
            return {};
        }

        D3D12_RESOURCE_DESC textureDesc{};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Width = width;
        textureDesc.Height = height;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = 1;
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
        Resource texture;
        HRESULT hr = CreateCommittedResource(&defaultHeap,
                                             D3D12_HEAP_FLAG_NONE,
                                             &textureDesc,
                                             D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr,
                                             texture);
        if (FAILED(hr)) {
            return {};
        }

        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(texture.Get(), 0, 1);
        CD3DX12_HEAP_PROPERTIES uploadHeap(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
        Resource upload;
        hr = CreateCommittedResource(&uploadHeap,
                                     D3D12_HEAP_FLAG_NONE,
                                     &uploadDesc,
                                     D3D12_RESOURCE_STATE_GENERIC_READ,
                                     nullptr,
                                     upload);
        if (FAILED(hr)) {
            return {};
        }

        CommandAllocator allocator;
        CommandList commandList;
        hr = CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, allocator);
        if (FAILED(hr)) {
            return {};
        }
        hr = CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, commandList);
        if (FAILED(hr)) {
            return {};
        }

        D3D12_SUBRESOURCE_DATA textureData{};
        textureData.pData = pixels;
        textureData.RowPitch = rowPitchBytes;
        textureData.SlicePitch = static_cast<LONG_PTR>(rowPitchBytes) * static_cast<LONG_PTR>(height);
        UpdateSubresources(commandList.Get(), texture.Get(), upload.Get(), 0, 0, 1, &textureData);
        ResourceBarrier barrier = CD3DX12_RESOURCE_BARRIER::Transition(texture.Get(),
                                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList.ResourceBarrier(1, &barrier);
        hr = commandList.Close();
        if (FAILED(hr)) {
            return {};
        }

        ID3D12CommandList* lists[] = { commandList.Get() };
        m_commandQueue.ExecuteCommandLists(1, lists);
        WaitForGPU();

        return StoreRhiResource(std::move(texture));
    }

    RhiBufferHandle Dx12GraphicsDevice::CreateRhiBuffer(const RhiBufferDesc& desc, const void* initialData)
    {
        if (!m_device || desc.sizeInBytes == 0 ||
            (initialData && desc.memoryUsage == RhiMemoryUsage::GpuToCpu)) {
            return {};
        }

        D3D12_RESOURCE_DESC dxDesc = CD3DX12_RESOURCE_DESC::Buffer(desc.sizeInBytes);
        if (desc.memoryUsage == RhiMemoryUsage::GpuOnly &&
            (HasFlag(desc.usage, RhiBufferUsageFlags::UnorderedAccess) ||
             HasFlag(desc.usage, RhiBufferUsageFlags::AccelerationStructure))) {
            dxDesc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
                static_cast<UINT>(dxDesc.Flags) | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        }

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = ToDx12HeapType(desc.memoryUsage);

        Resource resource;
        const bool needsStagingUpload = initialData && desc.memoryUsage == RhiMemoryUsage::GpuOnly;
        const D3D12_RESOURCE_STATES finalState = ToDx12State(desc.initialState);
        const D3D12_RESOURCE_STATES initialState = desc.memoryUsage == RhiMemoryUsage::CpuToGpu
            ? D3D12_RESOURCE_STATE_GENERIC_READ
            : (desc.memoryUsage == RhiMemoryUsage::GpuToCpu
                ? D3D12_RESOURCE_STATE_COPY_DEST
                : (needsStagingUpload ? D3D12_RESOURCE_STATE_COPY_DEST : finalState));
        const HRESULT hr = CreateCommittedResource(&heap,
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &dxDesc,
                                                   initialState,
                                                   nullptr,
                                                   resource);
        if (FAILED(hr)) {
            return {};
        }

        if (initialData && desc.memoryUsage == RhiMemoryUsage::CpuToGpu) {
            void* mapped = nullptr;
            const D3D12_RANGE emptyRange{ 0, 0 };
            if (SUCCEEDED(resource.Map(0, &emptyRange, &mapped)) && mapped) {
                std::memcpy(mapped, initialData, static_cast<size_t>(desc.sizeInBytes));
                resource.Unmap(0, nullptr);
            }
        }
        if (needsStagingUpload) {
            D3D12_HEAP_PROPERTIES uploadHeap{};
            uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC stagingDesc = dxDesc;
            stagingDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
            Resource upload;
            if (FAILED(CreateCommittedResource(&uploadHeap,
                                               D3D12_HEAP_FLAG_NONE,
                                               &stagingDesc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr,
                                               upload))) {
                return {};
            }

            void* mapped = nullptr;
            const D3D12_RANGE emptyRange{ 0, 0 };
            if (FAILED(upload.Map(0, &emptyRange, &mapped)) || !mapped) {
                return {};
            }
            std::memcpy(mapped, initialData, static_cast<size_t>(desc.sizeInBytes));
            upload.Unmap(0, nullptr);

            CommandAllocator allocator;
            CommandList commandList;
            if (FAILED(CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, allocator)) ||
                FAILED(CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, commandList))) {
                return {};
            }
            commandList.Get()->CopyBufferRegion(resource.Get(), 0, upload.Get(), 0, desc.sizeInBytes);
            if (finalState != D3D12_RESOURCE_STATE_COPY_DEST) {
                ResourceBarrier barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource.Get(),
                                                                               D3D12_RESOURCE_STATE_COPY_DEST,
                                                                               finalState);
                commandList.ResourceBarrier(1, &barrier);
            }
            if (FAILED(commandList.Close())) {
                return {};
            }
            ID3D12CommandList* lists[] = { commandList.Get() };
            m_commandQueue.ExecuteCommandLists(1, lists);
            WaitForGPU();
        }

        return StoreRhiResource(std::move(resource));
    }

    bool Dx12GraphicsDevice::UpdateRhiBuffer(RhiBufferHandle bufferHandle,
                                             uint64_t offsetInBytes,
                                             const void* data,
                                             uint64_t sizeInBytes)
    {
        Resource* resource = FindRhiResource(bufferHandle);
        if (!resource || !data || sizeInBytes == 0) {
            return false;
        }

        const uint64_t bufferSize = resource->Get()->GetDesc().Width;
        if (offsetInBytes >= bufferSize || sizeInBytes > bufferSize - offsetInBytes) {
            return false;
        }

        void* mapped = nullptr;
        const D3D12_RANGE readRange{ 0, 0 };
        if (FAILED(resource->Map(0, &readRange, &mapped)) || !mapped) {
            return false;
        }
        std::memcpy(static_cast<uint8_t*>(mapped) + offsetInBytes,
                    data,
                    static_cast<size_t>(sizeInBytes));
        const D3D12_RANGE writtenRange{
            static_cast<SIZE_T>(offsetInBytes),
            static_cast<SIZE_T>(offsetInBytes + sizeInBytes),
        };
        resource->Unmap(0, &writtenRange);
        return true;
    }

    bool Dx12GraphicsDevice::ReadRhiBuffer(RhiBufferHandle bufferHandle,
                                           uint64_t offsetInBytes,
                                           void* data,
                                           uint64_t sizeInBytes)
    {
        Resource* resource = FindRhiResource(bufferHandle);
        if (!resource || !data || sizeInBytes == 0) {
            return false;
        }

        const uint64_t bufferSize = resource->Get()->GetDesc().Width;
        if (offsetInBytes >= bufferSize || sizeInBytes > bufferSize - offsetInBytes) {
            return false;
        }

        void* mapped = nullptr;
        const D3D12_RANGE readRange{
            static_cast<SIZE_T>(offsetInBytes),
            static_cast<SIZE_T>(offsetInBytes + sizeInBytes),
        };
        if (FAILED(resource->Map(0, &readRange, &mapped)) || !mapped) {
            return false;
        }
        std::memcpy(data,
                    static_cast<const uint8_t*>(mapped) + offsetInBytes,
                    static_cast<size_t>(sizeInBytes));
        const D3D12_RANGE noWriteRange{ 0, 0 };
        resource->Unmap(0, &noWriteRange);
        return true;
    }

    bool Dx12GraphicsDevice::DestroyRhiResource(RhiResourceHandle resource)
    {
        if (!resource.IsValid()) {
            return false;
        }
        // TDR調査用の一時計装: RhiHandle型の非型安全性(全リソース種別が同一のプレーン構造体を
        // usingエイリアスしているだけで、コンパイラは所有者違いのハンドル取り違えを検出できない)
        // による誤破棄を疑っており、次回症状再現時にどのハンドルIDのリソースが・どのサイズ/種別で
        // 破棄されたかをログから直接追跡できるようにする。
        const auto it = m_rhiResources.find(resource.id);
        if (it != m_rhiResources.end() && it->second.Get()) {
            const D3D12_RESOURCE_DESC desc = it->second.Get()->GetDesc();
            char logBuf[224];
            snprintf(logBuf, sizeof(logBuf),
                     "[RhiResourceTrace] destroyed handleId=%llu addr=%p gpuVA=0x%llX dim=%d width=%llu height=%u format=%d\n",
                     static_cast<unsigned long long>(resource.id),
                     static_cast<void*>(it->second.Get()),
                     static_cast<unsigned long long>(it->second.Get()->GetGPUVirtualAddress()),
                     static_cast<int>(desc.Dimension),
                     static_cast<unsigned long long>(desc.Width),
                     static_cast<unsigned int>(desc.Height),
                     static_cast<int>(desc.Format));
            DebugLog(logBuf);
        }
        return m_rhiResources.erase(resource.id) != 0;
    }

    RhiShaderHandle Dx12GraphicsDevice::CreateRhiShaderModule(const RhiShaderModuleDesc& desc)
    {
        if (!desc.bytecode || desc.bytecodeSize == 0) {
            return {};
        }

        RhiShaderModule shader{};
        shader.stage = desc.stage;
        shader.bytecode.resize(static_cast<size_t>(desc.bytecodeSize));
        std::memcpy(shader.bytecode.data(), desc.bytecode, static_cast<size_t>(desc.bytecodeSize));

        const uint64_t id = m_nextRhiShaderHandle++;
        m_rhiShaders.emplace(id, std::move(shader));
        return RhiShaderHandle{ id };
    }

    RhiPipelineLayoutHandle Dx12GraphicsDevice::CreateRhiPipelineLayout(const RhiPipelineLayoutDesc& desc)
    {
        if (!m_device) {
            return {};
        }

        std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
        std::vector<D3D12_ROOT_PARAMETER> parameters;
        ranges.reserve(desc.bindingCount);
        parameters.reserve(desc.bindingCount);

        for (uint32_t i = 0; i < desc.bindingCount; ++i) {
            const RhiBindingRangeDesc& binding = desc.bindings[i];
            D3D12_ROOT_PARAMETER parameter{};
            parameter.ShaderVisibility = ToDx12ShaderVisibility(binding.visibility);

            if (binding.type == RhiBindingType::RootConstants) {
                parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                parameter.Constants.ShaderRegister = binding.baseRegister;
                parameter.Constants.RegisterSpace = binding.registerSpace;
                parameter.Constants.Num32BitValues = binding.rootConstantCount;
            } else if (binding.inlineRootDescriptor && binding.type != RhiBindingType::Sampler) {
                parameter.ParameterType = ToDx12RootDescriptorType(binding.type);
                parameter.Descriptor.ShaderRegister = binding.baseRegister;
                parameter.Descriptor.RegisterSpace = binding.registerSpace;
            } else {
                ranges.push_back({});
                D3D12_DESCRIPTOR_RANGE& range = ranges.back();
                range.RangeType = ToDx12DescriptorRangeType(binding.type);
                range.NumDescriptors = binding.descriptorCount;
                range.BaseShaderRegister = binding.baseRegister;
                range.RegisterSpace = binding.registerSpace;
                range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

                parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                parameter.DescriptorTable.NumDescriptorRanges = 1;
                parameter.DescriptorTable.pDescriptorRanges = &ranges.back();
            }
            parameters.push_back(parameter);
        }

        std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(desc.staticSamplerCount);
        for (uint32_t i = 0; i < desc.staticSamplerCount; ++i) {
            const RhiStaticSamplerDesc& src = desc.staticSamplers[i];
            D3D12_STATIC_SAMPLER_DESC& dst = samplers[i];
            dst.Filter = src.linearFilter ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            dst.AddressU = src.clamp ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            dst.AddressV = dst.AddressU;
            dst.AddressW = dst.AddressU;
            dst.ShaderRegister = src.shaderRegister;
            dst.RegisterSpace = src.registerSpace;
            dst.ShaderVisibility = ToDx12ShaderVisibility(src.visibility);
        }

        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = static_cast<UINT>(parameters.size());
        rootDesc.pParameters = parameters.empty() ? nullptr : parameters.data();
        rootDesc.NumStaticSamplers = static_cast<UINT>(samplers.size());
        rootDesc.pStaticSamplers = samplers.empty() ? nullptr : samplers.data();
        rootDesc.Flags = desc.allowInputAssembler
            ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
            : D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> error;
        HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (FAILED(hr)) {
            if (error && error->GetBufferPointer()) {
                DebugLog(static_cast<const char*>(error->GetBufferPointer()));
                DebugLog("\n");
            }
            return {};
        }

        RootSignature rootSignature;
        hr = CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), rootSignature);
        if (FAILED(hr)) {
            return {};
        }

        const uint64_t id = m_nextRhiPipelineLayoutHandle++;
        m_rhiPipelineLayouts.emplace(id, std::move(rootSignature));
        return RhiPipelineLayoutHandle{ id };
    }

    // Create*Pipeline returns handles whose id is an internal map key. Command encoders bind by
    // treating handle.id as the raw native pointer (ID3D12RootSignature*/ID3D12PipelineState*),
    // matching RenderPipelineStateCache::MakeLayoutHandle/MakePipelineHandle. Translate here so
    // callers that create via the device path can still bind through the encoder.
    RhiPipelineLayoutHandle Dx12GraphicsDevice::GetBindablePipelineLayoutHandle(RhiPipelineLayoutHandle handle)
    {
        const auto it = m_rhiPipelineLayouts.find(handle.id);
        if (it == m_rhiPipelineLayouts.end()) {
            return {};
        }
        return RhiPipelineLayoutHandle{ reinterpret_cast<uint64_t>(it->second.Get()) };
    }

    RhiPipelineHandle Dx12GraphicsDevice::GetBindablePipelineHandle(RhiPipelineHandle handle)
    {
        const auto it = m_rhiPipelines.find(handle.id);
        if (it == m_rhiPipelines.end()) {
            return {};
        }
        return RhiPipelineHandle{ reinterpret_cast<uint64_t>(it->second.Get()) };
    }


#endif
} // namespace SasamiRenderer
