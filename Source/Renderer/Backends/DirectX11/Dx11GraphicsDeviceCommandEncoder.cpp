// Dx11GraphicsDeviceCommandEncoder.cpp
// The RHI command encoder for DirectX 11. Unlike DirectX 12 there are no command lists to
// record into, so this class translates the RHI's record-then-submit shape onto the immediate
// context, tracking the bindings the caller expects to persist across calls.
#include "Renderer/Backends/DirectX11/Dx11GraphicsDevice.h"
#include "Renderer/Backends/DirectX11/Dx11ConversionUtils.h"

#if RHI_DIRECTX11

#include "Foundation/Tools/DebugOutput.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace SasamiEngine::Render
{
    class Dx11RhiCommandEncoder final : public IRhiCommandEncoder
    {
    public:
        Dx11RhiCommandEncoder(Dx11GraphicsDevice& device, RhiQueueType queueType)
            : m_device(device)
            , m_queueType(queueType)
        {
        }

        RhiQueueType QueueType() const { return m_queueType; }

        void SetGraphicsPipelineLayout(RhiPipelineLayoutHandle handle) override
        {
            const auto it = m_device.m_rhiPipelineLayouts.find(handle.id);
            if (!m_device.m_context || it == m_device.m_rhiPipelineLayouts.end()) return;
            for (const auto& sampler : it->second.staticSamplers) {
                ID3D11SamplerState* state = sampler.state.Get();
                const uint32_t flags = static_cast<uint32_t>(sampler.visibility);
                if (flags & static_cast<uint32_t>(RhiShaderStageFlags::Vertex)) m_device.m_context->VSSetSamplers(sampler.shaderRegister, 1, &state);
                if (flags & static_cast<uint32_t>(RhiShaderStageFlags::Hull)) m_device.m_context->HSSetSamplers(sampler.shaderRegister, 1, &state);
                if (flags & static_cast<uint32_t>(RhiShaderStageFlags::Domain)) m_device.m_context->DSSetSamplers(sampler.shaderRegister, 1, &state);
                if (flags & static_cast<uint32_t>(RhiShaderStageFlags::Geometry)) m_device.m_context->GSSetSamplers(sampler.shaderRegister, 1, &state);
                if (flags & static_cast<uint32_t>(RhiShaderStageFlags::Pixel)) m_device.m_context->PSSetSamplers(sampler.shaderRegister, 1, &state);
            }
        }

        void SetComputePipelineLayout(RhiPipelineLayoutHandle handle) override
        {
            const auto it = m_device.m_rhiPipelineLayouts.find(handle.id);
            if (!m_device.m_context || it == m_device.m_rhiPipelineLayouts.end()) return;
            for (const auto& sampler : it->second.staticSamplers) {
                const uint32_t flags = static_cast<uint32_t>(sampler.visibility);
                if (flags & static_cast<uint32_t>(RhiShaderStageFlags::Compute)) {
                    ID3D11SamplerState* state = sampler.state.Get();
                    m_device.m_context->CSSetSamplers(sampler.shaderRegister, 1, &state);
                }
            }
        }

        void BeginRenderPass(const RhiRenderPassDesc& desc) override
        {
            if (!m_device.m_device || !m_device.m_context) {
                return;
            }

            m_renderTargets.clear();
            m_depthStencil.Reset();
            std::vector<ID3D11RenderTargetView*> rtvs;
            rtvs.reserve(desc.colorAttachmentCount);

            for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i) {
                const RhiAttachmentDesc& attachment = desc.colorAttachments[i];
                const auto resourceIt = m_device.m_rhiResources.find(attachment.texture.id);
                if (resourceIt == m_device.m_rhiResources.end()) {
                    rtvs.push_back(nullptr);
                    continue;
                }

                D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
                rtvDesc.Format = ToDx11Format(attachment.format);
                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
                if (SUCCEEDED(m_device.m_device->CreateRenderTargetView(resourceIt->second.Get(), &rtvDesc, rtv.GetAddressOf()))) {
                    if (attachment.loadOp == RhiLoadOp::Clear) {
                        const float clear[] = {
                            attachment.clearColor.r,
                            attachment.clearColor.g,
                            attachment.clearColor.b,
                            attachment.clearColor.a,
                        };
                        m_device.m_context->ClearRenderTargetView(rtv.Get(), clear);
                    }
                    rtvs.push_back(rtv.Get());
                    m_renderTargets.push_back(rtv);
                } else {
                    rtvs.push_back(nullptr);
                }
            }

            ID3D11DepthStencilView* dsv = nullptr;
            if (desc.depthStencilAttachment) {
                const RhiAttachmentDesc& attachment = *desc.depthStencilAttachment;
                const auto resourceIt = m_device.m_rhiResources.find(attachment.texture.id);
                if (resourceIt != m_device.m_rhiResources.end()) {
                    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
                    dsvDesc.Format = ToDx11Format(attachment.format);
                    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                    if (SUCCEEDED(m_device.m_device->CreateDepthStencilView(resourceIt->second.Get(), &dsvDesc, m_depthStencil.GetAddressOf()))) {
                        dsv = m_depthStencil.Get();
                        if (attachment.loadOp == RhiLoadOp::Clear) {
                            m_device.m_context->ClearDepthStencilView(dsv,
                                                                      D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                                                      attachment.clearDepthStencil.depth,
                                                                      static_cast<UINT8>(attachment.clearDepthStencil.stencil));
                        }
                    }
                }
            }

            m_device.m_context->OMSetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.empty() ? nullptr : rtvs.data(), dsv);
        }

        void EndRenderPass() override
        {
            if (m_device.m_context) {
                m_device.m_context->OMSetRenderTargets(0, nullptr, nullptr);
            }
            m_renderTargets.clear();
            m_depthStencil.Reset();
        }

        void SetGraphicsPipeline(RhiPipelineHandle pipelineHandle) override
        {
            const auto it = m_device.m_rhiPipelines.find(pipelineHandle.id);
            if (it == m_device.m_rhiPipelines.end() || !m_device.m_context) {
                return;
            }

            const Dx11GraphicsDevice::RhiPipelineStateObjects& pipeline = it->second;
            m_device.m_context->IASetPrimitiveTopology(pipeline.topology);
            m_device.m_context->IASetInputLayout(pipeline.inputLayout.Get());
            m_device.m_context->VSSetShader(pipeline.vertexShader.Get(), nullptr, 0);
            m_device.m_context->PSSetShader(pipeline.pixelShader.Get(), nullptr, 0);
            m_device.m_context->GSSetShader(pipeline.geometryShader.Get(), nullptr, 0);
            m_device.m_context->HSSetShader(pipeline.hullShader.Get(), nullptr, 0);
            m_device.m_context->DSSetShader(pipeline.domainShader.Get(), nullptr, 0);
            m_device.m_context->RSSetState(pipeline.rasterizerState.Get());
            m_device.m_context->OMSetDepthStencilState(pipeline.depthStencilState.Get(), 0);
            const float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            m_device.m_context->OMSetBlendState(pipeline.blendState.Get(), blendFactor, 0xffffffffu);
        }

        void SetComputePipeline(RhiPipelineHandle pipelineHandle) override
        {
            const auto it = m_device.m_rhiPipelines.find(pipelineHandle.id);
            if (it != m_device.m_rhiPipelines.end() && m_device.m_context) {
                m_device.m_context->CSSetShader(it->second.computeShader.Get(), nullptr, 0);
            }
        }

        void SetPrimitiveTopology(RhiPrimitiveTopology topology) override
        {
            if (m_device.m_context) {
                m_device.m_context->IASetPrimitiveTopology(ToDx11Topology(topology));
            }
        }

        void SetViewports(const RhiViewport* viewports, uint32_t count) override
        {
            if (!m_device.m_context || !viewports || count == 0) {
                return;
            }
            std::vector<D3D11_VIEWPORT> dxViewports(count);
            for (uint32_t i = 0; i < count; ++i) {
                dxViewports[i] = ToDx11Viewport(viewports[i]);
            }
            m_device.m_context->RSSetViewports(count, dxViewports.data());
        }

        void SetScissors(const RhiRect* scissors, uint32_t count) override
        {
            if (!m_device.m_context || !scissors || count == 0) {
                return;
            }
            std::vector<D3D11_RECT> dxRects(count);
            for (uint32_t i = 0; i < count; ++i) {
                dxRects[i] = ToDx11Rect(scissors[i]);
            }
            m_device.m_context->RSSetScissorRects(count, dxRects.data());
        }

        void Draw(const RhiDrawDesc& draw) override
        {
            if (m_device.m_context) {
                m_device.m_context->DrawInstanced(draw.vertexCount,
                                                  draw.instanceCount,
                                                  draw.startVertex,
                                                  draw.startInstance);
            }
        }

        void DrawIndexed(const RhiDrawIndexedDesc& draw) override
        {
            if (m_device.m_context) {
                m_device.m_context->DrawIndexedInstanced(draw.indexCount,
                                                         draw.instanceCount,
                                                         draw.startIndex,
                                                         draw.baseVertex,
                                                         draw.startInstance);
            }
        }

        void Dispatch(const RhiDispatchDesc& dispatch) override
        {
            if (m_device.m_context) {
                m_device.m_context->Dispatch(dispatch.groupCountX, dispatch.groupCountY, dispatch.groupCountZ);
            }
        }

        void SetGraphicsDescriptorTable(uint32_t slot, RhiGpuDescriptorHandle table) override
        {
            if (!m_device.m_context || !table.IsValid()) {
                return;
            }
            const auto srvIt = m_device.m_rhiSrvs.find(table.ptr);
            if (srvIt != m_device.m_rhiSrvs.end()) {
                ID3D11ShaderResourceView* srv = srvIt->second.Get();
                m_device.m_context->PSSetShaderResources(slot, 1, &srv);
                m_device.m_context->VSSetShaderResources(slot, 1, &srv);
            }
        }

        void SetComputeDescriptorTable(uint32_t slot, RhiGpuDescriptorHandle table) override
        {
            if (!m_device.m_context || !table.IsValid()) {
                return;
            }
            const auto srvIt = m_device.m_rhiSrvs.find(table.ptr);
            if (srvIt != m_device.m_rhiSrvs.end()) {
                ID3D11ShaderResourceView* srv = srvIt->second.Get();
                m_device.m_context->CSSetShaderResources(slot, 1, &srv);
                return;
            }
            const auto uavIt = m_device.m_rhiUavs.find(table.ptr);
            if (uavIt != m_device.m_rhiUavs.end()) {
                ID3D11UnorderedAccessView* uav = uavIt->second.Get();
                const UINT initialCount = UINT_MAX;
                m_device.m_context->CSSetUnorderedAccessViews(slot, 1, &uav, &initialCount);
            }
        }

        void SetRenderTargets(uint32_t numRtvs,
                              const RhiCpuDescriptorHandle* rtvs,
                              const RhiCpuDescriptorHandle* dsv = nullptr) override
        {
            if (!m_device.m_context) {
                return;
            }

            m_boundRenderTargets.clear();
            m_boundDepthStencil.Reset();
            std::vector<ID3D11RenderTargetView*> dxRtvs;
            dxRtvs.reserve(numRtvs);
            for (uint32_t i = 0; i < numRtvs; ++i) {
                ID3D11RenderTargetView* view = nullptr;
                if (rtvs) {
                    const auto it = m_device.m_rhiRtvs.find(rtvs[i].ptr);
                    if (it != m_device.m_rhiRtvs.end()) {
                        view = it->second.Get();
                        m_boundRenderTargets.push_back(it->second);
                    }
                }
                dxRtvs.push_back(view);
            }

            ID3D11DepthStencilView* dxDsv = nullptr;
            if (dsv && dsv->IsValid()) {
                const auto it = m_device.m_rhiDsvs.find(dsv->ptr);
                if (it != m_device.m_rhiDsvs.end()) {
                    dxDsv = it->second.Get();
                    m_boundDepthStencil = it->second;
                }
            }

            m_device.m_context->OMSetRenderTargets(static_cast<UINT>(dxRtvs.size()),
                                                   dxRtvs.empty() ? nullptr : dxRtvs.data(),
                                                   dxDsv);
        }

        void ClearRenderTarget(RhiCpuDescriptorHandle rtv, const RhiClearColor& color) override
        {
            if (!m_device.m_context || !rtv.IsValid()) {
                return;
            }
            const auto it = m_device.m_rhiRtvs.find(rtv.ptr);
            if (it == m_device.m_rhiRtvs.end()) {
                return;
            }
            const float clear[] = { color.r, color.g, color.b, color.a };
            m_device.m_context->ClearRenderTargetView(it->second.Get(), clear);
        }

        void ClearDepthStencil(RhiCpuDescriptorHandle dsv, float depth, uint8_t stencil) override
        {
            if (!m_device.m_context || !dsv.IsValid()) {
                return;
            }
            const auto it = m_device.m_rhiDsvs.find(dsv.ptr);
            if (it != m_device.m_rhiDsvs.end()) {
                m_device.m_context->ClearDepthStencilView(it->second.Get(),
                                                          D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                                          depth,
                                                          stencil);
            }
        }

        void SetVertexBuffers(uint32_t startSlot, uint32_t count, const RhiVertexBufferView* views) override
        {
            if (!m_device.m_context || !views || count == 0) {
                return;
            }

            std::vector<ID3D11Buffer*> buffers(count, nullptr);
            std::vector<UINT> strides(count, 0);
            std::vector<UINT> offsets(count, 0);
            m_boundVertexBuffers.clear();
            m_boundVertexBuffers.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                const auto it = m_device.m_rhiResources.find(views[i].gpuAddress);
                if (it == m_device.m_rhiResources.end()) {
                    continue;
                }
                Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
                if (SUCCEEDED(it->second.As(&buffer))) {
                    buffers[i] = buffer.Get();
                    strides[i] = views[i].strideInBytes;
                    offsets[i] = 0;
                    m_boundVertexBuffers.push_back(buffer);
                }
            }
            m_device.m_context->IASetVertexBuffers(startSlot, count, buffers.data(), strides.data(), offsets.data());
        }

        void SetIndexBuffer(const RhiIndexBufferView& view) override
        {
            if (!m_device.m_context || view.gpuAddress == 0) {
                return;
            }
            const auto it = m_device.m_rhiResources.find(view.gpuAddress);
            if (it == m_device.m_rhiResources.end()) {
                return;
            }
            Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
            if (SUCCEEDED(it->second.As(&buffer))) {
                m_boundIndexBuffer = buffer;
                m_device.m_context->IASetIndexBuffer(buffer.Get(),
                                                     view.is32Bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT,
                                                     0);
            }
        }

        void SetVertexBufferBindings(uint32_t startSlot, uint32_t count, const RhiVertexBufferBinding* bindings) override
        {
            if (!m_device.m_context || !bindings || count == 0) {
                return;
            }

            std::vector<ID3D11Buffer*> buffers(count, nullptr);
            std::vector<UINT> strides(count, 0);
            std::vector<UINT> offsets(count, 0);
            m_boundVertexBuffers.clear();
            m_boundVertexBuffers.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                const auto it = m_device.m_rhiResources.find(bindings[i].buffer.id);
                if (it == m_device.m_rhiResources.end()) {
                    continue;
                }
                Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
                if (SUCCEEDED(it->second.As(&buffer))) {
                    buffers[i] = buffer.Get();
                    strides[i] = bindings[i].strideInBytes;
                    offsets[i] = static_cast<UINT>(bindings[i].offsetInBytes);
                    m_boundVertexBuffers.push_back(buffer);
                }
            }
            m_device.m_context->IASetVertexBuffers(startSlot, count, buffers.data(), strides.data(), offsets.data());
        }

        void SetIndexBufferBinding(const RhiIndexBufferBinding& binding) override
        {
            if (!m_device.m_context || !binding.buffer.IsValid()) {
                return;
            }
            const auto it = m_device.m_rhiResources.find(binding.buffer.id);
            if (it == m_device.m_rhiResources.end()) {
                return;
            }
            Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
            if (SUCCEEDED(it->second.As(&buffer))) {
                m_boundIndexBuffer = buffer;
                m_device.m_context->IASetIndexBuffer(buffer.Get(),
                                                     binding.is32Bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT,
                                                     static_cast<UINT>(binding.offsetInBytes));
            }
        }

        void CopyBuffer(const RhiBufferCopyDesc& copy) override
        {
            if (!m_device.m_context || copy.sizeInBytes == 0 ||
                copy.sourceOffsetInBytes > UINT_MAX ||
                copy.destinationOffsetInBytes > UINT_MAX ||
                copy.sizeInBytes > UINT_MAX) {
                return;
            }
            const auto sourceIt = m_device.m_rhiResources.find(copy.source.id);
            const auto destinationIt = m_device.m_rhiResources.find(copy.destination.id);
            if (sourceIt == m_device.m_rhiResources.end() || destinationIt == m_device.m_rhiResources.end()) {
                return;
            }
            D3D11_BOX sourceBox{};
            sourceBox.left = static_cast<UINT>(copy.sourceOffsetInBytes);
            sourceBox.right = static_cast<UINT>(copy.sourceOffsetInBytes + copy.sizeInBytes);
            sourceBox.top = 0;
            sourceBox.bottom = 1;
            sourceBox.front = 0;
            sourceBox.back = 1;
            m_device.m_context->CopySubresourceRegion(destinationIt->second.Get(),
                                                      0,
                                                      static_cast<UINT>(copy.destinationOffsetInBytes),
                                                      0,
                                                      0,
                                                      sourceIt->second.Get(),
                                                      0,
                                                      &sourceBox);
        }

    private:
        Dx11GraphicsDevice& m_device;
        RhiQueueType m_queueType = RhiQueueType::Graphics;
        std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> m_renderTargets;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_depthStencil;
        std::vector<Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> m_boundRenderTargets;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_boundDepthStencil;
        std::vector<Microsoft::WRL::ComPtr<ID3D11Buffer>> m_boundVertexBuffers;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_boundIndexBuffer;
    };

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
} // namespace SasamiEngine::Render
#endif
