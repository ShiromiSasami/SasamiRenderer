// Dx11SsaoGenerator.cpp
// Drives the shared SSAO shaders from the DirectX 11 native path. Split out of the device for the
// same reason Dx11ShadowMap and Dx11DepthNormalPrepass are: it owns its own targets and pipeline.
#include "Renderer/Backends/DirectX11/Dx11SsaoGenerator.h"
#include "Renderer/Backends/DirectX11/Dx11ConversionUtils.h"

#if RHI_DIRECTX11

#include "Foundation/Tools/DebugOutput.h"

#include <cstring>

namespace SasamiEngine::Render
{
    namespace
    {
        // Field order must match SSAOCB in Shaders/Effects/AmbientOcclusion/SSAO/SSAO_PS.hlsl,
        // which is also what Renderer.cpp's PushCameraCB(PV, InvPV, extra0, extra1, extra2) lays
        // out for the D3D12 generator. BlurCB in SSAO_Blur_PS.hlsl is a prefix of this, so one
        // buffer serves both passes.
        struct Dx11SsaoConstants
        {
            float cameraPV[16];
            float cameraInvPV[16];
            float ssaoParams[4];   // x: radius, y: bias, z: intensity, w: thickness
            float screenSize[4];   // x: width, y: height, z: 1/width, w: 1/height
            float cameraPos[4];    // xyz: camera world position, w: quality index
        };
        static_assert((sizeof(Dx11SsaoConstants) % 16) == 0,
                      "D3D11 constant buffers must be 16-byte aligned.");
    }

    bool Dx11SsaoGenerator::Ensure(ID3D11Device* device, uint32_t width, uint32_t height)
    {
        if (device == nullptr || width == 0 || height == 0) {
            return false;
        }
        if (m_shaderCompileFailed) {
            return false;
        }
        if (!CreateShaders(device) || !CreateStates(device) || !CreateConstantBuffer(device)) {
            return false;
        }
        if (m_width != width || m_height != height || !IsValid()) {
            if (!CreateTargets(device, width, height)) {
                return false;
            }
        }
        return IsValid();
    }

