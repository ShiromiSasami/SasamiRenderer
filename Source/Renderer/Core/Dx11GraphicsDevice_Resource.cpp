// Dx11GraphicsDevice_Resource.cpp
// RHI resource creation, pipeline state, command encoding.
#include "Renderer/Core/Dx11GraphicsDevice.h"

#if RHI_DIRECTX11

#include "Foundation/Tools/DebugOutput.h"

#include <cstring>
#include <utility>


namespace SasamiRenderer
{
    RhiTextureHandle Dx11GraphicsDevice::CreateRhiTexture(const RhiTextureDesc& desc)
    {
        if (!m_device || desc.extent.width == 0 || desc.extent.height == 0) {
            return {};
        }
        if (desc.dimension != RhiResourceDimension::Texture2D) {
            return {};
        }

        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = desc.extent.width;
        textureDesc.Height = desc.extent.height;
        textureDesc.MipLevels = desc.mipLevels;
        textureDesc.ArraySize = desc.arrayLayers;
        textureDesc.Format = ToDx11Format(desc.format);
        textureDesc.SampleDesc.Count = 1;
        textureDesc.Usage = desc.memoryUsage == RhiMemoryUsage::CpuToGpu ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = ToDx11TextureBindFlags(desc.usage);
        textureDesc.CPUAccessFlags = desc.memoryUsage == RhiMemoryUsage::CpuToGpu ? D3D11_CPU_ACCESS_WRITE : 0;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        if (FAILED(m_device->CreateTexture2D(&textureDesc, nullptr, texture.GetAddressOf()))) {
            return {};
        }

        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        if (FAILED(texture.As(&resource))) {
            return {};
        }
        const uint64_t id = m_nextRhiResourceHandle++;
        m_rhiResources.emplace(id, resource);
        return RhiTextureHandle{ id };
    }

    RhiBufferHandle Dx11GraphicsDevice::CreateRhiBuffer(const RhiBufferDesc& desc, const void* initialData)
    {
        if (!m_device || desc.sizeInBytes == 0) {
            return {};
        }

        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = static_cast<UINT>(desc.sizeInBytes);
        bufferDesc.Usage = desc.memoryUsage == RhiMemoryUsage::CpuToGpu ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
        bufferDesc.BindFlags = ToDx11BufferBindFlags(desc.usage);
        bufferDesc.CPUAccessFlags = desc.memoryUsage == RhiMemoryUsage::CpuToGpu ? D3D11_CPU_ACCESS_WRITE : 0;
        if (HasFlag(desc.usage, RhiBufferUsageFlags::Structured)) {
            bufferDesc.MiscFlags |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bufferDesc.StructureByteStride = desc.strideInBytes;
        }

        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = initialData;

        Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
        if (FAILED(m_device->CreateBuffer(&bufferDesc, initialData ? &data : nullptr, buffer.GetAddressOf()))) {
            return {};
        }

        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        if (FAILED(buffer.As(&resource))) {
            return {};
        }
        const uint64_t id = m_nextRhiResourceHandle++;
        m_rhiResources.emplace(id, resource);
        return RhiBufferHandle{ id };
    }

    RhiShaderHandle Dx11GraphicsDevice::CreateRhiShaderModule(const RhiShaderModuleDesc& desc)
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

    RhiPipelineLayoutHandle Dx11GraphicsDevice::CreateRhiPipelineLayout(const RhiPipelineLayoutDesc& desc)
    {
        const uint64_t id = m_nextRhiPipelineLayoutHandle++;
        m_rhiPipelineLayouts.emplace(id, desc.bindingCount);
        return RhiPipelineLayoutHandle{ id };
    }

