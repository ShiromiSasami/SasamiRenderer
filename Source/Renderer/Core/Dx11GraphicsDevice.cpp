#include "Renderer/Core/Dx11GraphicsDevice.h"

#if RHI_DIRECTX11

#include "Foundation/Tools/DebugOutput.h"

#include <cstring>
#include <utility>

namespace SasamiRenderer
{
    namespace
    {
        DXGI_FORMAT ToDx11Format(RhiFormat format)
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

        UINT ToDx11TextureBindFlags(RhiTextureUsageFlags usage)
        {
            UINT flags = 0;
            if (HasFlag(usage, RhiTextureUsageFlags::ShaderResource)) {
                flags |= D3D11_BIND_SHADER_RESOURCE;
            }
            if (HasFlag(usage, RhiTextureUsageFlags::RenderTarget)) {
                flags |= D3D11_BIND_RENDER_TARGET;
            }
            if (HasFlag(usage, RhiTextureUsageFlags::DepthStencil)) {
                flags |= D3D11_BIND_DEPTH_STENCIL;
            }
            if (HasFlag(usage, RhiTextureUsageFlags::UnorderedAccess)) {
                flags |= D3D11_BIND_UNORDERED_ACCESS;
            }
            return flags;
        }

        UINT ToDx11BufferBindFlags(RhiBufferUsageFlags usage)
        {
            UINT flags = 0;
            if (HasFlag(usage, RhiBufferUsageFlags::Vertex)) {
                flags |= D3D11_BIND_VERTEX_BUFFER;
            }
            if (HasFlag(usage, RhiBufferUsageFlags::Index)) {
                flags |= D3D11_BIND_INDEX_BUFFER;
            }
            if (HasFlag(usage, RhiBufferUsageFlags::Constant)) {
                flags |= D3D11_BIND_CONSTANT_BUFFER;
            }
            if (HasFlag(usage, RhiBufferUsageFlags::ShaderResource) ||
                HasFlag(usage, RhiBufferUsageFlags::Structured)) {
                flags |= D3D11_BIND_SHADER_RESOURCE;
            }
            if (HasFlag(usage, RhiBufferUsageFlags::UnorderedAccess)) {
                flags |= D3D11_BIND_UNORDERED_ACCESS;
            }
            return flags;
        }

        D3D11_SRV_DIMENSION ToDx11SrvDimension(RhiTextureViewDimension dimension)
        {
            switch (dimension) {
            case RhiTextureViewDimension::Texture2DArray: return D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            case RhiTextureViewDimension::TextureCube: return D3D11_SRV_DIMENSION_TEXTURECUBE;
            case RhiTextureViewDimension::Texture2D:
            default: return D3D11_SRV_DIMENSION_TEXTURE2D;
            }
        }

        D3D11_COMPARISON_FUNC ToDx11CompareOp(RhiCompareOp op)
        {
            switch (op) {
            case RhiCompareOp::Never: return D3D11_COMPARISON_NEVER;
            case RhiCompareOp::Less: return D3D11_COMPARISON_LESS;
            case RhiCompareOp::Equal: return D3D11_COMPARISON_EQUAL;
            case RhiCompareOp::LessEqual: return D3D11_COMPARISON_LESS_EQUAL;
            case RhiCompareOp::Greater: return D3D11_COMPARISON_GREATER;
            case RhiCompareOp::NotEqual: return D3D11_COMPARISON_NOT_EQUAL;
            case RhiCompareOp::GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL;
            case RhiCompareOp::Always:
            default: return D3D11_COMPARISON_ALWAYS;
            }
        }

        D3D11_CULL_MODE ToDx11CullMode(RhiCullMode mode)
        {
            switch (mode) {
            case RhiCullMode::None: return D3D11_CULL_NONE;
            case RhiCullMode::Front: return D3D11_CULL_FRONT;
            case RhiCullMode::Back:
            default: return D3D11_CULL_BACK;
            }
        }

        D3D11_FILL_MODE ToDx11FillMode(RhiFillMode mode)
        {
            return mode == RhiFillMode::Wireframe ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
        }

        D3D11_PRIMITIVE_TOPOLOGY ToDx11Topology(RhiPrimitiveTopology topology)
        {
            switch (topology) {
            case RhiPrimitiveTopology::TriangleStrip: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            case RhiPrimitiveTopology::LineList: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
            case RhiPrimitiveTopology::LineStrip: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
            case RhiPrimitiveTopology::PointList: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
            case RhiPrimitiveTopology::PatchList: return D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
            case RhiPrimitiveTopology::TriangleList:
            default: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            }
        }

        D3D11_VIEWPORT ToDx11Viewport(const RhiViewport& viewport)
        {
            D3D11_VIEWPORT out{};
            out.TopLeftX = viewport.x;
            out.TopLeftY = viewport.y;
            out.Width = viewport.width;
            out.Height = viewport.height;
            out.MinDepth = viewport.minDepth;
            out.MaxDepth = viewport.maxDepth;
            return out;
        }

