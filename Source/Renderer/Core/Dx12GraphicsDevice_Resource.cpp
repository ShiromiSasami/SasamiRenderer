// Dx12GraphicsDevice_Resource.cpp
// RHI resource creation: textures, buffers, shaders, pipeline layouts.
#include "Renderer/Core/Dx12GraphicsDevice.h"
#include "Renderer/Core/Dx11GraphicsDevice.h"
#include "Renderer/Core/OpenGLGraphicsDevice.h"
#include "Renderer/Core/VulkanGraphicsDevice.h"
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
    namespace
    {
        DXGI_FORMAT ToDxgiFormat(RhiFormat format)
        {
            switch (format) {
            case RhiFormat::R8UNorm: return DXGI_FORMAT_R8_UNORM;
            case RhiFormat::R8G8B8A8UNorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case RhiFormat::B8G8R8A8UNorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case RhiFormat::R16G16B16A16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case RhiFormat::R32G32Float: return DXGI_FORMAT_R32G32_FLOAT;
            case RhiFormat::R32Float: return DXGI_FORMAT_R32_FLOAT;
            case RhiFormat::R32UInt: return DXGI_FORMAT_R32_UINT;
            case RhiFormat::D32Float: return DXGI_FORMAT_D32_FLOAT;
            case RhiFormat::D24UNormS8UInt: return DXGI_FORMAT_D24_UNORM_S8_UINT;
            default: return DXGI_FORMAT_UNKNOWN;
            }
        }

        D3D12_RESOURCE_STATES ToDx12State(RhiResourceState state)
        {
            switch (state) {
            case RhiResourceState::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case RhiResourceState::DepthWrite: return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            case RhiResourceState::DepthRead: return D3D12_RESOURCE_STATE_DEPTH_READ;
            case RhiResourceState::ShaderResource:
                return static_cast<D3D12_RESOURCE_STATES>(
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            case RhiResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            case RhiResourceState::CopySource: return D3D12_RESOURCE_STATE_COPY_SOURCE;
            case RhiResourceState::CopyDest: return D3D12_RESOURCE_STATE_COPY_DEST;
            case RhiResourceState::Present: return D3D12_RESOURCE_STATE_PRESENT;
            case RhiResourceState::Common:
            default: return D3D12_RESOURCE_STATE_COMMON;
            }
        }

        D3D12_HEAP_TYPE ToDx12HeapType(RhiMemoryUsage usage)
        {
            switch (usage) {
            case RhiMemoryUsage::CpuToGpu: return D3D12_HEAP_TYPE_UPLOAD;
            case RhiMemoryUsage::GpuToCpu: return D3D12_HEAP_TYPE_READBACK;
            case RhiMemoryUsage::GpuOnly:
            default: return D3D12_HEAP_TYPE_DEFAULT;
            }
        }

        D3D12_RESOURCE_FLAGS ToDx12TextureFlags(RhiTextureUsageFlags usage)
        {
            UINT flags = D3D12_RESOURCE_FLAG_NONE;
            if (HasFlag(usage, RhiTextureUsageFlags::RenderTarget)) {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            }
            if (HasFlag(usage, RhiTextureUsageFlags::DepthStencil)) {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            }
            if (HasFlag(usage, RhiTextureUsageFlags::UnorderedAccess)) {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }
            return static_cast<D3D12_RESOURCE_FLAGS>(flags);
        }

        D3D12_SRV_DIMENSION ToDx12SrvDimension(RhiTextureViewDimension dimension)
        {
            switch (dimension) {
            case RhiTextureViewDimension::Texture1D: return D3D12_SRV_DIMENSION_TEXTURE1D;
            case RhiTextureViewDimension::Texture1DArray: return D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
            case RhiTextureViewDimension::Texture2DArray: return D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            case RhiTextureViewDimension::Texture3D: return D3D12_SRV_DIMENSION_TEXTURE3D;
            case RhiTextureViewDimension::TextureCube: return D3D12_SRV_DIMENSION_TEXTURECUBE;
            case RhiTextureViewDimension::TextureCubeArray: return D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            case RhiTextureViewDimension::Texture2D:
            default: return D3D12_SRV_DIMENSION_TEXTURE2D;
            }
        }

        D3D12_DESCRIPTOR_HEAP_TYPE ToDx12DescriptorHeapType(RhiDescriptorHeapType type)
        {
            switch (type) {
            case RhiDescriptorHeapType::Sampler: return D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            case RhiDescriptorHeapType::RenderTarget: return D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            case RhiDescriptorHeapType::DepthStencil: return D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            case RhiDescriptorHeapType::CbvSrvUav:
            default: return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            }
        }

        D3D12_COMMAND_LIST_TYPE ToDx12CommandListType(RhiQueueType queueType)
        {
            switch (queueType) {
            case RhiQueueType::Compute: return D3D12_COMMAND_LIST_TYPE_COMPUTE;
            case RhiQueueType::Copy: return D3D12_COMMAND_LIST_TYPE_COPY;
            case RhiQueueType::Graphics:
            case RhiQueueType::Present:
            default: return D3D12_COMMAND_LIST_TYPE_DIRECT;
            }
        }

        D3D12_SHADER_VISIBILITY ToDx12ShaderVisibility(RhiShaderStageFlags visibility)
        {
            const uint32_t flags = static_cast<uint32_t>(visibility);
            if (flags == static_cast<uint32_t>(RhiShaderStageFlags::Vertex)) {
                return D3D12_SHADER_VISIBILITY_VERTEX;
            }
            if (flags == static_cast<uint32_t>(RhiShaderStageFlags::Pixel)) {
                return D3D12_SHADER_VISIBILITY_PIXEL;
            }
            if (flags == static_cast<uint32_t>(RhiShaderStageFlags::Compute)) {
                return D3D12_SHADER_VISIBILITY_ALL;
            }
            return D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_DESCRIPTOR_RANGE_TYPE ToDx12DescriptorRangeType(RhiBindingType type)
        {
            switch (type) {
            case RhiBindingType::ConstantBuffer: return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
            case RhiBindingType::UnorderedAccess: return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            case RhiBindingType::Sampler: return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            case RhiBindingType::ShaderResource:
            default: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            }
        }

        D3D12_ROOT_PARAMETER_TYPE ToDx12RootDescriptorType(RhiBindingType type)
        {
            switch (type) {
            case RhiBindingType::ConstantBuffer: return D3D12_ROOT_PARAMETER_TYPE_CBV;
            case RhiBindingType::UnorderedAccess: return D3D12_ROOT_PARAMETER_TYPE_UAV;
            case RhiBindingType::ShaderResource: return D3D12_ROOT_PARAMETER_TYPE_SRV;
            case RhiBindingType::RootConstants: return D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            case RhiBindingType::Sampler:
            default: return D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            }
        }

        D3D12_COMPARISON_FUNC ToDx12CompareOp(RhiCompareOp op)
        {
            switch (op) {
            case RhiCompareOp::Never: return D3D12_COMPARISON_FUNC_NEVER;
            case RhiCompareOp::Less: return D3D12_COMPARISON_FUNC_LESS;
            case RhiCompareOp::Equal: return D3D12_COMPARISON_FUNC_EQUAL;
            case RhiCompareOp::LessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
            case RhiCompareOp::Greater: return D3D12_COMPARISON_FUNC_GREATER;
            case RhiCompareOp::NotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
            case RhiCompareOp::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            case RhiCompareOp::Always:
            default: return D3D12_COMPARISON_FUNC_ALWAYS;
            }
        }

        D3D12_PRIMITIVE_TOPOLOGY_TYPE ToDx12PrimitiveTopologyType(RhiPrimitiveTopology topology)
        {
            switch (topology) {
            case RhiPrimitiveTopology::LineList:
            case RhiPrimitiveTopology::LineStrip:
                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            case RhiPrimitiveTopology::PointList:
                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            case RhiPrimitiveTopology::PatchList:
                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
            case RhiPrimitiveTopology::TriangleList:
            case RhiPrimitiveTopology::TriangleStrip:
            default:
                return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            }
        }

        D3D_PRIMITIVE_TOPOLOGY ToDx12PrimitiveTopology(RhiPrimitiveTopology topology)
        {
            switch (topology) {
            case RhiPrimitiveTopology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            case RhiPrimitiveTopology::LineList: return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            case RhiPrimitiveTopology::LineStrip: return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            case RhiPrimitiveTopology::PointList: return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            case RhiPrimitiveTopology::PatchList: return D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
            case RhiPrimitiveTopology::TriangleList:
            default: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            }
        }

        D3D12_CULL_MODE ToDx12CullMode(RhiCullMode mode)
        {
            switch (mode) {
            case RhiCullMode::None: return D3D12_CULL_MODE_NONE;
            case RhiCullMode::Front: return D3D12_CULL_MODE_FRONT;
            case RhiCullMode::Back:
            default: return D3D12_CULL_MODE_BACK;
            }
        }

        D3D12_FILL_MODE ToDx12FillMode(RhiFillMode mode)
        {
            return mode == RhiFillMode::Wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
        }

    }


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
        dxDesc.Flags = ToDx12TextureFlags(desc.usage);

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

    RhiBufferHandle Dx12GraphicsDevice::CreateRhiBuffer(const RhiBufferDesc& desc, const void* initialData)
    {
        if (!m_device || desc.sizeInBytes == 0) {
            return {};
        }

        D3D12_RESOURCE_DESC dxDesc = CD3DX12_RESOURCE_DESC::Buffer(desc.sizeInBytes);
        if (HasFlag(desc.usage, RhiBufferUsageFlags::UnorderedAccess) ||
            HasFlag(desc.usage, RhiBufferUsageFlags::AccelerationStructure)) {
            dxDesc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
                static_cast<UINT>(dxDesc.Flags) | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        }

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = ToDx12HeapType(desc.memoryUsage);

        Resource resource;
        const D3D12_RESOURCE_STATES initialState =
            desc.memoryUsage == RhiMemoryUsage::CpuToGpu ? D3D12_RESOURCE_STATE_GENERIC_READ : ToDx12State(desc.initialState);
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

        return StoreRhiResource(std::move(resource));
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


#endif
} // namespace SasamiRenderer