    RhiPipelineHandle Dx11GraphicsDevice::CreateRhiGraphicsPipeline(const RhiGraphicsPipelineDesc& desc)
    {
        if (!m_device || !desc.layout.IsValid() || m_rhiPipelineLayouts.find(desc.layout.id) == m_rhiPipelineLayouts.end()) {
            return {};
        }

        RhiPipelineStateObjects pipeline{};
        pipeline.topology = ToDx11Topology(desc.topology);

        const uint8_t* vertexShaderBytecode = nullptr;
        size_t vertexShaderBytecodeSize = 0;

        auto applyShader = [&](RhiShaderStageFlags stage, const void* data, uint64_t size) -> bool {
            if (!data || size == 0) {
                return true;
            }
            switch (stage) {
            case RhiShaderStageFlags::Vertex:
                vertexShaderBytecode = static_cast<const uint8_t*>(data);
                vertexShaderBytecodeSize = static_cast<size_t>(size);
                return SUCCEEDED(m_device->CreateVertexShader(data, static_cast<SIZE_T>(size), nullptr, pipeline.vertexShader.GetAddressOf()));
            case RhiShaderStageFlags::Pixel:
                return SUCCEEDED(m_device->CreatePixelShader(data, static_cast<SIZE_T>(size), nullptr, pipeline.pixelShader.GetAddressOf()));
            case RhiShaderStageFlags::Geometry:
                return SUCCEEDED(m_device->CreateGeometryShader(data, static_cast<SIZE_T>(size), nullptr, pipeline.geometryShader.GetAddressOf()));
            case RhiShaderStageFlags::Hull:
                return SUCCEEDED(m_device->CreateHullShader(data, static_cast<SIZE_T>(size), nullptr, pipeline.hullShader.GetAddressOf()));
            case RhiShaderStageFlags::Domain:
                return SUCCEEDED(m_device->CreateDomainShader(data, static_cast<SIZE_T>(size), nullptr, pipeline.domainShader.GetAddressOf()));
            default:
                return true;
            }
        };

        if (desc.shaderHandles) {
            for (uint32_t i = 0; i < desc.shaderHandleCount; ++i) {
                const auto shaderIt = m_rhiShaders.find(desc.shaderHandles[i].id);
                if (shaderIt != m_rhiShaders.end() &&
                    !applyShader(shaderIt->second.stage, shaderIt->second.bytecode.data(), shaderIt->second.bytecode.size())) {
                    return {};
                }
            }
        }
        if (desc.shaders) {
            for (uint32_t i = 0; i < desc.shaderCount; ++i) {
                const RhiShaderModuleDesc& shader = desc.shaders[i];
                if (!applyShader(shader.stage, shader.bytecode, shader.bytecodeSize)) {
                    return {};
                }
            }
        }

        std::vector<D3D11_INPUT_ELEMENT_DESC> inputElements(desc.vertexAttributeCount);
        for (uint32_t i = 0; i < desc.vertexAttributeCount; ++i) {
            const RhiVertexAttributeDesc& src = desc.vertexAttributes[i];
            D3D11_INPUT_ELEMENT_DESC& dst = inputElements[i];
            dst.SemanticName = src.semantic;
            dst.SemanticIndex = src.semanticIndex;
            dst.Format = ToDx11Format(src.format);
            dst.InputSlot = src.binding;
            dst.AlignedByteOffset = src.offset;
            dst.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            dst.InstanceDataStepRate = 0;
            for (uint32_t bindingIndex = 0; bindingIndex < desc.vertexBindingCount; ++bindingIndex) {
                if (desc.vertexBindings[bindingIndex].binding == src.binding &&
                    desc.vertexBindings[bindingIndex].inputRate == RhiInputRate::PerInstance) {
                    dst.InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
                    dst.InstanceDataStepRate = 1;
                    break;
                }
            }
        }
        if (!inputElements.empty() && (!vertexShaderBytecode || vertexShaderBytecodeSize == 0 ||
            FAILED(m_device->CreateInputLayout(inputElements.data(),
                                               static_cast<UINT>(inputElements.size()),
                                               vertexShaderBytecode,
                                               vertexShaderBytecodeSize,
                                               pipeline.inputLayout.GetAddressOf())))) {
            return {};
        }

        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = ToDx11FillMode(desc.raster.fillMode);
        raster.CullMode = ToDx11CullMode(desc.raster.cullMode);
        raster.FrontCounterClockwise = desc.raster.frontCounterClockwise;
        raster.DepthClipEnable = desc.raster.depthClipEnabled;
        if (FAILED(m_device->CreateRasterizerState(&raster, pipeline.rasterizerState.GetAddressOf()))) {
            return {};
        }

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = desc.depthStencil.depthTestEnabled;
        depth.DepthWriteMask = desc.depthStencil.depthWriteEnabled ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = ToDx11CompareOp(desc.depthStencil.depthCompare);
        depth.StencilEnable = desc.depthStencil.stencilEnabled;
        if (FAILED(m_device->CreateDepthStencilState(&depth, pipeline.depthStencilState.GetAddressOf()))) {
            return {};
        }

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (desc.blend.alphaBlendEnabled) {
            blend.RenderTarget[0].BlendEnable = TRUE;
            blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        }
        if (FAILED(m_device->CreateBlendState(&blend, pipeline.blendState.GetAddressOf()))) {
            return {};
        }

        const uint64_t id = m_nextRhiPipelineHandle++;
        m_rhiPipelines.emplace(id, std::move(pipeline));
        return RhiPipelineHandle{ id };
    }

