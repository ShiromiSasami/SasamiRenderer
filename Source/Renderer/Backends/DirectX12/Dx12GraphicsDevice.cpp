#include "Renderer/Backends/DirectX12/Dx12GraphicsDevice.h"
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

        class Dx12RhiCommandEncoder final : public IRhiCommandEncoder
        {
        public:
            Dx12RhiCommandEncoder(Dx12GraphicsDevice& device, RhiQueueType queueType)
                : m_device(device)
                , m_queueType(queueType)
            {
                const D3D12_COMMAND_LIST_TYPE type = ToDx12CommandListType(queueType);
                if (FAILED(m_device.CreateCommandAllocator(type, m_allocator))) {
                    return;
                }
                if (FAILED(m_device.CreateCommandList(0, type, m_allocator, nullptr, m_commandList))) {
                    return;
                }
                m_valid = true;
            }

            bool IsValid() const { return m_valid; }
            RhiQueueType QueueType() const { return m_queueType; }
            CommandList& GetCommandList() { return m_commandList; }

            void TransitionResources(const RhiResourceTransitionDesc* transitions, uint32_t count) override
            {
                if (!m_valid || !transitions || count == 0) {
                    return;
                }

                std::vector<ResourceBarrier> barriers;
                barriers.reserve(count);
                for (uint32_t i = 0; i < count; ++i) {
                    Resource* resource = m_device.GetD3D12CompatibilityResource(transitions[i].resource);
                    if (!resource) {
                        continue;
                    }
                    barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(resource->Get(),
                                                                             ToDx12State(transitions[i].before),
                                                                             ToDx12State(transitions[i].after),
                                                                             transitions[i].subresource));
                }
                if (!barriers.empty()) {
                    m_commandList.ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
                }
            }

            void SetViewports(const RhiViewport* viewports, uint32_t count) override
            {
                if (!m_valid || !viewports || count == 0) {
                    return;
                }
                std::vector<Viewport> dxViewports(count);
                for (uint32_t i = 0; i < count; ++i) {
                    dxViewports[i] = {
                        viewports[i].x,
                        viewports[i].y,
                        viewports[i].width,
                        viewports[i].height,
                        viewports[i].minDepth,
                        viewports[i].maxDepth,
                    };
                }
                m_commandList.RSSetViewports(count, dxViewports.data());
            }

            void SetScissors(const RhiRect* scissors, uint32_t count) override
            {
                if (!m_valid || !scissors || count == 0) {
                    return;
                }
                std::vector<Rect> dxRects(count);
                for (uint32_t i = 0; i < count; ++i) {
                    dxRects[i] = { scissors[i].left, scissors[i].top, scissors[i].right, scissors[i].bottom };
                }
                m_commandList.RSSetScissorRects(count, dxRects.data());
            }

            void SetGraphicsPipeline(RhiPipelineHandle pipelineHandle) override
            {
                if (!m_valid || !pipelineHandle.IsValid()) return;
                // Registry lookup (handles created via RHI device)
                PipelineState* pipeline = m_device.GetD3D12CompatibilityPipelineState(pipelineHandle);
                if (pipeline) {
                    m_commandList.SetPipelineState(*pipeline);
                    return;
                }
                // Raw pointer fallback (handles from MakePipelineHandle)
                auto* pso = reinterpret_cast<ID3D12PipelineState*>(static_cast<uintptr_t>(pipelineHandle.id));
                m_commandList.Get()->SetPipelineState(pso);
            }

            void SetComputePipeline(RhiPipelineHandle pipelineHandle) override
            {
                SetGraphicsPipeline(pipelineHandle);
            }

            void SetPrimitiveTopology(RhiPrimitiveTopology topology) override
            {
                if (!m_valid) {
                    return;
                }
                m_commandList.IASetPrimitiveTopology(ToDx12PrimitiveTopology(topology));
            }

            void Draw(const RhiDrawDesc& desc) override
            {
                if (!m_valid) {
                    return;
                }
                m_commandList.DrawInstanced(desc.vertexCount,
                                            desc.instanceCount,
                                            desc.startVertex,
                                            desc.startInstance);
            }

            void DrawIndexed(const RhiDrawIndexedDesc& desc) override
            {
                if (!m_valid) {
                    return;
                }
                m_commandList.DrawIndexedInstanced(desc.indexCount,
                                                   desc.instanceCount,
                                                   desc.startIndex,
                                                   desc.baseVertex,
                                                   desc.startInstance);
            }

            void Dispatch(const RhiDispatchDesc& desc) override
            {
                if (!m_valid) {
                    return;
                }
                m_commandList.Dispatch(desc.groupCountX, desc.groupCountY, desc.groupCountZ);
            }

            void SetGraphicsPipelineLayout(RhiPipelineLayoutHandle handle) override
            {
                if (!m_valid || !handle.IsValid()) return;
                auto* sig = reinterpret_cast<ID3D12RootSignature*>(static_cast<uintptr_t>(handle.id));
                m_commandList.Get()->SetGraphicsRootSignature(sig);
            }

            void SetComputePipelineLayout(RhiPipelineLayoutHandle handle) override
            {
                if (!m_valid || !handle.IsValid()) return;
                auto* sig = reinterpret_cast<ID3D12RootSignature*>(static_cast<uintptr_t>(handle.id));
                m_commandList.Get()->SetComputeRootSignature(sig);
            }

            void SetDescriptorHeap(RhiDescriptorHeapHandle handle) override
            {
                if (!m_valid || !handle.IsValid()) return;
                auto* heap = reinterpret_cast<ID3D12DescriptorHeap*>(static_cast<uintptr_t>(handle.id));
                m_commandList.SetDescriptorHeaps(1, &heap);
            }

            void SetGraphicsDescriptorTable(uint32_t slot, RhiGpuDescriptorHandle table) override
            {
                if (!m_valid) return;
                m_commandList.SetGraphicsRootDescriptorTable(slot, { table.ptr });
            }

            void SetComputeDescriptorTable(uint32_t slot, RhiGpuDescriptorHandle table) override
            {
                if (!m_valid) return;
                m_commandList.SetComputeRootDescriptorTable(slot, { table.ptr });
            }

            void SetGraphicsConstantBufferView(uint32_t slot, RhiGpuAddress address) override
            {
                if (!m_valid) return;
                m_commandList.SetGraphicsRootConstantBufferView(slot, address);
            }

            void SetComputeConstantBufferView(uint32_t slot, RhiGpuAddress address) override
            {
                if (!m_valid) return;
                m_commandList.SetComputeRootConstantBufferView(slot, address);
            }

            void SetGraphicsShaderResourceView(uint32_t slot, RhiGpuAddress address) override
            {
                if (!m_valid) return;
                m_commandList.SetGraphicsRootShaderResourceView(slot, address);
            }

            void SetComputeShaderResourceView(uint32_t slot, RhiGpuAddress address) override
            {
                if (!m_valid) return;
                m_commandList.SetComputeRootShaderResourceView(slot, address);
            }

            void SetRenderTargets(uint32_t numRtvs, const RhiCpuDescriptorHandle* rtvs, const RhiCpuDescriptorHandle* dsv) override
            {
                if (!m_valid) return;
                std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> dxRtvs(numRtvs);
                for (uint32_t i = 0; i < numRtvs; ++i) dxRtvs[i] = { static_cast<SIZE_T>(rtvs[i].ptr) };
                const D3D12_CPU_DESCRIPTOR_HANDLE* dsvPtr = nullptr;
                D3D12_CPU_DESCRIPTOR_HANDLE dxDsv{};
                if (dsv && dsv->IsValid()) { dxDsv = { static_cast<SIZE_T>(dsv->ptr) }; dsvPtr = &dxDsv; }
                m_commandList.OMSetRenderTargets(numRtvs, numRtvs > 0 ? dxRtvs.data() : nullptr, FALSE, dsvPtr);
            }

            void ClearRenderTarget(RhiCpuDescriptorHandle rtv, const RhiClearColor& color) override
            {
                if (!m_valid) return;
                const FLOAT rgba[4] = { color.r, color.g, color.b, color.a };
                m_commandList.ClearRenderTargetView({ static_cast<SIZE_T>(rtv.ptr) }, rgba, 0, nullptr);
            }

            void ClearDepthStencil(RhiCpuDescriptorHandle dsv, float depth, uint8_t stencil) override
            {
                if (!m_valid) return;
                m_commandList.ClearDepthStencilView({ static_cast<SIZE_T>(dsv.ptr) },
                    D3D12_CLEAR_FLAG_DEPTH, depth, stencil, 0, nullptr);
            }

            void SetVertexBuffers(uint32_t startSlot, uint32_t count, const RhiVertexBufferView* views) override
            {
                if (!m_valid || count == 0 || !views) return;
                std::vector<D3D12_VERTEX_BUFFER_VIEW> dxViews(count);
                for (uint32_t i = 0; i < count; ++i) {
                    dxViews[i].BufferLocation = views[i].gpuAddress;
                    dxViews[i].StrideInBytes  = views[i].strideInBytes;
                    dxViews[i].SizeInBytes    = views[i].sizeInBytes;
                }
                m_commandList.IASetVertexBuffers(startSlot, count, dxViews.data());
            }

            void SetIndexBuffer(const RhiIndexBufferView& view) override
            {
                if (!m_valid) return;
                D3D12_INDEX_BUFFER_VIEW ibv{};
                ibv.BufferLocation = view.gpuAddress;
                ibv.SizeInBytes    = view.sizeInBytes;
                ibv.Format         = view.is32Bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
                m_commandList.IASetIndexBuffer(&ibv);
            }

        private:
            Dx12GraphicsDevice& m_device;
            RhiQueueType m_queueType = RhiQueueType::Graphics;
            CommandAllocator m_allocator;
            CommandList m_commandList;
            bool m_valid = false;
        };
    }

    // Initialize + Getters → Dx12GraphicsDevice_Init.cpp
    // Resource creation → Dx12GraphicsDevice_Resource.cpp


    RhiPipelineHandle Dx12GraphicsDevice::CreateRhiGraphicsPipeline(const RhiGraphicsPipelineDesc& desc)
    {
        if (!m_device || !desc.layout.IsValid()) {
            return {};
        }
        const auto layoutIt = m_rhiPipelineLayouts.find(desc.layout.id);
        if (layoutIt == m_rhiPipelineLayouts.end()) {
            return {};
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = layoutIt->second.Get();

        auto applyShader = [&](RhiShaderStageFlags stage, const void* data, uint64_t size) {
            D3D12_SHADER_BYTECODE bytecode{ data, static_cast<SIZE_T>(size) };
            if (stage == RhiShaderStageFlags::Vertex) {
                pso.VS = bytecode;
            } else if (stage == RhiShaderStageFlags::Pixel) {
                pso.PS = bytecode;
            } else if (stage == RhiShaderStageFlags::Geometry) {
                pso.GS = bytecode;
            } else if (stage == RhiShaderStageFlags::Hull) {
                pso.HS = bytecode;
            } else if (stage == RhiShaderStageFlags::Domain) {
                pso.DS = bytecode;
            }
        };

        if (desc.shaderHandles && desc.shaderHandleCount > 0) {
            for (uint32_t i = 0; i < desc.shaderHandleCount; ++i) {
                const RhiShaderHandle handle = desc.shaderHandles[i];
                const auto shaderIt = m_rhiShaders.find(handle.id);
                if (shaderIt == m_rhiShaders.end()) {
                    continue;
                }
                const RhiShaderModule& shader = shaderIt->second;
                applyShader(shader.stage, shader.bytecode.data(), shader.bytecode.size());
            }
        }
        if (desc.shaders) {
            for (uint32_t i = 0; i < desc.shaderCount; ++i) {
                const RhiShaderModuleDesc& shader = desc.shaders[i];
                applyShader(shader.stage, shader.bytecode, shader.bytecodeSize);
            }
        }

        std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements(desc.vertexAttributeCount);
        for (uint32_t i = 0; i < desc.vertexAttributeCount; ++i) {
            const RhiVertexAttributeDesc& src = desc.vertexAttributes[i];
            D3D12_INPUT_ELEMENT_DESC& dst = inputElements[i];
            dst.SemanticName = src.semantic;
            dst.SemanticIndex = src.semanticIndex;
            dst.Format = ToDxgiFormat(src.format);
            dst.InputSlot = src.binding;
            dst.AlignedByteOffset = src.offset;
            for (uint32_t bindingIndex = 0; bindingIndex < desc.vertexBindingCount; ++bindingIndex) {
                if (desc.vertexBindings[bindingIndex].binding == src.binding &&
                    desc.vertexBindings[bindingIndex].inputRate == RhiInputRate::PerInstance) {
                    dst.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
                    dst.InstanceDataStepRate = 1;
                    break;
                }
            }
        }

        pso.InputLayout = {
            inputElements.empty() ? nullptr : inputElements.data(),
            static_cast<UINT>(inputElements.size())
        };
        pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso.RasterizerState.FillMode = ToDx12FillMode(desc.raster.fillMode);
        pso.RasterizerState.CullMode = ToDx12CullMode(desc.raster.cullMode);
        pso.RasterizerState.FrontCounterClockwise = desc.raster.frontCounterClockwise;
        pso.RasterizerState.DepthClipEnable = desc.raster.depthClipEnabled;
        pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pso.DepthStencilState.DepthEnable = desc.depthStencil.depthTestEnabled;
        pso.DepthStencilState.DepthWriteMask = desc.depthStencil.depthWriteEnabled
            ? D3D12_DEPTH_WRITE_MASK_ALL
            : D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.DepthStencilState.DepthFunc = ToDx12CompareOp(desc.depthStencil.depthCompare);
        pso.DepthStencilState.StencilEnable = desc.depthStencil.stencilEnabled;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = ToDx12PrimitiveTopologyType(desc.topology);
        pso.NumRenderTargets = desc.colorFormatCount;
        for (uint32_t i = 0; i < desc.colorFormatCount && i < 8; ++i) {
            pso.RTVFormats[i] = ToDxgiFormat(desc.colorFormats[i]);
        }
        pso.DSVFormat = ToDxgiFormat(desc.depthStencilFormat);
        pso.SampleDesc.Count = 1;

        PipelineState pipeline;
        if (FAILED(CreateGraphicsPipelineState(pso, pipeline))) {
            return {};
        }
        const uint64_t id = m_nextRhiPipelineHandle++;
        m_rhiPipelines.emplace(id, std::move(pipeline));
        return RhiPipelineHandle{ id };
    }

    RhiPipelineHandle Dx12GraphicsDevice::CreateRhiComputePipeline(const RhiComputePipelineDesc& desc)
    {
        if (!desc.shader.bytecode || desc.shader.bytecodeSize == 0) {
            return {};
        }

        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> error;
        if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error))) {
            return {};
        }

        RootSignature rootSignature;
        if (FAILED(CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), rootSignature))) {
            return {};
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = rootSignature.Get();
        pso.CS = { desc.shader.bytecode, static_cast<SIZE_T>(desc.shader.bytecodeSize) };

        PipelineState pipeline;
        if (FAILED(CreateComputePipelineState(pso, pipeline))) {
            return {};
        }
        const uint64_t layoutId = m_nextRhiPipelineLayoutHandle++;
        m_rhiPipelineLayouts.emplace(layoutId, std::move(rootSignature));
        const uint64_t id = m_nextRhiPipelineHandle++;
        m_rhiPipelines.emplace(id, std::move(pipeline));
        return RhiPipelineHandle{ id };
    }

    RhiDescriptorAllocation Dx12GraphicsDevice::AllocateRhiDescriptors(RhiDescriptorHeapType type,
                                                                      uint32_t count,
                                                                      bool shaderVisible)
    {
        if (!m_device || count == 0) {
            return {};
        }

        DescriptorHeap heap;
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = ToDx12DescriptorHeapType(type);
        desc.NumDescriptors = count;
        desc.Flags = shaderVisible &&
                (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
                 desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
            ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
            : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(CreateDescriptorHeap(desc, heap))) {
            return {};
        }

        const uint32_t increment = GetDescriptorHandleIncrementSize(desc.Type);
        RhiDescriptorAllocation allocation{};
        allocation.type = type;
        allocation.count = count;
        allocation.increment = increment;
        allocation.cpu.ptr = heap.GetCPUDescriptorHandleForHeapStart().ptr;
        if (desc.Flags == D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
            allocation.gpu.ptr = heap.GetGPUDescriptorHandleForHeapStart().ptr;
        }
        m_rhiDescriptorHeaps.push_back(std::move(heap));
        return allocation;
    }

    bool Dx12GraphicsDevice::CreateRhiShaderResourceView(RhiResourceHandle resourceHandle,
                                                         const RhiTextureViewDesc& desc,
                                                         RhiCpuDescriptorHandle destination)
    {
        Resource* resource = FindRhiResource(resourceHandle);
        if (!resource || !destination.IsValid()) {
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = ToDxgiFormat(desc.format);
        srv.ViewDimension = ToDx12SrvDimension(desc.dimension);
        switch (srv.ViewDimension) {
        case D3D12_SRV_DIMENSION_TEXTURE1D:
            srv.Texture1D.MostDetailedMip = desc.baseMipLevel;
            srv.Texture1D.MipLevels = desc.mipLevelCount;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
            srv.Texture1DArray.MostDetailedMip = desc.baseMipLevel;
            srv.Texture1DArray.MipLevels = desc.mipLevelCount;
            srv.Texture1DArray.FirstArraySlice = desc.baseArrayLayer;
            srv.Texture1DArray.ArraySize = desc.arrayLayerCount;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            srv.Texture2DArray.MostDetailedMip = desc.baseMipLevel;
            srv.Texture2DArray.MipLevels = desc.mipLevelCount;
            srv.Texture2DArray.FirstArraySlice = desc.baseArrayLayer;
            srv.Texture2DArray.ArraySize = desc.arrayLayerCount;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE3D:
            srv.Texture3D.MostDetailedMip = desc.baseMipLevel;
            srv.Texture3D.MipLevels = desc.mipLevelCount;
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE:
            srv.TextureCube.MostDetailedMip = desc.baseMipLevel;
            srv.TextureCube.MipLevels = desc.mipLevelCount;
            break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
            srv.TextureCubeArray.MostDetailedMip = desc.baseMipLevel;
            srv.TextureCubeArray.MipLevels = desc.mipLevelCount;
            srv.TextureCubeArray.First2DArrayFace = desc.baseArrayLayer;
            srv.TextureCubeArray.NumCubes = desc.arrayLayerCount / 6;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2D:
        default:
            srv.Texture2D.MostDetailedMip = desc.baseMipLevel;
            srv.Texture2D.MipLevels = desc.mipLevelCount;
            break;
        }
        CreateShaderResourceView(*resource, &srv, CpuDescriptorHandle{ destination.ptr });
        return true;
    }

    bool Dx12GraphicsDevice::CreateRhiRenderTargetView(RhiTextureHandle texture,
                                                       const RhiRenderTargetViewDesc& desc,
                                                       RhiCpuDescriptorHandle destination)
    {
        Resource* resource = FindRhiResource(texture);
        if (!resource || !destination.IsValid()) {
            return false;
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = ToDxgiFormat(desc.format);
        rtv.ViewDimension = desc.dimension == RhiTextureViewDimension::Texture2DArray
            ? D3D12_RTV_DIMENSION_TEXTURE2DARRAY
            : D3D12_RTV_DIMENSION_TEXTURE2D;
        if (rtv.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DARRAY) {
            rtv.Texture2DArray.MipSlice = desc.mipLevel;
            rtv.Texture2DArray.FirstArraySlice = desc.baseArrayLayer;
            rtv.Texture2DArray.ArraySize = desc.arrayLayerCount;
        } else {
            rtv.Texture2D.MipSlice = desc.mipLevel;
        }
        CreateRenderTargetView(*resource, &rtv, CpuDescriptorHandle{ destination.ptr });
        return true;
    }

    bool Dx12GraphicsDevice::CreateRhiDepthStencilView(RhiTextureHandle texture,
                                                       const RhiDepthStencilViewDesc& desc,
                                                       RhiCpuDescriptorHandle destination)
    {
        Resource* resource = FindRhiResource(texture);
        if (!resource || !destination.IsValid()) {
            return false;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = ToDxgiFormat(desc.format);
        dsv.ViewDimension = desc.dimension == RhiTextureViewDimension::Texture2DArray
            ? D3D12_DSV_DIMENSION_TEXTURE2DARRAY
            : D3D12_DSV_DIMENSION_TEXTURE2D;
        UINT dsvFlags = D3D12_DSV_FLAG_NONE;
        if (desc.readOnlyDepth) {
            dsvFlags |= D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        }
        if (desc.readOnlyStencil) {
            dsvFlags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
        }
        dsv.Flags = static_cast<D3D12_DSV_FLAGS>(dsvFlags);
        if (dsv.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DARRAY) {
            dsv.Texture2DArray.MipSlice = desc.mipLevel;
            dsv.Texture2DArray.FirstArraySlice = desc.baseArrayLayer;
            dsv.Texture2DArray.ArraySize = desc.arrayLayerCount;
        } else {
            dsv.Texture2D.MipSlice = desc.mipLevel;
        }
        CreateDepthStencilView(*resource, &dsv, CpuDescriptorHandle{ destination.ptr });
        return true;
    }

    std::unique_ptr<IRhiCommandEncoder> Dx12GraphicsDevice::CreateCommandEncoder(RhiQueueType queueType)
    {
        auto encoder = std::make_unique<Dx12RhiCommandEncoder>(*this, queueType);
        if (!encoder->IsValid()) {
            return std::make_unique<NullRhiCommandEncoder>();
        }
        return encoder;
    }

    bool Dx12GraphicsDevice::SubmitCommandEncoder(IRhiCommandEncoder& encoder, RhiQueueType queueType)
    {
        auto* dxEncoder = dynamic_cast<Dx12RhiCommandEncoder*>(&encoder);
        if (!dxEncoder || dxEncoder->QueueType() != queueType) {
            return false;
        }
        CommandList& commandList = dxEncoder->GetCommandList();
        if (FAILED(commandList.Close())) {
            return false;
        }

        ID3D12CommandList* lists[] = { commandList.Get() };
        CommandQueue* queue = nullptr;
        if (queueType == RhiQueueType::Compute && m_computeQueue.IsValid()) {
            queue = &m_computeQueue;
        } else {
            queue = &m_commandQueue;
        }
        if (!queue || !queue->IsValid()) {
            return false;
        }
        queue->ExecuteCommandLists(1, lists);
        return true;
    }

    Resource* Dx12GraphicsDevice::GetD3D12CompatibilityResource(RhiResourceHandle handle)
    {
        return FindRhiResource(handle);
    }

    PipelineState* Dx12GraphicsDevice::GetD3D12CompatibilityPipelineState(RhiPipelineHandle handle)
    {
        return FindRhiPipeline(handle);
    }

    HRESULT Dx12GraphicsDevice::CreateDescriptorHeap(const DescriptorHeapDesc& desc, DescriptorHeap& out)
    {
        return m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&out.m_heap));
    }

    HRESULT Dx12GraphicsDevice::CreateCommittedResource(const HeapProperties* heapProps,
                                                        HeapFlags heapFlags,
                                                        const ResourceDesc* desc,
                                                        ResourceState initialState,
                                                        const ClearValue* clearValue,
                                                        Resource& out)
    {
        return m_device->CreateCommittedResource(heapProps, heapFlags, desc, initialState, clearValue, IID_PPV_ARGS(&out.m_resource));
    }

    HRESULT Dx12GraphicsDevice::CreateCommandAllocator(CommandListType type, CommandAllocator& out)
    {
        return m_device->CreateCommandAllocator(type, IID_PPV_ARGS(&out.m_allocator));
    }

    HRESULT Dx12GraphicsDevice::CreateCommandList(UINT nodeMask,
                                                  CommandListType type,
                                                  CommandAllocator& allocator,
                                                  PipelineState* initialPSO,
                                                  CommandList& out)
    {
        return m_device->CreateCommandList(nodeMask,
                                           type,
                                           allocator.Get(),
                                           initialPSO ? initialPSO->Get() : nullptr,
                                           IID_PPV_ARGS(&out.m_list));
    }

    HRESULT Dx12GraphicsDevice::CreateGraphicsPipelineState(const GraphicsPipelineDesc& desc, PipelineState& out)
    {
        return m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&out.m_state));
    }

    HRESULT Dx12GraphicsDevice::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, PipelineState& out)
    {
        return m_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&out.m_state));
    }

    HRESULT Dx12GraphicsDevice::CreatePipelineStateFromStream(const void* streamData, size_t streamSize, PipelineState& out)
    {
        ComPtr<ID3D12Device2> device2;
        HRESULT hr = m_device.As(&device2);
        if (FAILED(hr)) {
            return hr;
        }
        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{};
        streamDesc.SizeInBytes                   = streamSize;
        streamDesc.pPipelineStateSubobjectStream = const_cast<void*>(streamData);
        return device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&out.m_state));
    }

    HRESULT Dx12GraphicsDevice::CreateRootSignature(UINT nodeMask, const void* blobData, size_t blobSize, RootSignature& out)
    {
        return m_device->CreateRootSignature(nodeMask, blobData, blobSize, IID_PPV_ARGS(&out.m_signature));
    }

    void Dx12GraphicsDevice::CreateShaderResourceView(Resource& resource, const ShaderResourceViewDesc* desc, CpuDescriptorHandle dest)
    {
        m_device->CreateShaderResourceView(resource.Get(), desc, dest);
    }

    void Dx12GraphicsDevice::CreateDepthStencilView(Resource& resource, const DepthStencilViewDesc* desc, CpuDescriptorHandle dest)
    {
        m_device->CreateDepthStencilView(resource.Get(), desc, dest);
    }

    void Dx12GraphicsDevice::CreateRenderTargetView(Resource& resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, CpuDescriptorHandle dest)
    {
        m_device->CreateRenderTargetView(resource.Get(), desc, dest);
    }

    void Dx12GraphicsDevice::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, CpuDescriptorHandle dest)
    {
        m_device->CreateConstantBufferView(desc, dest);
    }

    void Dx12GraphicsDevice::CreateSampler(const D3D12_SAMPLER_DESC* desc, CpuDescriptorHandle dest)
    {
        m_device->CreateSampler(desc, dest);
    }

    HRESULT Dx12GraphicsDevice::CreateFence(UINT64 initialValue, D3D12_FENCE_FLAGS flags, ID3D12Fence** fence)
    {
        return m_device->CreateFence(initialValue, flags, IID_PPV_ARGS(fence));
    }

    Resource* Dx12GraphicsDevice::FindRhiResource(RhiResourceHandle handle)
    {
        const auto it = m_rhiResources.find(handle.id);
        return it != m_rhiResources.end() ? &it->second : nullptr;
    }

    const Resource* Dx12GraphicsDevice::FindRhiResource(RhiResourceHandle handle) const
    {
        const auto it = m_rhiResources.find(handle.id);
        return it != m_rhiResources.end() ? &it->second : nullptr;
    }

    PipelineState* Dx12GraphicsDevice::FindRhiPipeline(RhiPipelineHandle handle)
    {
        const auto it = m_rhiPipelines.find(handle.id);
        return it != m_rhiPipelines.end() ? &it->second : nullptr;
    }

    RhiResourceHandle Dx12GraphicsDevice::StoreRhiResource(Resource&& resource)
    {
        const uint64_t id = m_nextRhiResourceHandle++;
        m_rhiResources.emplace(id, std::move(resource));
        return RhiResourceHandle{ id };
    }
