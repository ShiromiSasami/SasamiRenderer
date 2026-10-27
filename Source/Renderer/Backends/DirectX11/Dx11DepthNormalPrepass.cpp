// Dx11DepthNormalPrepass.cpp
// The depth + world-normal prepass that feeds the native DirectX 11 SSAO generator. Split out of
// the device for the same reason Dx11ShadowMap is: it owns its own targets, states and pipeline.
#include "Renderer/Backends/DirectX11/Dx11DepthNormalPrepass.h"
#include "Renderer/Backends/DirectX11/Dx11ConversionUtils.h"

#if RHI_DIRECTX11

#include "Foundation/Tools/DebugOutput.h"

#include <cstring>

namespace SasamiEngine::Render
{
    namespace
    {
        // Field order must match DepthNormalCB in NativeDepthNormalCommon.hlsli.
        struct Dx11DepthNormalConstants
        {
            float viewProjection[16];
            float model[16];
        };
        static_assert((sizeof(Dx11DepthNormalConstants) % 16) == 0,
                      "D3D11 constant buffers must be 16-byte aligned.");

        struct Dx11DepthNormalBoneConstants
        {
            float boneMatrices[128 * 16];
        };
        static_assert((sizeof(Dx11DepthNormalBoneConstants) % 16) == 0,
                      "D3D11 constant buffers must be 16-byte aligned.");

        constexpr float kIdentityMatrix[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };
    }

    bool Dx11DepthNormalPrepass::Ensure(ID3D11Device* device, uint32_t width, uint32_t height)
    {
        if (device == nullptr || width == 0 || height == 0) {
            return false;
        }
        if (m_shaderCompileFailed) {
            return false;
        }
        if (!CreateShaders(device) || !CreateStates(device) || !CreateConstantBuffers(device)) {
            return false;
        }
        if (m_width != width || m_height != height || !IsValid()) {
            if (!CreateTargets(device, width, height)) {
                return false;
            }
        }
        return IsValid();
    }