        D3D11_RECT ToDx11Rect(const RhiRect& rect)
        {
            return D3D11_RECT{ rect.left, rect.top, rect.right, rect.bottom };
        }
    }

    class Dx11RhiCommandEncoder final : public IRhiCommandEncoder
    {
    public:
        Dx11RhiCommandEncoder(Dx11GraphicsDevice& device, RhiQueueType queueType)
            : m_device(device)
            , m_queueType(queueType)
        {
        }

        RhiQueueType QueueType() const { return m_queueType; }

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

    Dx11GraphicsDevice::~Dx11GraphicsDevice()
    {
        Cleanup();
    }

    bool Dx11GraphicsDevice::Initialize(HWND hWnd, UINT width, UINT height, UINT bufferCount)
    {
        Cleanup();

        DXGI_SWAP_CHAIN_DESC swapDesc{};
        swapDesc.BufferCount = bufferCount;
        swapDesc.BufferDesc.Width = width;
        swapDesc.BufferDesc.Height = height;
        swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.OutputWindow = hWnd;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.SampleDesc.Quality = 0;
        swapDesc.Windowed = TRUE;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        UINT createFlags = 0;
#if defined(_DEBUG)
        createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        const D3D_FEATURE_LEVEL requestedLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL createdLevel{};

        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr,
                                                   D3D_DRIVER_TYPE_HARDWARE,
                                                   nullptr,
                                                   createFlags,
                                                   requestedLevels,
                                                   _countof(requestedLevels),
                                                   D3D11_SDK_VERSION,
                                                   &swapDesc,
                                                   m_swapChain.GetAddressOf(),
                                                   m_device.GetAddressOf(),
                                                   &createdLevel,
                                                   m_context.GetAddressOf());
        if (FAILED(hr) && (createFlags & D3D11_CREATE_DEVICE_DEBUG)) {
            createFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = D3D11CreateDeviceAndSwapChain(nullptr,
                                               D3D_DRIVER_TYPE_HARDWARE,
                                               nullptr,
                                               createFlags,
                                               requestedLevels,
                                               _countof(requestedLevels),
                                               D3D11_SDK_VERSION,
                                               &swapDesc,
                                               m_swapChain.GetAddressOf(),
                                               m_device.GetAddressOf(),
                                               &createdLevel,
                                               m_context.GetAddressOf());
        }
        if (FAILED(hr)) {
            DebugLog("Dx11GraphicsDevice::Initialize: D3D11CreateDeviceAndSwapChain failed.\n");
            Cleanup();
            return false;
        }

        if (!CreateBackBufferView()) {
            Cleanup();
            return false;
        }

        m_capabilities = {};
        m_capabilities.api = RhiBackendApi::DirectX11;
        m_capabilities.supportsGraphicsQueue = true;
        m_capabilities.supportsComputeQueue = createdLevel >= D3D_FEATURE_LEVEL_11_0;
        m_capabilities.supportsSwapChain = true;
        m_capabilities.supportsNativeFrame = true;
        m_capabilities.supportsFeatureRenderPasses = false;
        m_capabilities.supportsD3D12CompatibilitySurface = false;
        m_capabilities.supportsRhiResourceCreation = true;
        m_capabilities.supportsRhiDescriptorCreation = true;
        m_capabilities.supportsRhiPipelineCreation = true;
        m_capabilities.supportsRhiCommandEncoding = true;
        return true;
    }

    GraphicsRuntime Dx11GraphicsDevice::GetBackend() const
    {
        return GraphicsRuntime::DirectX11;
    }

    void* Dx11GraphicsDevice::GetNativeDeviceHandle() const
    {
        return m_device.Get();
    }

    void* Dx11GraphicsDevice::GetNativeGraphicsQueueHandle() const
    {
        return m_context.Get();
    }

    ID3D12Device* Dx11GraphicsDevice::GetDevice() const
    {
        return nullptr;
    }

    ID3D12Device5* Dx11GraphicsDevice::GetRayTracingDevice() const
    {
        return nullptr;
    }

    const RhiBackendCapabilities& Dx11GraphicsDevice::GetCapabilities() const
    {
        return m_capabilities;
    }

    bool Dx11GraphicsDevice::SupportsHardwareRayTracing() const
    {
        return false;
    }

    CommandQueue& Dx11GraphicsDevice::GetCommandQueue()
    {
        return m_emptyGraphicsQueue;
    }

    CommandQueue& Dx11GraphicsDevice::GetComputeQueue()
    {
        return m_emptyComputeQueue;
    }

    SwapChain& Dx11GraphicsDevice::GetSwapChain()
    {
        return m_emptySwapChain;
    }

    UINT Dx11GraphicsDevice::GetDescriptorHandleIncrementSize(DescriptorHeapType) const
    {
        return 0;
    }