    bool Dx11SsaoGenerator::CreateShaders(ID3D11Device* device)
    {
        if (m_fullscreenVertexShader && m_ssaoPixelShader && m_blurPixelShader) {
            return true;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> ssaoBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> blurBlob;
        // The D3D12 path's own SSAO sources, compiled for shader model 5.0. They declare nothing
        // D3D12-specific -- t0/t1/t2, s0, b0 map straight onto D3D11 slots -- so there is no
        // DX11 fork of the SSAO maths to keep in step.
        if (!CompileDx11Shader(L"Effects/AmbientOcclusion/SSAO/SSAO_VS.hlsl", "VSMain", "vs_5_0", vsBlob) ||
            !CompileDx11Shader(L"Effects/AmbientOcclusion/SSAO/SSAO_PS.hlsl", "PSMain", "ps_5_0", ssaoBlob) ||
            !CompileDx11Shader(L"Effects/AmbientOcclusion/SSAO/SSAO_Blur_PS.hlsl", "PSMain", "ps_5_0", blurBlob)) {
            DebugLog("Dx11SsaoGenerator::CreateShaders: SSAO shader compilation failed.\n");
            m_shaderCompileFailed = true;
            return false;
        }

        if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                              vsBlob->GetBufferSize(),
                                              nullptr,
                                              m_fullscreenVertexShader.GetAddressOf())) ||
            FAILED(device->CreatePixelShader(ssaoBlob->GetBufferPointer(),
                                             ssaoBlob->GetBufferSize(),
                                             nullptr,
                                             m_ssaoPixelShader.GetAddressOf())) ||
            FAILED(device->CreatePixelShader(blurBlob->GetBufferPointer(),
                                             blurBlob->GetBufferSize(),
                                             nullptr,
                                             m_blurPixelShader.GetAddressOf()))) {
            DebugLog("Dx11SsaoGenerator::CreateShaders: SSAO shader creation failed.\n");
            m_shaderCompileFailed = true;
            return false;
        }

        // The debug blit is optional: losing it costs an inspection aid, not the AO term.
        Microsoft::WRL::ComPtr<ID3DBlob> debugBlob;
        if (CompileDx11Shader(L"Native/Dx11/NativeAoDebug_PS.hlsl", "PSMain", "ps_5_0", debugBlob)) {
            if (FAILED(device->CreatePixelShader(debugBlob->GetBufferPointer(),
                                                 debugBlob->GetBufferSize(),
                                                 nullptr,
                                                 m_debugPixelShader.GetAddressOf()))) {
                m_debugPixelShader.Reset();
            }
        }

        return true;
    }

    bool Dx11SsaoGenerator::CreateStates(ID3D11Device* device)
    {
        if (!m_pointClampSampler) {
            // Matches the static point-clamp sampler the D3D12 SSAO root signature declares: the
            // shaders index neighbouring texels themselves and must not get filtered values.
            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(device->CreateSamplerState(&sampler, m_pointClampSampler.GetAddressOf()))) {
                DebugLog("Dx11SsaoGenerator::CreateStates: CreateSamplerState failed.\n");
                return false;
            }
        }

        if (!m_rasterizerState) {
            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode = D3D11_FILL_SOLID;
            raster.CullMode = D3D11_CULL_NONE;
            raster.DepthClipEnable = TRUE;
            if (FAILED(device->CreateRasterizerState(&raster, m_rasterizerState.GetAddressOf()))) {
                DebugLog("Dx11SsaoGenerator::CreateStates: CreateRasterizerState failed.\n");
                return false;
            }
        }

        if (!m_depthStencilState) {
            // Fullscreen passes: no depth test, no depth write, matching the D3D12 SSAO PSO.
            D3D11_DEPTH_STENCIL_DESC depth{};
            depth.DepthEnable = FALSE;
            depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
            depth.StencilEnable = FALSE;
            if (FAILED(device->CreateDepthStencilState(&depth, m_depthStencilState.GetAddressOf()))) {
                DebugLog("Dx11SsaoGenerator::CreateStates: CreateDepthStencilState failed.\n");
                return false;
            }
        }

        if (!m_blendState) {
            D3D11_BLEND_DESC blend{};
            blend.RenderTarget[0].BlendEnable = FALSE;
            blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(device->CreateBlendState(&blend, m_blendState.GetAddressOf()))) {
                DebugLog("Dx11SsaoGenerator::CreateStates: CreateBlendState failed.\n");
                return false;
            }
        }
        return true;
    }

    bool Dx11SsaoGenerator::CreateConstantBuffer(ID3D11Device* device)
    {
        if (m_constantBuffer) {
            return true;
        }
        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = static_cast<UINT>(sizeof(Dx11SsaoConstants));
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&cbDesc, nullptr, m_constantBuffer.GetAddressOf()))) {
            DebugLog("Dx11SsaoGenerator::CreateConstantBuffer: CreateBuffer failed.\n");
            return false;
        }
        return true;
    }

    bool Dx11SsaoGenerator::CreateTargets(ID3D11Device* device, uint32_t width, uint32_t height)
    {
        m_rawRtv.Reset();
        m_rawSrv.Reset();
        m_rawTexture.Reset();
        m_blurRtv.Reset();
        m_blurSrv.Reset();
        m_blurTexture.Reset();
        m_width = 0;
        m_height = 0;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        // R8G8B8A8_UNORM at full render resolution, the same format and size RenderTargetPool
        // gives the D3D12 SSAO and SSAO-blur targets.
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device->CreateTexture2D(&desc, nullptr, m_rawTexture.GetAddressOf())) ||
            FAILED(device->CreateRenderTargetView(m_rawTexture.Get(), nullptr, m_rawRtv.GetAddressOf())) ||
            FAILED(device->CreateShaderResourceView(m_rawTexture.Get(), nullptr, m_rawSrv.GetAddressOf()))) {
            DebugLog("Dx11SsaoGenerator::CreateTargets: raw SSAO target creation failed.\n");
            return false;
        }
        if (FAILED(device->CreateTexture2D(&desc, nullptr, m_blurTexture.GetAddressOf())) ||
            FAILED(device->CreateRenderTargetView(m_blurTexture.Get(), nullptr, m_blurRtv.GetAddressOf())) ||
            FAILED(device->CreateShaderResourceView(m_blurTexture.Get(), nullptr, m_blurSrv.GetAddressOf()))) {
            DebugLog("Dx11SsaoGenerator::CreateTargets: blurred SSAO target creation failed.\n");
            return false;
        }

        m_width = width;
        m_height = height;
        return true;
    }

    void Dx11SsaoGenerator::BeginFullscreenPass(ID3D11DeviceContext* context,
                                                ID3D11RenderTargetView* target,
                                                uint32_t width,
                                                uint32_t height)
    {
        context->OMSetRenderTargets(1, &target, nullptr);

        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);

        context->RSSetState(m_rasterizerState.Get());
        context->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
        const float blendFactor[4] = {};
        context->OMSetBlendState(m_blendState.Get(), blendFactor, 0xffffffffu);

        // SSAO_VS.hlsl builds its triangle from SV_VertexID, so there is no input layout, no
        // vertex buffer and no index buffer to bind.
        context->IASetInputLayout(nullptr);
        context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
        context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(m_fullscreenVertexShader.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        context->CSSetShader(nullptr, nullptr, 0);

        ID3D11SamplerState* sampler = m_pointClampSampler.Get();
        context->PSSetSamplers(0, 1, &sampler);
    }

    void Dx11SsaoGenerator::Render(ID3D11DeviceContext* context,
                                   ID3D11ShaderResourceView* depthSrv,
                                   ID3D11ShaderResourceView* normalSrv,
                                   const float viewProjection[16],
                                   const float invViewProjection[16],
                                   const float cameraPos[3],
                                   uint32_t width,
                                   uint32_t height,
                                   const Tuning& tuning)
    {
        if (context == nullptr || depthSrv == nullptr || normalSrv == nullptr || !IsValid()) {
            return;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            Dx11SsaoConstants constants{};
            std::memcpy(constants.cameraPV, viewProjection, sizeof(constants.cameraPV));
            std::memcpy(constants.cameraInvPV, invViewProjection, sizeof(constants.cameraInvPV));
            constants.ssaoParams[0] = tuning.radius;
            constants.ssaoParams[1] = tuning.bias;
            constants.ssaoParams[2] = tuning.intensity;
            constants.ssaoParams[3] = tuning.thickness;
            constants.screenSize[0] = static_cast<float>(width);
            constants.screenSize[1] = static_cast<float>(height);
            constants.screenSize[2] = (width > 0u) ? 1.0f / static_cast<float>(width) : 0.0f;
            constants.screenSize[3] = (height > 0u) ? 1.0f / static_cast<float>(height) : 0.0f;
            constants.cameraPos[0] = cameraPos[0];
            constants.cameraPos[1] = cameraPos[1];
            constants.cameraPos[2] = cameraPos[2];
            constants.cameraPos[3] = static_cast<float>(tuning.quality);
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(m_constantBuffer.Get(), 0);
        }

        // Pass 1: raw occlusion. Cleared to white because 1.0 means "unoccluded", so any texel the
        // shader skips reads back as fully lit -- the same clear the D3D12 pass issues.
        BeginFullscreenPass(context, m_rawRtv.Get(), m_width, m_height);
        const float unoccluded[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        context->ClearRenderTargetView(m_rawRtv.Get(), unoccluded);
        context->PSSetShader(m_ssaoPixelShader.Get(), nullptr, 0);
        ID3D11Buffer* constantBuffer = m_constantBuffer.Get();
        context->PSSetConstantBuffers(0, 1, &constantBuffer);
        ID3D11ShaderResourceView* ssaoInputs[2] = { depthSrv, normalSrv };
        context->PSSetShaderResources(0, 2, ssaoInputs);
        context->Draw(3, 0);

        // Pass 2: depth- and normal-aware blur. Binding the blur target first releases the raw
        // target, which this pass now reads.
        BeginFullscreenPass(context, m_blurRtv.Get(), m_width, m_height);
        context->PSSetShader(m_blurPixelShader.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &constantBuffer);
        ID3D11ShaderResourceView* blurInputs[3] = { m_rawSrv.Get(), depthSrv, normalSrv };
        context->PSSetShaderResources(0, 3, blurInputs);
        context->Draw(3, 0);

        // The prepass writes depth and normals again next frame, so leave nothing of this pass's
        // reads bound; likewise release the blur target, which the mesh pass samples.
        ID3D11ShaderResourceView* nullSrvs[3] = { nullptr, nullptr, nullptr };
        context->PSSetShaderResources(0, 3, nullSrvs);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    void Dx11SsaoGenerator::DrawDebugView(ID3D11DeviceContext* context,
                                          ID3D11RenderTargetView* target,
                                          uint32_t width,
                                          uint32_t height)
    {
        if (context == nullptr || target == nullptr || !m_debugPixelShader || !IsValid()) {
            return;
        }

        BeginFullscreenPass(context, target, width, height);
        context->PSSetShader(m_debugPixelShader.Get(), nullptr, 0);
        ID3D11ShaderResourceView* aoSrv = m_blurSrv.Get();
        context->PSSetShaderResources(0, 1, &aoSrv);
        context->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrv = nullptr;
        context->PSSetShaderResources(0, 1, &nullSrv);
    }
}

#endif // RHI_DIRECTX11
