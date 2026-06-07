#include "Renderer/Backends/DirectX11/Dx11GraphicsDevice.h"

#if RHI_DIRECTX11

#include "Foundation/Tools/DebugOutput.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <utility>

#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace SasamiRenderer
{
    namespace
    {
        struct Dx11RayMarchConstants
        {
            float invViewProjection[16];
            float cameraPos[3];
            float sceneTimeSec;
            float sunDir[3];
            float sunIntensity;
            float sunColor[3];
            float cloudCover;
            float renderWidth;
            float renderHeight;
            float fluidMode;
            float cloudDensity;
            float extra0[4];
            float extra1[4];
            float extra2[4];
        };

        struct Dx11MeshConstants
        {
            float viewProjection[16];
            float model[16];
            float baseColor[4];
            float emissiveRoughness[4];
            float lightDirIntensity[4];
            float lightColor[4];
            float cameraPosMetallic[4];
            float featureFlags[4];
            float featureFlags2[4];
        };

        static_assert((sizeof(Dx11RayMarchConstants) % 16) == 0,
                      "D3D11 constant buffers must be 16-byte aligned.");
        static_assert((sizeof(Dx11MeshConstants) % 16) == 0,
                      "D3D11 constant buffers must be 16-byte aligned.");

        const char* kDx11MeshVertexShader = R"(
cbuffer MeshCB : register(b0)
{
    row_major float4x4 u_viewProjection;
    row_major float4x4 u_model;
    float4 u_baseColor;
    float4 u_emissiveRoughness;
    float4 u_lightDirIntensity;
    float4 u_lightColor;
    float4 u_cameraPosMetallic;
    float4 u_featureFlags;
    float4 u_featureFlags2;
}

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float4 color    : COLOR0;
    float2 uv       : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

PSInput VSMain(VSInput input)
{
    PSInput o;
    float4 worldPos = mul(float4(input.position, 1.0), u_model);
    o.position = mul(worldPos, u_viewProjection);
    o.worldPos = worldPos.xyz;
    o.worldN = normalize(mul(input.normal, (float3x3)u_model));
    o.color = input.color * u_baseColor;
    o.uv = input.uv;
    return o;
}
)";

        const char* kDx11MeshPixelShader = R"(
cbuffer MeshCB : register(b0)
{
    row_major float4x4 u_viewProjection;
    row_major float4x4 u_model;
    float4 u_baseColor;
    float4 u_emissiveRoughness;
    float4 u_lightDirIntensity;
    float4 u_lightColor;
    float4 u_cameraPosMetallic;
    float4 u_featureFlags;
    float4 u_featureFlags2;
}

Texture2D AlbedoTex : register(t0);
SamplerState LinearWrap : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float4 color    : COLOR0;
    float2 uv       : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 texColor = (u_featureFlags.x > 0.5) ? AlbedoTex.Sample(LinearWrap, input.uv) : float4(1.0, 1.0, 1.0, 1.0);
    float3 n = normalize(input.worldN);
    float3 l = normalize(-u_lightDirIntensity.xyz);
    float ndotl = saturate(dot(n, l));
    float roughness = saturate(u_emissiveRoughness.w);
    float metallic = saturate(u_cameraPosMetallic.w);
    float iblBoost = (u_featureFlags.y > 0.5) ? 0.10 : 0.0;
    float ssaoScale = (u_featureFlags.z > 0.5) ? 0.92 : 1.0;
    float shadowScale = (u_featureFlags.w > 0.5) ? 0.88 : 1.0;
    float ambient = (lerp(0.18, 0.08, metallic) + iblBoost) * ssaoScale;
    float diffuse = ambient + ndotl * u_lightDirIntensity.w;
    float3 lit = input.color.rgb * texColor.rgb * u_lightColor.rgb * (ambient + (diffuse - ambient) * shadowScale);
    lit += u_emissiveRoughness.rgb;
    lit = lerp(lit, lit * (1.0 - roughness) + input.color.rgb * 0.15, metallic * 0.25);
    if (u_featureFlags2.x > 0.5) {
        lit = lit / (lit + 1.0);
        lit = pow(saturate(lit), 1.0 / 2.2);
    }
    return float4(saturate(lit), input.color.a * texColor.a);
}
)";

        std::filesystem::path GetExecutableDir()
        {
            wchar_t exePath[MAX_PATH] = {};
            const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (len == 0 || len == MAX_PATH) {
                return std::filesystem::current_path();
            }
            return std::filesystem::path(exePath).parent_path();
        }

        std::filesystem::path FindShaderFile(const wchar_t* relativePath)
        {
            std::error_code ec;
            std::array<std::filesystem::path, 2> starts = {
                std::filesystem::current_path(ec),
                GetExecutableDir(),
            };

            for (const auto& start : starts) {
                std::filesystem::path dir = std::filesystem::absolute(start, ec);
                if (ec) {
                    dir = start;
                    ec.clear();
                }

                for (;;) {
                    const std::filesystem::path candidate =
                        dir / L"Source" / L"Renderer" / L"Shaders" / relativePath;
                    if (std::filesystem::exists(candidate, ec)) {
                        return candidate;
                    }

                    const std::filesystem::path parent = dir.parent_path();
                    if (parent.empty() || parent == dir) {
                        break;
                    }
                    dir = parent;
                }
            }

            return {};
        }

        bool CompileDx11Shader(const wchar_t* relativePath,
                               const char* entryPoint,
                               const char* profile,
                               Microsoft::WRL::ComPtr<ID3DBlob>& outBlob)
        {
            const std::filesystem::path shaderPath = FindShaderFile(relativePath);
            if (shaderPath.empty()) {
                DebugLog("Dx11GraphicsDevice: shader file was not found.\n");
                return false;
            }

            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
            flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

            Microsoft::WRL::ComPtr<ID3DBlob> errors;
            const HRESULT hr = D3DCompileFromFile(shaderPath.c_str(),
                                                  nullptr,
                                                  D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                  entryPoint,
                                                  profile,
                                                  flags,
                                                  0,
                                                  outBlob.GetAddressOf(),
                                                  errors.GetAddressOf());
            if (FAILED(hr)) {
                DebugLog("Dx11GraphicsDevice: shader compilation failed.\n");
                if (errors && errors->GetBufferPointer()) {
                    DebugLog(static_cast<const char*>(errors->GetBufferPointer()));
                    DebugLog("\n");
                }
                return false;
            }
            return true;
        }

        bool CompileDx11ShaderSource(const char* source,
                                     const char* entryPoint,
                                     const char* profile,
                                     Microsoft::WRL::ComPtr<ID3DBlob>& outBlob)
        {
            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
            flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
            Microsoft::WRL::ComPtr<ID3DBlob> errors;
            const HRESULT hr = D3DCompile(source,
                                          std::strlen(source),
                                          nullptr,
                                          nullptr,
                                          nullptr,
                                          entryPoint,
                                          profile,
                                          flags,
                                          0,
                                          outBlob.GetAddressOf(),
                                          errors.GetAddressOf());
            if (FAILED(hr)) {
                DebugLog("Dx11GraphicsDevice: shader source compilation failed.\n");
                if (errors && errors->GetBufferPointer()) {
                    DebugLog(static_cast<const char*>(errors->GetBufferPointer()));
                    DebugLog("\n");
                }
                return false;
            }
            return true;
        }

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

    bool Dx11GraphicsDevice::EnsureRayMarchResources()
    {
        if (!m_device) {
            return false;
        }
        if (m_rayMarchVertexShader && m_rayMarchPixelShader && m_rayMarchConstantBuffer) {
            return true;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
        if (!CompileDx11Shader(L"RayMarch/RayMarch_VS.hlsl", "VSMain", "vs_5_0", vsBlob) ||
            !CompileDx11Shader(L"RayMarch/RayMarch_PS.hlsl", "PSMain", "ps_5_0", psBlob)) {
            return false;
        }

        HRESULT hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                                  vsBlob->GetBufferSize(),
                                                  nullptr,
                                                  m_rayMarchVertexShader.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11GraphicsDevice::EnsureRayMarchResources: CreateVertexShader failed.\n");
            return false;
        }

        hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(),
                                         psBlob->GetBufferSize(),
                                         nullptr,
                                         m_rayMarchPixelShader.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11GraphicsDevice::EnsureRayMarchResources: CreatePixelShader failed.\n");
            return false;
        }

        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = static_cast<UINT>(sizeof(Dx11RayMarchConstants));
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = m_device->CreateBuffer(&cbDesc, nullptr, m_rayMarchConstantBuffer.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11GraphicsDevice::EnsureRayMarchResources: CreateBuffer failed.\n");
            return false;
        }

        return true;
    }

    bool Dx11GraphicsDevice::RenderRayMarchFrame(const RhiBackendRayMarchFrameDesc& desc)
    {
        if (!m_context || !m_backBufferRtv || !EnsureRayMarchResources()) {
            return false;
        }

        Dx11RayMarchConstants constants{};
        std::memcpy(constants.invViewProjection, desc.invViewProjection, sizeof(constants.invViewProjection));
        constants.cameraPos[0] = desc.cameraPos[0];
        constants.cameraPos[1] = desc.cameraPos[1];
        constants.cameraPos[2] = desc.cameraPos[2];
        constants.sceneTimeSec = desc.sceneTimeSec;
        constants.sunDir[0] = desc.sunDir[0];
        constants.sunDir[1] = desc.sunDir[1];
        constants.sunDir[2] = desc.sunDir[2];
        constants.sunIntensity = desc.sunIntensity;
        constants.sunColor[0] = desc.sunColor[0];
        constants.sunColor[1] = desc.sunColor[1];
        constants.sunColor[2] = desc.sunColor[2];
        constants.cloudCover = desc.cloudCover;
        constants.renderWidth = desc.renderWidth;
        constants.renderHeight = desc.renderHeight;
        constants.fluidMode = desc.fluidMode;
        constants.cloudDensity = desc.cloudDensity;
        constants.extra0[0] = desc.debugMode;
        constants.extra0[1] = desc.tanHalfFovY * desc.aspectRatio;
        constants.extra0[2] = desc.tanHalfFovY;
        constants.extra0[3] = desc.explicitCameraBasis ? 1.0f : 0.0f;
        constants.extra1[0] = desc.cameraRight[0];
        constants.extra1[1] = desc.cameraRight[1];
        constants.extra1[2] = desc.cameraRight[2];
        constants.extra2[0] = desc.cameraUp[0];
        constants.extra2[1] = desc.cameraUp[1];
        constants.extra2[2] = desc.cameraUp[2];

        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = m_context->Map(m_rayMarchConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            DebugLog("Dx11GraphicsDevice::RenderRayMarchFrame: constant buffer map failed.\n");
            return false;
        }
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        m_context->Unmap(m_rayMarchConstantBuffer.Get(), 0);

        ID3D11RenderTargetView* rtv = m_backBufferRtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, nullptr);

        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = desc.renderWidth;
        viewport.Height = desc.renderHeight;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &viewport);

        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_rayMarchVertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(m_rayMarchPixelShader.Get(), nullptr, 0);
        m_context->GSSetShader(nullptr, nullptr, 0);
        m_context->HSSetShader(nullptr, nullptr, 0);
        m_context->DSSetShader(nullptr, nullptr, 0);
        m_context->CSSetShader(nullptr, nullptr, 0);
        ID3D11Buffer* cb = m_rayMarchConstantBuffer.Get();
        m_context->PSSetConstantBuffers(0, 1, &cb);
        m_context->Draw(3, 0);
        return true;
    }

    bool Dx11GraphicsDevice::EnsureMeshFrameResources(UINT width, UINT height)
    {
        if (!m_device || width == 0 || height == 0) {
            return false;
        }

        if (!m_meshVertexShader || !m_meshPixelShader || !m_meshInputLayout) {
            Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
            Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
            if (!CompileDx11ShaderSource(kDx11MeshVertexShader, "VSMain", "vs_5_0", vsBlob) ||
                !CompileDx11ShaderSource(kDx11MeshPixelShader, "PSMain", "ps_5_0", psBlob)) {
                return false;
            }

            HRESULT hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                                      vsBlob->GetBufferSize(),
                                                      nullptr,
                                                      m_meshVertexShader.GetAddressOf());
            if (FAILED(hr)) {
                DebugLog("Dx11GraphicsDevice::EnsureMeshFrameResources: CreateVertexShader failed.\n");
                return false;
            }

            hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(),
                                             psBlob->GetBufferSize(),
                                             nullptr,
                                             m_meshPixelShader.GetAddressOf());
            if (FAILED(hr)) {
                DebugLog("Dx11GraphicsDevice::EnsureMeshFrameResources: CreatePixelShader failed.\n");
                return false;
            }

            D3D11_INPUT_ELEMENT_DESC inputElements[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            hr = m_device->CreateInputLayout(inputElements,
                                             _countof(inputElements),
                                             vsBlob->GetBufferPointer(),
                                             vsBlob->GetBufferSize(),
                                             m_meshInputLayout.GetAddressOf());
            if (FAILED(hr)) {
                DebugLog("Dx11GraphicsDevice::EnsureMeshFrameResources: CreateInputLayout failed.\n");
                return false;
            }
        }

        if (!m_meshConstantBuffer) {
            D3D11_BUFFER_DESC cbDesc{};
            cbDesc.ByteWidth = static_cast<UINT>(sizeof(Dx11MeshConstants));
            cbDesc.Usage = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(m_device->CreateBuffer(&cbDesc, nullptr, m_meshConstantBuffer.GetAddressOf()))) {
                DebugLog("Dx11GraphicsDevice::EnsureMeshFrameResources: CreateBuffer failed.\n");
                return false;
            }
        }

        if (!m_meshSamplerState) {
            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampler.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
            sampler.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
            sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            sampler.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(m_device->CreateSamplerState(&sampler, m_meshSamplerState.GetAddressOf()))) {
                return false;
            }
        }

        if (!m_meshRasterizerState) {
            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode = D3D11_FILL_SOLID;
            raster.CullMode = D3D11_CULL_NONE;
            raster.DepthClipEnable = TRUE;
            raster.ScissorEnable = FALSE;
            if (FAILED(m_device->CreateRasterizerState(&raster, m_meshRasterizerState.GetAddressOf()))) {
                return false;
            }
        }

        if (!m_meshDepthStencilState) {
            D3D11_DEPTH_STENCIL_DESC depth{};
            depth.DepthEnable = TRUE;
            depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
            if (FAILED(m_device->CreateDepthStencilState(&depth, m_meshDepthStencilState.GetAddressOf()))) {
                return false;
            }
        }

        if (!m_meshBlendState) {
            D3D11_BLEND_DESC blend{};
            blend.RenderTarget[0].BlendEnable = TRUE;
            blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(m_device->CreateBlendState(&blend, m_meshBlendState.GetAddressOf()))) {
                return false;
            }
        }

        if (!m_backBufferDsv || m_backBufferDepthWidth != width || m_backBufferDepthHeight != height) {
            m_backBufferDsv.Reset();
            m_backBufferDepth.Reset();

            D3D11_TEXTURE2D_DESC depthDesc{};
            depthDesc.Width = width;
            depthDesc.Height = height;
            depthDesc.MipLevels = 1;
            depthDesc.ArraySize = 1;
            depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
            depthDesc.SampleDesc.Count = 1;
            depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            HRESULT hr = m_device->CreateTexture2D(&depthDesc, nullptr, m_backBufferDepth.GetAddressOf());
            if (FAILED(hr)) {
                DebugLog("Dx11GraphicsDevice::EnsureMeshFrameResources: depth texture creation failed.\n");
                return false;
            }

            D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
            dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            hr = m_device->CreateDepthStencilView(m_backBufferDepth.Get(), &dsvDesc, m_backBufferDsv.GetAddressOf());
            if (FAILED(hr)) {
                DebugLog("Dx11GraphicsDevice::EnsureMeshFrameResources: depth view creation failed.\n");
                return false;
            }

            m_backBufferDepthWidth = width;
            m_backBufferDepthHeight = height;
        }

        return true;
    }

    bool Dx11GraphicsDevice::RenderMeshFrame(const RhiBackendMeshFrameDesc& desc)
    {
        if (!m_context || !m_backBufferRtv || !desc.draws || desc.drawCount == 0) {
            return false;
        }

        const UINT width = static_cast<UINT>(desc.renderWidth);
        const UINT height = static_cast<UINT>(desc.renderHeight);
        if (!EnsureMeshFrameResources(width, height)) {
            return false;
        }

        ID3D11RenderTargetView* rtv = m_backBufferRtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, m_backBufferDsv.Get());
        m_context->ClearDepthStencilView(m_backBufferDsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = desc.renderWidth;
        viewport.Height = desc.renderHeight;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &viewport);

        m_context->IASetInputLayout(m_meshInputLayout.Get());
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_meshVertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(m_meshPixelShader.Get(), nullptr, 0);
        m_context->GSSetShader(nullptr, nullptr, 0);
        m_context->HSSetShader(nullptr, nullptr, 0);
        m_context->DSSetShader(nullptr, nullptr, 0);
        m_context->CSSetShader(nullptr, nullptr, 0);
        m_context->RSSetState(m_meshRasterizerState.Get());
        m_context->OMSetDepthStencilState(m_meshDepthStencilState.Get(), 0);
        const float blendFactor[4] = {};
        m_context->OMSetBlendState(m_meshBlendState.Get(), blendFactor, 0xffffffffu);

        ID3D11Buffer* cb = m_meshConstantBuffer.Get();
        m_context->VSSetConstantBuffers(0, 1, &cb);
        m_context->PSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState* sampler = m_meshSamplerState.Get();
        m_context->PSSetSamplers(0, 1, &sampler);

        for (uint32_t i = 0; i < desc.drawCount; ++i) {
            const RhiBackendMeshDrawDesc& draw = desc.draws[i];
            if (draw.vertexBufferHandle == 0 || (draw.indexCount == 0 && draw.vertexCount == 0)) {
                continue;
            }

            const auto vbIt = m_rhiResources.find(draw.vertexBufferHandle);
            if (vbIt == m_rhiResources.end()) {
                continue;
            }
            Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
            if (FAILED(vbIt->second.As(&vertexBuffer))) {
                continue;
            }

            Dx11MeshConstants constants{};
            std::memcpy(constants.viewProjection, desc.viewProjection, sizeof(constants.viewProjection));
            std::memcpy(constants.model, draw.model, sizeof(constants.model));
            std::memcpy(constants.baseColor, draw.baseColor, sizeof(constants.baseColor));
            constants.emissiveRoughness[0] = draw.emissive[0];
            constants.emissiveRoughness[1] = draw.emissive[1];
            constants.emissiveRoughness[2] = draw.emissive[2];
            constants.emissiveRoughness[3] = draw.roughness;
            constants.lightDirIntensity[0] = desc.sunDir[0];
            constants.lightDirIntensity[1] = desc.sunDir[1];
            constants.lightDirIntensity[2] = desc.sunDir[2];
            constants.lightDirIntensity[3] = desc.sunIntensity;
            constants.lightColor[0] = desc.sunColor[0];
            constants.lightColor[1] = desc.sunColor[1];
            constants.lightColor[2] = desc.sunColor[2];
            constants.lightColor[3] = 1.0f;
            constants.cameraPosMetallic[0] = desc.cameraPos[0];
            constants.cameraPosMetallic[1] = desc.cameraPos[1];
            constants.cameraPosMetallic[2] = desc.cameraPos[2];
            constants.cameraPosMetallic[3] = draw.metallic;
            constants.featureFlags[0] = draw.albedoSrv != 0 ? 1.0f : 0.0f;
            constants.featureFlags[1] = desc.enableIbl ? 1.0f : 0.0f;
            constants.featureFlags[2] = desc.enableSsao ? 1.0f : 0.0f;
            constants.featureFlags[3] = desc.enableShadow ? 1.0f : 0.0f;
            constants.featureFlags2[0] = desc.enablePostProcess ? 1.0f : 0.0f;

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(m_context->Map(m_meshConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                continue;
            }
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            m_context->Unmap(m_meshConstantBuffer.Get(), 0);

            UINT stride = draw.vertexStride;
            UINT offset = 0;
            ID3D11Buffer* vb = vertexBuffer.Get();
            m_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);

            ID3D11ShaderResourceView* albedoSrv = nullptr;
            const auto srvIt = m_rhiSrvs.find(draw.albedoSrv);
            if (srvIt != m_rhiSrvs.end()) {
                albedoSrv = srvIt->second.Get();
            }
            m_context->PSSetShaderResources(0, 1, &albedoSrv);

            if (draw.indexBufferHandle != 0 && draw.indexCount > 0) {
                const auto ibIt = m_rhiResources.find(draw.indexBufferHandle);
                if (ibIt == m_rhiResources.end()) {
                    continue;
                }
                Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
                if (FAILED(ibIt->second.As(&indexBuffer))) {
                    continue;
                }
                m_context->IASetIndexBuffer(indexBuffer.Get(),
                                            draw.index32Bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT,
                                            0);
                m_context->DrawIndexed(draw.indexCount, 0, 0);
            } else {
                m_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
                m_context->Draw(draw.vertexCount, 0);
            }
        }

        ID3D11ShaderResourceView* nullSrv = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSrv);

        return true;
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
        if (frameDesc.rayMarch.enabled) {
            (void)RenderRayMarchFrame(frameDesc.rayMarch);
        } else if (frameDesc.mesh.enabled) {
            (void)RenderMeshFrame(frameDesc.mesh);
        }
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

    bool Dx11GraphicsDevice::ResizeBackendSwapChain(UINT width, UINT height)
    {
        if (!m_swapChain || !m_device || width == 0 || height == 0) {
            return false;
        }

        if (m_context) {
            m_context->OMSetRenderTargets(0, nullptr, nullptr);
            m_context->Flush();
        }

        m_backBufferDsv.Reset();
        m_backBufferDepth.Reset();
        m_backBufferDepthWidth = 0;
        m_backBufferDepthHeight = 0;
        m_backBufferRtv.Reset();
        HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            (void)CreateBackBufferView();
            DebugLog("Dx11GraphicsDevice::ResizeBackendSwapChain: ResizeBuffers failed.\n");
            return false;
        }

        return CreateBackBufferView();
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
        m_backBufferDsv.Reset();
        m_backBufferDepth.Reset();
        m_backBufferDepthWidth = 0;
        m_backBufferDepthHeight = 0;
        m_rayMarchVertexShader.Reset();
        m_rayMarchPixelShader.Reset();
        m_rayMarchConstantBuffer.Reset();
        m_meshVertexShader.Reset();
        m_meshPixelShader.Reset();
        m_meshInputLayout.Reset();
        m_meshConstantBuffer.Reset();
        m_meshSamplerState.Reset();
        m_meshRasterizerState.Reset();
        m_meshDepthStencilState.Reset();
        m_meshBlendState.Reset();
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