    RhiPipelineHandle Dx11GraphicsDevice::CreateRhiComputePipeline(const RhiComputePipelineDesc& desc)
    {
        if (!m_device || !desc.shader.bytecode || desc.shader.bytecodeSize == 0) {
            return {};
        }

        RhiPipelineStateObjects pipeline{};
        if (FAILED(m_device->CreateComputeShader(desc.shader.bytecode,
                                                 static_cast<SIZE_T>(desc.shader.bytecodeSize),
                                                 nullptr,
                                                 pipeline.computeShader.GetAddressOf()))) {
            return {};
        }
        const uint64_t id = m_nextRhiPipelineHandle++;
        m_rhiPipelines.emplace(id, std::move(pipeline));
        return RhiPipelineHandle{ id };
    }

    RhiDescriptorAllocation Dx11GraphicsDevice::AllocateRhiDescriptors(RhiDescriptorHeapType type,
                                                                      uint32_t count,
                                                                      bool shaderVisible)
    {
        (void)shaderVisible;
        if (count == 0) {
            return {};
        }
        const uint64_t base = m_nextRhiDescriptorHandle;
        m_nextRhiDescriptorHandle += count;

        RhiDescriptorAllocation allocation{};
        allocation.type = type;
        allocation.cpu.ptr = base;
        allocation.gpu.ptr = base;
        allocation.count = count;
        allocation.increment = 1;
        return allocation;
    }