    bool Dx11DepthNormalPrepass::CreateShaders(ID3D11Device* device)
    {
        if (m_staticVertexShader && m_pixelShader) {
            return true;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
        if (!CompileDx11Shader(L"Native/Dx11/NativeDepthNormal_VS.hlsl", "VSMain", "vs_5_0", vsBlob) ||
            !CompileDx11Shader(L"Native/Dx11/NativeDepthNormal_PS.hlsl", "PSMain", "ps_5_0", psBlob)) {
            DebugLog("Dx11DepthNormalPrepass::CreateShaders: prepass shader compilation failed.\n");
            m_shaderCompileFailed = true;
            return false;
        }

        if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                              vsBlob->GetBufferSize(),
                                              nullptr,
                                              m_staticVertexShader.GetAddressOf())) ||
            FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(),
                                             psBlob->GetBufferSize(),
                                             nullptr,
                                             m_pixelShader.GetAddressOf()))) {
            DebugLog("Dx11DepthNormalPrepass::CreateShaders: shader creation failed.\n");
            m_shaderCompileFailed = true;
            return false;
        }

        // Offsets match the native mesh vertex layout in EnsureMeshFrameResources; only the two
        // elements this pass actually reads are declared.
        const D3D11_INPUT_ELEMENT_DESC staticElements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(device->CreateInputLayout(staticElements,
                                             _countof(staticElements),
                                             vsBlob->GetBufferPointer(),
                                             vsBlob->GetBufferSize(),
                                             m_staticInputLayout.GetAddressOf()))) {
            DebugLog("Dx11DepthNormalPrepass::CreateShaders: static CreateInputLayout failed.\n");
            m_shaderCompileFailed = true;
            return false;
        }

        // Skinned geometry is optional in exactly the way it is for the shadow map: a failure here
        // costs skinned meshes their occlusion contribution, not the whole pass.
        Microsoft::WRL::ComPtr<ID3DBlob> skinnedVsBlob;
        if (CompileDx11Shader(L"Native/Dx11/NativeDepthNormalSkinned_VS.hlsl", "VSMain", "vs_5_0", skinnedVsBlob) &&
            SUCCEEDED(device->CreateVertexShader(skinnedVsBlob->GetBufferPointer(),
                                                 skinnedVsBlob->GetBufferSize(),
                                                 nullptr,
                                                 m_skinnedVertexShader.GetAddressOf()))) {
            // "JOINTS_"/"WEIGHTS_" without the trailing digit: D3D11 splits an HLSL semantic into
            // a name and a numeric index, so JOINTS_0 is the name "JOINTS_" at index 0. Same
            // spelling as Dx11ShadowMap's skinned layout.
            const D3D11_INPUT_ELEMENT_DESC skinnedElements[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "JOINTS_",  0, DXGI_FORMAT_R8G8B8A8_UINT,      0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "WEIGHTS_", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            if (FAILED(device->CreateInputLayout(skinnedElements,
                                                 _countof(skinnedElements),
                                                 skinnedVsBlob->GetBufferPointer(),
                                                 skinnedVsBlob->GetBufferSize(),
                                                 m_skinnedInputLayout.GetAddressOf()))) {
                m_skinnedVertexShader.Reset();
                m_skinnedInputLayout.Reset();
                DebugLog("Dx11DepthNormalPrepass::CreateShaders: skinned layout unavailable, "
                         "skinned meshes will not occlude.\n");
            }
        } else {
            DebugLog("Dx11DepthNormalPrepass::CreateShaders: skinned prepass shader unavailable, "
                     "skinned meshes will not occlude.\n");
        }

        return true;
    }

    bool Dx11DepthNormalPrepass::CreateStates(ID3D11Device* device)
    {
        if (!m_rasterizerState) {
            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode = D3D11_FILL_SOLID;
            // Matches the native mesh and shadow passes, which both run unculled.
            raster.CullMode = D3D11_CULL_NONE;
            raster.DepthClipEnable = TRUE;
            if (FAILED(device->CreateRasterizerState(&raster, m_rasterizerState.GetAddressOf()))) {
                DebugLog("Dx11DepthNormalPrepass::CreateStates: CreateRasterizerState failed.\n");
                return false;
            }
        }

        if (!m_depthStencilState) {
            D3D11_DEPTH_STENCIL_DESC depth{};
            depth.DepthEnable = TRUE;
            depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            // Reverse-Z: near = 1.0, far = 0.0, so the nearest fragment is the greatest.
            depth.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
            if (FAILED(device->CreateDepthStencilState(&depth, m_depthStencilState.GetAddressOf()))) {
                DebugLog("Dx11DepthNormalPrepass::CreateStates: CreateDepthStencilState failed.\n");
                return false;
            }
        }

        if (!m_blendState) {
            D3D11_BLEND_DESC blend{};
            blend.RenderTarget[0].BlendEnable = FALSE;
            blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(device->CreateBlendState(&blend, m_blendState.GetAddressOf()))) {
                DebugLog("Dx11DepthNormalPrepass::CreateStates: CreateBlendState failed.\n");
                return false;
            }
        }
        return true;
    }

    bool Dx11DepthNormalPrepass::CreateConstantBuffers(ID3D11Device* device)
    {
        if (!m_constantBuffer) {
            D3D11_BUFFER_DESC cbDesc{};
            cbDesc.ByteWidth = static_cast<UINT>(sizeof(Dx11DepthNormalConstants));
            cbDesc.Usage = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&cbDesc, nullptr, m_constantBuffer.GetAddressOf()))) {
                DebugLog("Dx11DepthNormalPrepass::CreateConstantBuffers: CreateBuffer (b0) failed.\n");
                return false;
            }
        }
        if (!m_boneConstantBuffer) {
            D3D11_BUFFER_DESC boneDesc{};
            boneDesc.ByteWidth = static_cast<UINT>(sizeof(Dx11DepthNormalBoneConstants));
            boneDesc.Usage = D3D11_USAGE_DYNAMIC;
            boneDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            boneDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&boneDesc, nullptr, m_boneConstantBuffer.GetAddressOf()))) {
                DebugLog("Dx11DepthNormalPrepass::CreateConstantBuffers: CreateBuffer (b1) failed.\n");
                return false;
            }
        }
        return true;
    }

    bool Dx11DepthNormalPrepass::CreateTargets(ID3D11Device* device, uint32_t width, uint32_t height)
    {
        m_dsv.Reset();
        m_depthSrv.Reset();
        m_depthTexture.Reset();
        m_normalRtv.Reset();
        m_normalSrv.Reset();
        m_normalTexture.Reset();
        m_width = 0;
        m_height = 0;

        D3D11_TEXTURE2D_DESC depthDesc{};
        depthDesc.Width = width;
        depthDesc.Height = height;
        depthDesc.MipLevels = 1;
        depthDesc.ArraySize = 1;
        // Typeless so one resource backs both the D32_FLOAT depth view written here and the
        // R32_FLOAT shader view the SSAO shaders sample, the same pairing Dx11ShadowMap uses.
        depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Usage = D3D11_USAGE_DEFAULT;
        depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&depthDesc, nullptr, m_depthTexture.GetAddressOf()))) {
            DebugLog("Dx11DepthNormalPrepass::CreateTargets: depth texture creation failed.\n");
            return false;
        }

        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(device->CreateDepthStencilView(m_depthTexture.Get(), &dsvDesc, m_dsv.GetAddressOf()))) {
            DebugLog("Dx11DepthNormalPrepass::CreateTargets: depth view creation failed.\n");
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
        depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        depthSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(m_depthTexture.Get(),
                                                    &depthSrvDesc,
                                                    m_depthSrv.GetAddressOf()))) {
            DebugLog("Dx11DepthNormalPrepass::CreateTargets: depth SRV creation failed.\n");
            return false;
        }

        D3D11_TEXTURE2D_DESC normalDesc{};
        normalDesc.Width = width;
        normalDesc.Height = height;
        normalDesc.MipLevels = 1;
        normalDesc.ArraySize = 1;
        // Same format RenderTargetPool gives the D3D12 GBuffer normal attachment, so the shared
        // encode/decode pair in SSAO_NormalDecode.hlsli applies unchanged.
        normalDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        normalDesc.SampleDesc.Count = 1;
        normalDesc.Usage = D3D11_USAGE_DEFAULT;
        normalDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&normalDesc, nullptr, m_normalTexture.GetAddressOf()))) {
            DebugLog("Dx11DepthNormalPrepass::CreateTargets: normal texture creation failed.\n");
            return false;
        }
        if (FAILED(device->CreateRenderTargetView(m_normalTexture.Get(), nullptr, m_normalRtv.GetAddressOf())) ||
            FAILED(device->CreateShaderResourceView(m_normalTexture.Get(), nullptr, m_normalSrv.GetAddressOf()))) {
            DebugLog("Dx11DepthNormalPrepass::CreateTargets: normal view creation failed.\n");
            return false;
        }

        m_width = width;
        m_height = height;
        return true;
    }

    void Dx11DepthNormalPrepass::Render(ID3D11DeviceContext* context,
                                        const Dx11PrepassDraw* draws,
                                        uint32_t drawCount,
                                        const float viewProjection[16])
    {
        if (context == nullptr || draws == nullptr || !IsValid()) {
            return;
        }

        ID3D11RenderTargetView* normalRtv = m_normalRtv.Get();
        context->OMSetRenderTargets(1, &normalRtv, m_dsv.Get());
        // Encoded (0,0,0): the decode turns a near-zero vector into a fixed up vector, so an
        // unwritten texel never produces a wild normal. Depth clears to the reverse-Z far plane,
        // which is also the "background, skip me" value the SSAO shaders test for.
        const float normalClear[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
        context->ClearRenderTargetView(normalRtv, normalClear);
        context->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);

        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = static_cast<float>(m_width);
        viewport.Height = static_cast<float>(m_height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);

        context->RSSetState(m_rasterizerState.Get());
        context->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
        const float blendFactor[4] = {};
        context->OMSetBlendState(m_blendState.Get(), blendFactor, 0xffffffffu);
        context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        context->CSSetShader(nullptr, nullptr, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (uint32_t i = 0; i < drawCount; ++i) {
            RenderDraw(context, draws[i], viewProjection);
        }

        // Leave nothing pointing at the depth target: it is sampled as a shader resource in the
        // very next pass, and D3D11 would silently unbind one of the two views if both stayed set.
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    void Dx11DepthNormalPrepass::RenderDraw(ID3D11DeviceContext* context,
                                            const Dx11PrepassDraw& draw,
                                            const float viewProjection[16])
    {
        if (draw.vertexBuffer == nullptr || draw.vertexStride == 0) {
            return;
        }

        const bool skinned = draw.boneMatrices != nullptr &&
                             m_skinnedVertexShader != nullptr &&
                             m_skinnedInputLayout != nullptr;

        context->VSSetShader(skinned ? m_skinnedVertexShader.Get() : m_staticVertexShader.Get(), nullptr, 0);
        context->IASetInputLayout(skinned ? m_skinnedInputLayout.Get() : m_staticInputLayout.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            Dx11DepthNormalConstants constants{};
            std::memcpy(constants.viewProjection, viewProjection, sizeof(constants.viewProjection));
            std::memcpy(constants.model, draw.model ? draw.model : kIdentityMatrix, sizeof(constants.model));
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(m_constantBuffer.Get(), 0);
        }
        ID3D11Buffer* constantBuffer = m_constantBuffer.Get();
        context->VSSetConstantBuffers(0, 1, &constantBuffer);

        if (skinned) {
            if (SUCCEEDED(context->Map(m_boneConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, draw.boneMatrices, sizeof(Dx11DepthNormalBoneConstants));
                context->Unmap(m_boneConstantBuffer.Get(), 0);
            }
            ID3D11Buffer* boneConstantBuffer = m_boneConstantBuffer.Get();
            context->VSSetConstantBuffers(1, 1, &boneConstantBuffer);
        }

        UINT stride = draw.vertexStride;
        UINT offset = 0;
        ID3D11Buffer* vertexBuffer = draw.vertexBuffer;
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);

        if (draw.indexBuffer != nullptr) {
            const DXGI_FORMAT indexFormat = draw.index32Bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
            context->IASetIndexBuffer(draw.indexBuffer, indexFormat, 0);
            context->DrawIndexed(draw.indexCount, 0, 0);
        } else {
            context->Draw(draw.vertexCount, 0);
        }
    }
}

#endif // RHI_DIRECTX11
