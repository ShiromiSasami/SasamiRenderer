#pragma once

// Dx12ConversionUtils.h
// RHI -> D3D12/DXGI enum conversion helpers shared by the DirectX12 backend
// translation units. These previously lived as byte-identical copies in the
// anonymous namespace of Dx12GraphicsDevice.cpp, Dx12GraphicsDeviceInit.cpp and
// Dx12GraphicsDeviceResource.cpp; a new RhiFormat or RhiResourceState added to
// only some of those copies would still compile and silently pick the wrong
// native format at runtime. Mirrors the VulkanGraphicsDeviceUtils.h convention
// already used by the Vulkan backend.

#include "Renderer/Backends/DirectX12/Dx12GraphicsDevice.h"

#if RHI_DIRECTX12

namespace SasamiRenderer
{
    inline DXGI_FORMAT ToDxgiFormat(RhiFormat format)
    {
        switch (format) {
        case RhiFormat::R8UNorm: return DXGI_FORMAT_R8_UNORM;
        case RhiFormat::R8G8B8A8UNorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case RhiFormat::B8G8R8A8UNorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case RhiFormat::R16G16B16A16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case RhiFormat::R32G32B32Float: return DXGI_FORMAT_R32G32B32_FLOAT;
        case RhiFormat::R32G32Float: return DXGI_FORMAT_R32G32_FLOAT;
        case RhiFormat::R16Float: return DXGI_FORMAT_R16_FLOAT;
        case RhiFormat::R16Typeless: return DXGI_FORMAT_R16_TYPELESS;
        case RhiFormat::R16UNorm: return DXGI_FORMAT_R16_UNORM;
        case RhiFormat::R32Float: return DXGI_FORMAT_R32_FLOAT;
        case RhiFormat::R32UInt: return DXGI_FORMAT_R32_UINT;
        case RhiFormat::R32Typeless: return DXGI_FORMAT_R32_TYPELESS;
        case RhiFormat::D16UNorm: return DXGI_FORMAT_D16_UNORM;
        case RhiFormat::D32Float: return DXGI_FORMAT_D32_FLOAT;
        case RhiFormat::D24UNormS8UInt: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }

    inline D3D12_RESOURCE_STATES ToDx12State(RhiResourceState state)
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

    inline D3D12_HEAP_TYPE ToDx12HeapType(RhiMemoryUsage usage)
    {
        switch (usage) {
        case RhiMemoryUsage::CpuToGpu: return D3D12_HEAP_TYPE_UPLOAD;
        case RhiMemoryUsage::GpuToCpu: return D3D12_HEAP_TYPE_READBACK;
        case RhiMemoryUsage::GpuOnly:
        default: return D3D12_HEAP_TYPE_DEFAULT;
        }
    }

    inline D3D12_RESOURCE_FLAGS ToDx12TextureFlags(RhiTextureUsageFlags usage)
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

    inline D3D12_SRV_DIMENSION ToDx12SrvDimension(RhiTextureViewDimension dimension)
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

    inline D3D12_DESCRIPTOR_HEAP_TYPE ToDx12DescriptorHeapType(RhiDescriptorHeapType type)
    {
        switch (type) {
        case RhiDescriptorHeapType::Sampler: return D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        case RhiDescriptorHeapType::RenderTarget: return D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        case RhiDescriptorHeapType::DepthStencil: return D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        case RhiDescriptorHeapType::CbvSrvUav:
        default: return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        }
    }

    inline D3D12_COMMAND_LIST_TYPE ToDx12CommandListType(RhiQueueType queueType)
    {
        switch (queueType) {
        case RhiQueueType::Compute: return D3D12_COMMAND_LIST_TYPE_COMPUTE;
        case RhiQueueType::Copy: return D3D12_COMMAND_LIST_TYPE_COPY;
        case RhiQueueType::Graphics:
        case RhiQueueType::Present:
        default: return D3D12_COMMAND_LIST_TYPE_DIRECT;
        }
    }

    inline D3D12_SHADER_VISIBILITY ToDx12ShaderVisibility(RhiShaderStageFlags visibility)
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

    inline D3D12_DESCRIPTOR_RANGE_TYPE ToDx12DescriptorRangeType(RhiBindingType type)
    {
        switch (type) {
        case RhiBindingType::ConstantBuffer: return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        case RhiBindingType::UnorderedAccess: return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        case RhiBindingType::Sampler: return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        case RhiBindingType::ShaderResource:
        default: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        }
    }

    inline D3D12_ROOT_PARAMETER_TYPE ToDx12RootDescriptorType(RhiBindingType type)
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

    inline D3D12_COMPARISON_FUNC ToDx12CompareOp(RhiCompareOp op)
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

    inline D3D12_PRIMITIVE_TOPOLOGY_TYPE ToDx12PrimitiveTopologyType(RhiPrimitiveTopology topology)
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

    inline D3D_PRIMITIVE_TOPOLOGY ToDx12PrimitiveTopology(RhiPrimitiveTopology topology)
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

    inline D3D12_CULL_MODE ToDx12CullMode(RhiCullMode mode)
    {
        switch (mode) {
        case RhiCullMode::None: return D3D12_CULL_MODE_NONE;
        case RhiCullMode::Front: return D3D12_CULL_MODE_FRONT;
        case RhiCullMode::Back:
        default: return D3D12_CULL_MODE_BACK;
        }
    }

    inline D3D12_FILL_MODE ToDx12FillMode(RhiFillMode mode)
    {
        return mode == RhiFillMode::Wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    }
}

#endif // RHI_DIRECTX12