    bool Dx11GraphicsDevice::CreateRhiShaderResourceView(RhiResourceHandle resourceHandle,
                                                         const RhiTextureViewDesc& desc,
                                                         RhiCpuDescriptorHandle destination)
    {
        const auto resourceIt = m_rhiResources.find(resourceHandle.id);
        if (resourceIt == m_rhiResources.end() || !destination.IsValid()) {
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = ToDx11Format(desc.format);
        srvDesc.ViewDimension = ToDx11SrvDimension(desc.dimension);
        if (srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
            srvDesc.Texture2DArray.MostDetailedMip = desc.baseMipLevel;
            srvDesc.Texture2DArray.MipLevels = desc.mipLevelCount;
            srvDesc.Texture2DArray.FirstArraySlice = desc.baseArrayLayer;
            srvDesc.Texture2DArray.ArraySize = desc.arrayLayerCount;
        } else if (srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBE) {
            srvDesc.TextureCube.MostDetailedMip = desc.baseMipLevel;
            srvDesc.TextureCube.MipLevels = desc.mipLevelCount;
        } else {
            srvDesc.Texture2D.MostDetailedMip = desc.baseMipLevel;
            srvDesc.Texture2D.MipLevels = desc.mipLevelCount;
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(m_device->CreateShaderResourceView(resourceIt->second.Get(), &srvDesc, view.GetAddressOf()))) {
            return false;
        }
        m_rhiSrvs[destination.ptr] = view;
        return true;
    }

    bool Dx11GraphicsDevice::CreateRhiRenderTargetView(RhiTextureHandle texture,
                                                       const RhiRenderTargetViewDesc& desc,
                                                       RhiCpuDescriptorHandle destination)
    {
        const auto resourceIt = m_rhiResources.find(texture.id);
        if (resourceIt == m_rhiResources.end() || !destination.IsValid()) {
            return false;
        }

        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = ToDx11Format(desc.format);
        rtvDesc.ViewDimension = desc.dimension == RhiTextureViewDimension::Texture2DArray
            ? D3D11_RTV_DIMENSION_TEXTURE2DARRAY
            : D3D11_RTV_DIMENSION_TEXTURE2D;
        if (rtvDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) {
            rtvDesc.Texture2DArray.MipSlice = desc.mipLevel;
            rtvDesc.Texture2DArray.FirstArraySlice = desc.baseArrayLayer;
            rtvDesc.Texture2DArray.ArraySize = desc.arrayLayerCount;
        } else {
            rtvDesc.Texture2D.MipSlice = desc.mipLevel;
        }

        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;
        if (FAILED(m_device->CreateRenderTargetView(resourceIt->second.Get(), &rtvDesc, view.GetAddressOf()))) {
            return false;
        }
        m_rhiRtvs[destination.ptr] = view;
        return true;
    }

    bool Dx11GraphicsDevice::CreateRhiDepthStencilView(RhiTextureHandle texture,
                                                       const RhiDepthStencilViewDesc& desc,
                                                       RhiCpuDescriptorHandle destination)
    {
        const auto resourceIt = m_rhiResources.find(texture.id);
        if (resourceIt == m_rhiResources.end() || !destination.IsValid()) {
            return false;
        }

        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = ToDx11Format(desc.format);
        dsvDesc.ViewDimension = desc.dimension == RhiTextureViewDimension::Texture2DArray
            ? D3D11_DSV_DIMENSION_TEXTURE2DARRAY
            : D3D11_DSV_DIMENSION_TEXTURE2D;
        if (dsvDesc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY) {
            dsvDesc.Texture2DArray.MipSlice = desc.mipLevel;
            dsvDesc.Texture2DArray.FirstArraySlice = desc.baseArrayLayer;
            dsvDesc.Texture2DArray.ArraySize = desc.arrayLayerCount;
        } else {
            dsvDesc.Texture2D.MipSlice = desc.mipLevel;
        }
        if (desc.readOnlyDepth) {
            dsvDesc.Flags |= D3D11_DSV_READ_ONLY_DEPTH;
        }
        if (desc.readOnlyStencil) {
            dsvDesc.Flags |= D3D11_DSV_READ_ONLY_STENCIL;
        }

        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> view;
        if (FAILED(m_device->CreateDepthStencilView(resourceIt->second.Get(), &dsvDesc, view.GetAddressOf()))) {
            return false;
        }
        m_rhiDsvs[destination.ptr] = view;
        return true;
    }

    std::unique_ptr<IRhiCommandEncoder> Dx11GraphicsDevice::CreateCommandEncoder(RhiQueueType queueType)
    {
        if (!m_context) {
            return std::make_unique<NullRhiCommandEncoder>();
        }
        return std::make_unique<Dx11RhiCommandEncoder>(*this, queueType);
    }

    bool Dx11GraphicsDevice::SubmitCommandEncoder(IRhiCommandEncoder& encoder, RhiQueueType queueType)
    {
        auto* dxEncoder = dynamic_cast<Dx11RhiCommandEncoder*>(&encoder);
        if (!dxEncoder || dxEncoder->QueueType() != queueType || !m_context) {
            return false;
        }
        m_context->Flush();
        return true;
    }
} // namespace SasamiRenderer

#endif