    void Dx11GraphicsDevice::WaitForGPU()
    {
        if (m_context) {
            m_context->Flush();
        }
    }

    bool Dx11GraphicsDevice::ExecuteBackendFrame(const RhiBackendFrameDesc& frameDesc)
    {
        if (!m_context || !m_swapChain || !m_backBufferRtv || !frameDesc.present) {
            return false;
        }

        const float clear[] = {
            frameDesc.clearColor.r,
            frameDesc.clearColor.g,
            frameDesc.clearColor.b,
            frameDesc.clearColor.a,
        };
        ID3D11RenderTargetView* rtv = m_backBufferRtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, nullptr);
        m_context->ClearRenderTargetView(m_backBufferRtv.Get(), clear);
        return SUCCEEDED(m_swapChain->Present(1, 0));
    }

    bool Dx11GraphicsDevice::RenderBackendClearFrame(const float clearColor[4])
    {
        RhiBackendFrameDesc frameDesc{};
        if (clearColor) {
            frameDesc.clearColor = { clearColor[0], clearColor[1], clearColor[2], clearColor[3] };
        }
        frameDesc.present = true;
        return ExecuteBackendFrame(frameDesc);
    }


    HRESULT Dx11GraphicsDevice::CreateDescriptorHeap(const DescriptorHeapDesc&, DescriptorHeap&)
    {
        return E_NOTIMPL;
    }

    HRESULT Dx11GraphicsDevice::CreateCommittedResource(const HeapProperties*,
                                                        HeapFlags,
                                                        const ResourceDesc*,
                                                        ResourceState,
                                                        const ClearValue*,
                                                        Resource&)
    {
        return E_NOTIMPL;
    }

    HRESULT Dx11GraphicsDevice::CreateCommandAllocator(CommandListType, CommandAllocator&)
    {
        return E_NOTIMPL;
    }

    HRESULT Dx11GraphicsDevice::CreateCommandList(UINT, CommandListType, CommandAllocator&, PipelineState*, CommandList&)
    {
        return E_NOTIMPL;
    }

    HRESULT Dx11GraphicsDevice::CreateGraphicsPipelineState(const GraphicsPipelineDesc&, PipelineState&)
    {
        return E_NOTIMPL;
    }

    HRESULT Dx11GraphicsDevice::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC&, PipelineState&)
    {
        return E_NOTIMPL;
    }

    HRESULT Dx11GraphicsDevice::CreatePipelineStateFromStream(const void*, size_t, PipelineState&)
    {
        return E_NOTIMPL;
    }

    HRESULT Dx11GraphicsDevice::CreateRootSignature(UINT, const void*, size_t, RootSignature&)
    {
        return E_NOTIMPL;
    }

    void Dx11GraphicsDevice::CreateShaderResourceView(Resource&, const ShaderResourceViewDesc*, CpuDescriptorHandle)
    {
    }

    void Dx11GraphicsDevice::CreateDepthStencilView(Resource&, const DepthStencilViewDesc*, CpuDescriptorHandle)
    {
    }

    void Dx11GraphicsDevice::CreateRenderTargetView(Resource&, const D3D12_RENDER_TARGET_VIEW_DESC*, CpuDescriptorHandle)
    {
    }

    void Dx11GraphicsDevice::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC*, CpuDescriptorHandle)
    {
    }

    void Dx11GraphicsDevice::CreateSampler(const D3D12_SAMPLER_DESC*, CpuDescriptorHandle)
    {
    }

    HRESULT Dx11GraphicsDevice::CreateFence(UINT64, D3D12_FENCE_FLAGS, ID3D12Fence**)
    {
        return E_NOTIMPL;
    }

    bool Dx11GraphicsDevice::CreateBackBufferView()
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
        if (FAILED(hr)) {
            DebugLog("Dx11GraphicsDevice::CreateBackBufferView: GetBuffer failed.\n");
            return false;
        }

        hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, m_backBufferRtv.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11GraphicsDevice::CreateBackBufferView: CreateRenderTargetView failed.\n");
            return false;
        }
        return true;
    }

    void Dx11GraphicsDevice::Cleanup()
    {
        if (m_context) {
            m_context->ClearState();
            m_context->Flush();
        }
        m_backBufferRtv.Reset();
        m_swapChain.Reset();
        m_rhiResources.clear();
        m_rhiShaders.clear();
        m_rhiPipelineLayouts.clear();
        m_rhiPipelines.clear();
        m_rhiSrvs.clear();
        m_rhiRtvs.clear();
        m_rhiDsvs.clear();
        m_nextRhiResourceHandle = 1;
        m_nextRhiDescriptorHandle = 1;
        m_nextRhiShaderHandle = 1;
        m_nextRhiPipelineLayoutHandle = 1;
        m_nextRhiPipelineHandle = 1;
        m_context.Reset();
        m_device.Reset();
        m_capabilities = {};
    }
}

#endif
