// Dx11GraphicsDeviceMesh.cpp
// The static native mesh path: shader/state setup, native shadow casting, and the opaque/
// transparent draw loop. Split out at the same granularity as the skinned, sky, and ray march
// paths, which already live in their own sibling files.
#include "Renderer/Backends/DirectX11/Dx11GraphicsDevice.h"
#include "Renderer/Backends/DirectX11/Dx11ConversionUtils.h"

#if RHI_DIRECTX11

#include "Foundation/Tools/DebugOutput.h"

#include <algorithm>
#include <cstring>

#include <d3dcompiler.h>

namespace SasamiEngine::Render
{
    namespace
    {

        // Kept in step with kNativeShadowMapResolution in RendererFrameGraph.cpp, which sizes
        // the projection this map is rendered with. The two must agree or the normal bias,
        // derived there from the ortho width and this resolution, is scaled wrong.
        constexpr uint32_t kNativeShadowMapResolution = 2048u;

    }

    bool Dx11GraphicsDevice::EnsureMeshFrameResources(UINT width, UINT height)
    {
        if (!m_device || width == 0 || height == 0) {
            return false;
        }

        if (!m_meshVertexShader || !m_meshPixelShader || !m_meshInputLayout) {
            Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
            Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
            // These used to be string literals in this file. The pixel shader now carries a real
            // Cook-Torrance BRDF and PCF shadow lookup, which is far too much HLSL to keep readable
            // as a C++ literal, so both live under Shaders/Native/Dx11/ like every other shader.
            if (!CompileDx11Shader(L"Native/Dx11/NativeMesh_VS.hlsl", "VSMain", "vs_5_0", vsBlob) ||
                !CompileDx11Shader(L"Native/Dx11/NativeMesh_PS.hlsl", "PSMain", "ps_5_0", psBlob)) {
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

        if (!m_shadowMap.IsValid()) {
            // Depth-only caster shaders. The skinned one is optional: without it skinned meshes
            // simply do not cast, which is a far better failure than losing the whole shadow map.
            Microsoft::WRL::ComPtr<ID3DBlob> shadowVsBlob;
            Microsoft::WRL::ComPtr<ID3DBlob> shadowSkinnedVsBlob;
            if (CompileDx11Shader(L"Native/Dx11/NativeShadowDepth_VS.hlsl", "VSMain", "vs_5_0", shadowVsBlob)) {
                const bool haveSkinned = CompileDx11Shader(L"Native/Dx11/NativeShadowDepthSkinned_VS.hlsl",
                                                           "VSMain",
                                                           "vs_5_0",
                                                           shadowSkinnedVsBlob);
                m_shadowMap.Initialize(m_device.Get(),
                                       kNativeShadowMapResolution,
                                       shadowVsBlob->GetBufferPointer(),
                                       shadowVsBlob->GetBufferSize(),
                                       haveSkinned ? shadowSkinnedVsBlob->GetBufferPointer() : nullptr,
                                       haveSkinned ? shadowSkinnedVsBlob->GetBufferSize() : 0u);
            }
            // A failed shadow map is not fatal either: the frame descriptor's shadowMapResolution
            // is forced to 0 below and the pixel shader falls back to unshadowed lighting.
            if (!m_shadowMap.IsValid()) {
                DebugLog("Dx11GraphicsDevice::EnsureMeshFrameResources: shadow map unavailable, "
                         "rendering without cast shadows.\n");
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

        if (!m_meshOcclusionDummySrv) {
            D3D11_TEXTURE2D_DESC dummyDesc{};
            dummyDesc.Width = 1;
            dummyDesc.Height = 1;
            dummyDesc.MipLevels = 1;
            dummyDesc.ArraySize = 1;
            dummyDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dummyDesc.SampleDesc.Count = 1;
            dummyDesc.Usage = D3D11_USAGE_IMMUTABLE;
            dummyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const uint32_t whitePixel = 0xFFFFFFFFu;
            D3D11_SUBRESOURCE_DATA dummyInitData{};
            dummyInitData.pSysMem = &whitePixel;
            dummyInitData.SysMemPitch = sizeof(whitePixel);
            Microsoft::WRL::ComPtr<ID3D11Texture2D> dummyTexture;
            if (FAILED(m_device->CreateTexture2D(&dummyDesc, &dummyInitData, dummyTexture.GetAddressOf())) ||
                FAILED(m_device->CreateShaderResourceView(dummyTexture.Get(), nullptr, m_meshOcclusionDummySrv.GetAddressOf()))) {
                DebugLog("Dx11GraphicsDevice::EnsureMeshFrameResources: occlusion dummy SRV creation failed.\n");
                return false;
            }
        }

        if (!m_meshAmbientCubeDummySrv) {
            D3D11_TEXTURE2D_DESC dummyCubeDesc{};
            dummyCubeDesc.Width = 1;
            dummyCubeDesc.Height = 1;
            dummyCubeDesc.MipLevels = 1;
            dummyCubeDesc.ArraySize = 6;
            dummyCubeDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dummyCubeDesc.SampleDesc.Count = 1;
            dummyCubeDesc.Usage = D3D11_USAGE_IMMUTABLE;
            dummyCubeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            dummyCubeDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
            const uint32_t whiteCubePixel = 0xFFFFFFFFu;
            D3D11_SUBRESOURCE_DATA dummyCubeInitData[6];
            for (uint32_t face = 0; face < 6u; ++face) {
                dummyCubeInitData[face].pSysMem = &whiteCubePixel;
                dummyCubeInitData[face].SysMemPitch = sizeof(whiteCubePixel);
                dummyCubeInitData[face].SysMemSlicePitch = 0;
            }
            Microsoft::WRL::ComPtr<ID3D11Texture2D> dummyCubeTexture;
            if (FAILED(m_device->CreateTexture2D(&dummyCubeDesc, dummyCubeInitData, dummyCubeTexture.GetAddressOf())) ||
                FAILED(m_device->CreateShaderResourceView(dummyCubeTexture.Get(), nullptr, m_meshAmbientCubeDummySrv.GetAddressOf()))) {
                DebugLog("Dx11GraphicsDevice::EnsureMeshFrameResources: ambient cube dummy SRV creation failed.\n");
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
            depth.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;  // Reverse-Z: near=1.0, far=0.0, so the nearest fragment wins with >=
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

        if (!m_meshOpaqueBlendState) {
            D3D11_BLEND_DESC blend{};
            blend.RenderTarget[0].BlendEnable = FALSE;
            blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(m_device->CreateBlendState(&blend, m_meshOpaqueBlendState.GetAddressOf()))) {
                return false;
            }
        }

        if (!m_meshTransparentDepthState) {
            D3D11_DEPTH_STENCIL_DESC depth{};
            depth.DepthEnable = TRUE;
            depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            depth.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
            if (FAILED(m_device->CreateDepthStencilState(&depth, m_meshTransparentDepthState.GetAddressOf()))) {
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

    void Dx11GraphicsDevice::CollectOpaqueDraws(const RhiBackendMeshFrameDesc& desc,
                                                std::vector<Dx11ShadowCaster>& out)
    {
        // Resolving handles is the device's job -- it owns m_rhiResources -- so the consuming
        // passes only ever see ready-made ID3D11Buffer pointers.
        const auto resolveBuffer = [this](uint64_t handle) -> ID3D11Buffer* {
            if (handle == 0) {
                return nullptr;
            }
            const auto it = m_rhiResources.find(handle);
            if (it == m_rhiResources.end()) {
                return nullptr;
            }
            Microsoft::WRL::ComPtr<ID3D11Buffer> buffer;
            if (FAILED(it->second.As(&buffer))) {
                return nullptr;
            }
            return buffer.Get();
        };

        out.clear();
        out.reserve(static_cast<size_t>(desc.drawCount) + desc.skinnedDrawCount);

        for (uint32_t i = 0; i < desc.drawCount; ++i) {
            const RhiBackendMeshDrawDesc& draw = desc.draws[i];
            // Transparent geometry is excluded: a cut-out or blended surface casting a solid
            // opaque shadow is worse than it casting none, and the depth-only pass has no alpha.
            if (draw.transparent || (draw.indexCount == 0 && draw.vertexCount == 0)) {
                continue;
            }
            ID3D11Buffer* vertexBuffer = resolveBuffer(draw.vertexBufferHandle);
            if (vertexBuffer == nullptr) {
                continue;
            }
            Dx11ShadowCaster caster{};
            caster.vertexBuffer = vertexBuffer;
            caster.indexBuffer = resolveBuffer(draw.indexBufferHandle);
            caster.vertexCount = draw.vertexCount;
            caster.indexCount = draw.indexCount;
            caster.vertexStride = draw.vertexStride;
            caster.index32Bit = draw.index32Bit;
            caster.model = draw.model;
            out.push_back(caster);
        }

        for (uint32_t i = 0; i < desc.skinnedDrawCount; ++i) {
            const RhiBackendSkinnedMeshDrawDesc& draw = desc.skinnedDraws[i];
            if (draw.transparent || draw.boneMatrices == nullptr ||
                (draw.indexCount == 0 && draw.vertexCount == 0)) {
                continue;
            }
            ID3D11Buffer* vertexBuffer = resolveBuffer(draw.vertexBufferHandle);
            if (vertexBuffer == nullptr) {
                continue;
            }
            Dx11ShadowCaster caster{};
            caster.vertexBuffer = vertexBuffer;
            caster.indexBuffer = resolveBuffer(draw.indexBufferHandle);
            caster.vertexCount = draw.vertexCount;
            caster.indexCount = draw.indexCount;
            caster.vertexStride = draw.vertexStride;
            caster.index32Bit = draw.index32Bit;
            caster.model = draw.model;
            caster.boneMatrices = draw.boneMatrices;
            out.push_back(caster);
        }

    }

    bool Dx11GraphicsDevice::RenderNativeShadowMap(const RhiBackendMeshFrameDesc& desc)
    {
        if (!desc.enableShadow || desc.shadowMapResolution == 0u || !m_shadowMap.IsValid()) {
            return false;
        }

        CollectOpaqueDraws(desc, m_resolvedOpaqueDraws);
        if (m_resolvedOpaqueDraws.empty()) {
            return false;
        }

        m_shadowMap.Render(m_context.Get(),
                           m_resolvedOpaqueDraws.data(),
                           static_cast<uint32_t>(m_resolvedOpaqueDraws.size()),
                           desc.lightViewProjection);
        return true;
    }

    bool Dx11GraphicsDevice::RenderNativeAmbientOcclusion(const RhiBackendMeshFrameDesc& desc)
    {
        // Every bail-out below leaves the frame without a runtime-AO texture, which the mesh
        // passes turn into runtimeAo = 1.0. Routed through one place so the transition is reported
        // once however it was reached.
        const auto skip = [this](const char* reason) {
            if (m_lastReportedSsaoState != 0) {
                m_lastReportedSsaoState = 0;
                char message[192];
                snprintf(message,
                         sizeof(message),
                         "Dx11GraphicsDevice::RenderNativeAmbientOcclusion: no runtime AO (%s).\n",
                         reason);
                DebugLog(message);
            }
            return false;
        };

        if (!m_context || !m_device || !desc.enableSsao) {
            return skip("disabled or no device");
        }

        const UINT width = static_cast<UINT>(desc.renderWidth);
        const UINT height = static_cast<UINT>(desc.renderHeight);
        if (width == 0u || height == 0u) {
            return skip("zero render size");
        }
        if (!m_aoPrepass.Ensure(m_device.Get(), width, height) ||
            !m_ssaoGenerator.Ensure(m_device.Get(), width, height)) {
            return skip("prepass or SSAO resources unavailable");
        }

        CollectOpaqueDraws(desc, m_resolvedOpaqueDraws);
        if (m_resolvedOpaqueDraws.empty()) {
            return skip("no opaque geometry");
        }

        m_aoPrepass.Render(m_context.Get(),
                           m_resolvedOpaqueDraws.data(),
                           static_cast<uint32_t>(m_resolvedOpaqueDraws.size()),
                           desc.viewProjection);
        Dx11SsaoGenerator::Tuning tuning{};
        tuning.radius = desc.runtimeAoRadius;
        tuning.bias = desc.runtimeAoBias;
        tuning.intensity = desc.runtimeAoIntensity;
        tuning.thickness = desc.runtimeAoThickness;
        tuning.quality = desc.runtimeAoQuality;
        m_ssaoGenerator.Render(m_context.Get(),
                               m_aoPrepass.DepthSrv(),
                               m_aoPrepass.NormalSrv(),
                               desc.viewProjection,
                               desc.invViewProjection,
                               desc.cameraPos,
                               width,
                               height,
                               tuning);

        if (m_lastReportedSsaoState != 1) {
            m_lastReportedSsaoState = 1;
            char message[160];
            snprintf(message,
                     sizeof(message),
                     "Dx11GraphicsDevice::RenderNativeAmbientOcclusion: runtime AO live, "
                     "%zu occluders at %ux%u.\n",
                     m_resolvedOpaqueDraws.size(),
                     width,
                     height);
            DebugLog(message);
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

        // D3D11 requires the depth view to match the render target's dimensions; when they differ
        // it drops the depth view instead of failing, and every draw then overwrites the last one
        // regardless of distance. The depth buffer here is sized from the frame description while
        // the render target is the swap chain, so the two are worth comparing out loud once.
        if (!m_reportedDepthTargetSizes) {
            m_reportedDepthTargetSizes = true;
            Microsoft::WRL::ComPtr<ID3D11Resource> rtResource;
            m_backBufferRtv->GetResource(rtResource.GetAddressOf());
            Microsoft::WRL::ComPtr<ID3D11Texture2D> rtTexture;
            D3D11_TEXTURE2D_DESC rtDesc{};
            if (rtResource && SUCCEEDED(rtResource.As(&rtTexture))) {
                rtTexture->GetDesc(&rtDesc);
            }
            char message[192];
            snprintf(message,
                     sizeof(message),
                     "Dx11GraphicsDevice::RenderMeshFrame: render target %ux%u, depth %ux%u, "
                     "frame desc %ux%u.\n",
                     rtDesc.Width,
                     rtDesc.Height,
                     m_backBufferDepthWidth,
                     m_backBufferDepthHeight,
                     width,
                     height);
            DebugLog(message);
        }

        ID3D11RenderTargetView* rtv = m_backBufferRtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, m_backBufferDsv.Get());
        m_context->ClearDepthStencilView(m_backBufferDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);

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

        ID3D11Buffer* cb = m_meshConstantBuffer.Get();
        m_context->VSSetConstantBuffers(0, 1, &cb);
        m_context->PSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState* sampler = m_meshSamplerState.Get();
        m_context->PSSetSamplers(0, 1, &sampler);
        ID3D11SamplerState* shadowSampler = m_shadowMap.ComparisonSampler();
        if (shadowSampler != nullptr) {
            m_context->PSSetSamplers(1, 1, &shadowSampler);
        }
        ID3D11ShaderResourceView* shadowSrv = m_shadowMapReadyThisFrame ? m_shadowMap.Srv() : nullptr;
        m_context->PSSetShaderResources(1, 1, &shadowSrv);

        // desc.ambientCubeSrv is a single frame-wide handle (Skybox owns one environment cube, not
        // one per draw), so it is resolved and bound once here rather than inside issueDraw below.
        // It stays 0/unresolved until Skybox::BuildNativeCubemap's async HDR load finishes. The
        // shader never samples the dummy -- featureFlags2.y gates it onto the analytic hemisphere
        // instead -- so the dummy exists only to keep slot 3 from being left unbound.
        const bool hasAmbientCube = desc.ambientCubeSrv != 0 && m_rhiSrvs.count(desc.ambientCubeSrv) != 0;
        ID3D11ShaderResourceView* ambientCubeSrv = hasAmbientCube
            ? m_rhiSrvs.at(desc.ambientCubeSrv).Get()
            : m_meshAmbientCubeDummySrv.Get();
        m_context->PSSetShaderResources(3, 1, &ambientCubeSrv);

        // Runtime (screen-space) ambient occlusion, frame-wide like the ambient cube above.
        // m_meshOcclusionDummySrv is the same 1x1 white texture the material-occlusion slot
        // falls back to: sampling it returns 1.0, so an absent AO texture leaves the slot bound
        // and the shader's runtimeAo factor a no-op.
        ID3D11ShaderResourceView* runtimeAoSrv = m_ssaoReadyThisFrame
            ? m_ssaoGenerator.Srv()
            : m_meshOcclusionDummySrv.Get();
        m_context->PSSetShaderResources(4, 1, &runtimeAoSrv);

        // Every `continue` below drops a mesh from the frame silently. The submitter already
        // reports what it never handed over; these counters cover the other half, so "geometry is
        // missing" can be answered with a number instead of a screenshot.
        uint32_t issuedDraws = 0;
        uint32_t skippedEmpty = 0;
        uint32_t skippedUnknownVertexBuffer = 0;
        uint32_t skippedUnknownIndexBuffer = 0;

        // Shared body for a single draw call; only the pass setup (blend/depth state) and the
        // order in which indices are fed to this differ between the opaque and transparent passes.
        const auto issueDraw = [&](uint32_t i) {
            const RhiBackendMeshDrawDesc& draw = desc.draws[i];
            if (draw.vertexBufferHandle == 0 || (draw.indexCount == 0 && draw.vertexCount == 0)) {
                ++skippedEmpty;
                return;
            }

            const auto vbIt = m_rhiResources.find(draw.vertexBufferHandle);
            if (vbIt == m_rhiResources.end()) {
                ++skippedUnknownVertexBuffer;
                return;
            }
            Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
            if (FAILED(vbIt->second.As(&vertexBuffer))) {
                ++skippedUnknownVertexBuffer;
                return;
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
            constants.exposureParams[0] = desc.exposure;   // RenderSettings::exposure
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
            // Not desc.enableSsao: the flag gates the runtimeAo sample, so it has to mean
            // "an AO texture was actually produced this frame", the same way shadowParams.w
            // means "a shadow map was actually rendered". Transparent draws opt out: the prepass
            // excludes them, so at their pixels the AO texture describes whatever opaque surface
            // is behind them -- applying it would darken glass by the occlusion of the wall it is
            // in front of. DX12 reaches the same place from the other direction: its runtimeAo
            // lives in the deferred lighting pass, which only ever shades opaque geometry.
            constants.featureFlags[2] = (m_ssaoReadyThisFrame && !draw.transparent) ? 1.0f : 0.0f;
            constants.featureFlags[3] = desc.enableShadow ? 1.0f : 0.0f;
            std::memcpy(constants.lightViewProjection,
                        desc.lightViewProjection,
                        sizeof(constants.lightViewProjection));
            constants.shadowParams[0] = desc.shadowDepthBias;
            constants.shadowParams[1] = desc.shadowNormalBias;
            constants.shadowParams[2] = desc.shadowTexelWorld;
            // Resolution doubles as the "is there a shadow map" switch in the shader, so a frame
            // whose shadow pass found no casters reads as unshadowed instead of sampling garbage.
            constants.shadowParams[3] = m_shadowMapReadyThisFrame
                ? static_cast<float>(desc.shadowMapResolution)
                : 0.0f;
            constants.featureFlags2[0] = desc.enablePostProcess ? 1.0f : 0.0f;
            constants.featureFlags2[1] = hasAmbientCube ? 1.0f : 0.0f;
            // Inverse render size, so the pixel shader can turn its SV_POSITION into the
            // screen UV the full-resolution AO texture is indexed by. Packed into the two
            // spare components rather than growing Dx11MeshConstants.
            constants.featureFlags2[2] = (desc.renderWidth > 0.0f) ? 1.0f / desc.renderWidth : 0.0f;
            constants.featureFlags2[3] = (desc.renderHeight > 0.0f) ? 1.0f / desc.renderHeight : 0.0f;

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(m_context->Map(m_meshConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) {
                return;
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

            // draw.occlusionSrv's texture slot may hold packed metallic-roughness instead of AO
            // (see RhiBackendMeshDrawDesc::occlusionSlotHasAo); only bind it as occlusion when the
            // flag confirms the R channel really is ambient occlusion, otherwise fall back to the
            // white dummy (materialAo=1.0, i.e. no change from before AO support existed). Mirrors
            // the Vulkan backend's occlusionSrv gate in VulkanGraphicsDevice.cpp.
            ID3D11ShaderResourceView* occlusionSrv = m_meshOcclusionDummySrv.Get();
            if (draw.occlusionSlotHasAo && draw.occlusionSrv != 0) {
                const auto occlusionSrvIt = m_rhiSrvs.find(draw.occlusionSrv);
                if (occlusionSrvIt != m_rhiSrvs.end()) {
                    occlusionSrv = occlusionSrvIt->second.Get();
                }
            }
            m_context->PSSetShaderResources(2, 1, &occlusionSrv);

            if (draw.indexBufferHandle != 0 && draw.indexCount > 0) {
                const auto ibIt = m_rhiResources.find(draw.indexBufferHandle);
                if (ibIt == m_rhiResources.end()) {
                    ++skippedUnknownIndexBuffer;
                    return;
                }
                Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
                if (FAILED(ibIt->second.As(&indexBuffer))) {
                    ++skippedUnknownIndexBuffer;
                    return;
                }
                m_context->IASetIndexBuffer(indexBuffer.Get(),
                                            draw.index32Bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT,
                                            0);
                m_context->DrawIndexed(draw.indexCount, 0, 0);
            } else {
                m_context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
                m_context->Draw(draw.vertexCount, 0);
            }
            ++issuedDraws;
        };

        m_meshOpaqueDrawOrder.clear();
        m_meshTransparentDrawOrder.clear();
        m_meshOpaqueDrawOrder.reserve(desc.drawCount);
        m_meshTransparentDrawOrder.reserve(desc.drawCount);
        for (uint32_t i = 0; i < desc.drawCount; ++i) {
            if (desc.draws[i].transparent) {
                m_meshTransparentDrawOrder.push_back(i);
            } else {
                m_meshOpaqueDrawOrder.push_back(i);
            }
        }

        // The mesh VS is row-vector (`mul(float4(pos, 1.0), u_model)` with a row_major u_model), so
        // the translation lives in row 3 of the flattened 16-float model matrix, i.e. indices
        // 12/13/14 - not the column-vector convention's indices 3/7/11.
        const auto distanceSqToCamera = [&](uint32_t i) {
            const float* model = desc.draws[i].model;
            const float dx = model[12] - desc.cameraPos[0];
            const float dy = model[13] - desc.cameraPos[1];
            const float dz = model[14] - desc.cameraPos[2];
            return dx * dx + dy * dy + dz * dz;
        };
        std::sort(m_meshTransparentDrawOrder.begin(),
                  m_meshTransparentDrawOrder.end(),
                  [&](uint32_t a, uint32_t b) { return distanceSqToCamera(a) > distanceSqToCamera(b); });

        const float blendFactor[4] = {};

        // Pass 1: opaque draws, in submission order, blend disabled, depth write on.
        m_context->OMSetBlendState(m_meshOpaqueBlendState.Get(), blendFactor, 0xffffffffu);
        m_context->OMSetDepthStencilState(m_meshDepthStencilState.Get(), 0);
        for (uint32_t index : m_meshOpaqueDrawOrder) {
            issueDraw(index);
        }

        // Pass 2: transparent draws, back-to-front, alpha blend on, depth write off (depth test
        // stays on so transparents still cull behind opaque geometry).
        m_context->OMSetBlendState(m_meshBlendState.Get(), blendFactor, 0xffffffffu);
        m_context->OMSetDepthStencilState(m_meshTransparentDepthState.Get(), 0);
        for (uint32_t index : m_meshTransparentDrawOrder) {
            issueDraw(index);
        }

        // The transparent bucket is tiny in practice, and whether a draw belongs in it at all is
        // decided by a single material bool with no alpha value behind it. Print what actually
        // landed there so a see-through wall can be traced to its material instead of guessed at.
        if (!m_meshTransparentDrawOrder.empty() && !m_reportedTransparentDraws) {
            m_reportedTransparentDraws = true;
            for (uint32_t index : m_meshTransparentDrawOrder) {
                const RhiBackendMeshDrawDesc& draw = desc.draws[index];
                char message[224];
                snprintf(message,
                         sizeof(message),
                         "Dx11GraphicsDevice::RenderMeshFrame: transparent draw %u: "
                         "baseColorAlpha=%.3f indexCount=%u albedoSrv=%llu.\n",
                         index,
                         draw.baseColor[3],
                         draw.indexCount,
                         static_cast<unsigned long long>(draw.albedoSrv));
                DebugLog(message);
            }
        }

        if (issuedDraws != m_lastReportedIssuedMeshDraws) {
            m_lastReportedIssuedMeshDraws = issuedDraws;
            char message[224];
            snprintf(message,
                     sizeof(message),
                     "Dx11GraphicsDevice::RenderMeshFrame: issued %u of %u draws "
                     "(%zu opaque, %zu transparent; %u empty, %u unknown vertex buffer, "
                     "%u unknown index buffer).\n",
                     issuedDraws,
                     desc.drawCount,
                     m_meshOpaqueDrawOrder.size(),
                     m_meshTransparentDrawOrder.size(),
                     skippedEmpty,
                     skippedUnknownVertexBuffer,
                     skippedUnknownIndexBuffer);
            DebugLog(message);
        }

        ID3D11ShaderResourceView* nullSrv = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSrv);
        // The shadow map is a depth-stencil target again next frame, and D3D11 refuses to bind a
        // resource that is still readable as an SRV, so release slot 1 here as well.
        m_context->PSSetShaderResources(1, 1, &nullSrv);
        // Slot 2 (occlusion) is bound to either a real material SRV or the dummy every draw; clear
        // it too so nothing from this pass lingers bound into whatever runs next.
        m_context->PSSetShaderResources(2, 1, &nullSrv);
        // Slot 3 (ambient cube) is bound once per frame above to either the Skybox cube or the
        // dummy; clear it for the same reason as slot 2.
        m_context->PSSetShaderResources(3, 1, &nullSrv);
        // Slot 4 (runtime AO) holds a render target the SSAO pass writes again next frame, so it
        // must not still be readable here -- the same reason slot 1 is released above.
        m_context->PSSetShaderResources(4, 1, &nullSrv);

        return true;
    }
} // namespace SasamiEngine::Render
#endif