#endif

    GraphicsRuntime GetBuildDefaultGraphicsRuntime()
    {
#if RHI_DIRECTX12
        return GraphicsRuntime::DirectX12;
#elif RHI_VULKAN
        return GraphicsRuntime::Vulkan;
#elif RHI_DIRECTX11
        return GraphicsRuntime::DirectX11;
#elif RHI_OPENGL
        return GraphicsRuntime::OpenGL;
#else
        return GraphicsRuntime::DirectX12;
#endif
    }

    bool IsGraphicsRuntimeEnabled(GraphicsRuntime runtime)
    {
        switch (runtime) {
        case GraphicsRuntime::DirectX12: return RHI_DIRECTX12 != 0;
        case GraphicsRuntime::Vulkan: return RHI_VULKAN != 0;
        case GraphicsRuntime::DirectX11: return RHI_DIRECTX11 != 0;
        case GraphicsRuntime::OpenGL: return RHI_OPENGL != 0;
        default: return false;
        }
    }

    std::unique_ptr<IRHIDevice> CreateRHIDevice(GraphicsRuntime runtime)
    {
        switch (runtime) {
        case GraphicsRuntime::DirectX12:
#if RHI_DIRECTX12
            return std::make_unique<Dx12GraphicsDevice>();
#else
            DebugLog("GraphicsRuntime::DirectX12 is disabled by build symbol.\n");
            return nullptr;
#endif
        case GraphicsRuntime::Vulkan:
#if RHI_VULKAN
            return std::make_unique<VulkanGraphicsDevice>();
#else
            DebugLog("GraphicsRuntime::Vulkan is disabled by build symbol.\n");
#endif
            return nullptr;
        case GraphicsRuntime::DirectX11:
#if RHI_DIRECTX11
            return std::make_unique<Dx11GraphicsDevice>();
#else
            DebugLog("GraphicsRuntime::DirectX11 is disabled by build symbol.\n");
#endif
            return nullptr;
        case GraphicsRuntime::OpenGL:
#if RHI_OPENGL
            return std::make_unique<OpenGLGraphicsDevice>();
#else
            DebugLog("GraphicsRuntime::OpenGL is disabled by build symbol.\n");
#endif
            return nullptr;
        default:
            return nullptr;
        }
    }

    const char* GraphicsRuntimeToString(GraphicsRuntime runtime)
    {
        switch (runtime) {
        case GraphicsRuntime::DirectX12: return "DirectX12";
        case GraphicsRuntime::Vulkan: return "Vulkan";
        case GraphicsRuntime::DirectX11: return "DirectX11";
        case GraphicsRuntime::OpenGL: return "OpenGL";
        default: return "Unknown";
        }
    }
}
