#include "Renderer/Backends/DirectX11/Dx11ShadowMap.h"

#if RHI_DIRECTX11

#include "Foundation/Tools/DebugOutput.h"

#include <cstring>

namespace SasamiEngine::Render
{
    namespace
    {
        struct Dx11ShadowConstants
        {
            float lightViewProjection[16];
            float model[16];
        };
        static_assert((sizeof(Dx11ShadowConstants) % 16) == 0,
                      "D3D11 constant buffers must be 16-byte aligned.");

        struct Dx11ShadowBoneConstants
        {
            float boneMatrices[128 * 16];
        };
        static_assert((sizeof(Dx11ShadowBoneConstants) % 16) == 0,
                      "D3D11 constant buffers must be 16-byte aligned.");

        constexpr float kIdentityMatrix[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };
    }

    bool Dx11ShadowMap::Initialize(ID3D11Device* device,
                                   uint32_t resolution,
                                   const void* staticVsBytecode, size_t staticVsSize,
                                   const void* skinnedVsBytecode, size_t skinnedVsSize)
    {
        if (!device || resolution == 0) {
            DebugLog("Dx11ShadowMap::Initialize: invalid device or zero resolution.\n");
            return false;
        }
        if (!staticVsBytecode || staticVsSize == 0) {
            DebugLog("Dx11ShadowMap::Initialize: missing static depth-only vertex shader bytecode.\n");
            return false;
        }

        m_resolution = resolution;

        if (!CreateDepthResources(device, resolution)) {
            return false;
        }
        if (!CreateSampler(device)) {
            return false;
        }
        if (!CreateStaticPipeline(device, staticVsBytecode, staticVsSize)) {
            return false;
        }
        // Skinned casters are optional: a caller that never has skinned meshes can pass null here
        // and skinned casters are simply skipped at render time.
        if (skinnedVsBytecode && skinnedVsSize > 0) {
            if (!CreateSkinnedPipeline(device, skinnedVsBytecode, skinnedVsSize)) {
                return false;
            }
        }
        if (!CreateSharedStates(device)) {
            return false;
        }
        if (!CreateConstantBuffers(device)) {
            return false;
        }

        return true;
    }

    bool Dx11ShadowMap::CreateDepthResources(ID3D11Device* device, uint32_t resolution)
    {
        D3D11_TEXTURE2D_DESC textureDesc{};
        textureDesc.Width = resolution;
        textureDesc.Height = resolution;
        textureDesc.MipLevels = 1;
        textureDesc.ArraySize = 1;
        // Typeless so the same resource can back both the D32_FLOAT depth-stencil view and the
        // R32_FLOAT shader-resource view used to sample the map from the lighting pass.
        textureDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.SampleDesc.Quality = 0;
        textureDesc.Usage = D3D11_USAGE_DEFAULT;
        textureDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device->CreateTexture2D(&textureDesc, nullptr, m_depthTexture.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11ShadowMap::CreateDepthResources: CreateTexture2D failed.\n");
            return false;
        }

        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        hr = device->CreateDepthStencilView(m_depthTexture.Get(), &dsvDesc, m_dsv.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11ShadowMap::CreateDepthResources: CreateDepthStencilView failed.\n");
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;
        hr = device->CreateShaderResourceView(m_depthTexture.Get(), &srvDesc, m_srv.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11ShadowMap::CreateDepthResources: CreateShaderResourceView failed.\n");
            return false;
        }

        return true;
    }

    bool Dx11ShadowMap::CreateSampler(ID3D11Device* device)
    {
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        // Reverse-Z: 0 is the far plane, so an all-zero border reads as "no occluder" outside the
        // shadow map instead of "always occluded".
        samplerDesc.BorderColor[0] = 0.0f;
        samplerDesc.BorderColor[1] = 0.0f;
        samplerDesc.BorderColor[2] = 0.0f;
        samplerDesc.BorderColor[3] = 0.0f;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_GREATER_EQUAL;
        samplerDesc.MinLOD = 0.0f;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

        HRESULT hr = device->CreateSamplerState(&samplerDesc, m_sampler.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11ShadowMap::CreateSampler: CreateSamplerState failed.\n");
            return false;
        }
        return true;
    }

    bool Dx11ShadowMap::CreateStaticPipeline(ID3D11Device* device,
                                             const void* staticVsBytecode, size_t staticVsSize)
    {
        HRESULT hr = device->CreateVertexShader(staticVsBytecode,
                                                staticVsSize,
                                                nullptr,
                                                m_staticVertexShader.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11ShadowMap::CreateStaticPipeline: CreateVertexShader failed.\n");
            return false;
        }

        // The vertex buffer handed to Render() is the caller's ordinary mesh vertex buffer, bound
        // with its normal stride, so only POSITION needs to be declared here.
        D3D11_INPUT_ELEMENT_DESC inputElements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        hr = device->CreateInputLayout(inputElements,
                                       _countof(inputElements),
                                       staticVsBytecode,
                                       staticVsSize,
                                       m_staticInputLayout.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11ShadowMap::CreateStaticPipeline: CreateInputLayout failed.\n");
            return false;
        }
        return true;
    }

    bool Dx11ShadowMap::CreateSkinnedPipeline(ID3D11Device* device,
                                              const void* skinnedVsBytecode, size_t skinnedVsSize)
    {
        HRESULT hr = device->CreateVertexShader(skinnedVsBytecode,
                                                skinnedVsSize,
                                                nullptr,
                                                m_skinnedVertexShader.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11ShadowMap::CreateSkinnedPipeline: CreateVertexShader failed.\n");
            return false;
        }

        // Must match the skinned mesh vertex buffer layout in
        // Dx11GraphicsDevice::EnsureSkinnedMeshFrameResources exactly (semantic names, formats and
        // AlignedByteOffsets), since this class draws the caller's real skinned mesh vertex buffer
        // as-is: POSITION/NORMAL/COLOR/TEXCOORD occupy bytes 0-47, JOINTS_0 packs into 4 bytes at
        // offset 48, WEIGHTS_0 follows at offset 52. HLSL splits the trailing digit off a semantic,
        // so "JOINTS_0"/"WEIGHTS_0" in the shader are semantics "JOINTS_"/"WEIGHTS_" at index 0.
        D3D11_INPUT_ELEMENT_DESC inputElements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "JOINTS_",  0, DXGI_FORMAT_R8G8B8A8_UINT,      0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "WEIGHTS_", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        hr = device->CreateInputLayout(inputElements,
                                       _countof(inputElements),
                                       skinnedVsBytecode,
                                       skinnedVsSize,
                                       m_skinnedInputLayout.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11ShadowMap::CreateSkinnedPipeline: CreateInputLayout failed.\n");
            return false;
        }
        return true;
    }

    bool Dx11ShadowMap::CreateSharedStates(ID3D11Device* device)
    {
        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        // Matches the rest of the native raster path, which runs with back-face culling disabled.
        rasterDesc.CullMode = D3D11_CULL_NONE;
        rasterDesc.DepthClipEnable = TRUE;
        HRESULT hr = device->CreateRasterizerState(&rasterDesc, m_rasterizerState.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11ShadowMap::CreateSharedStates: CreateRasterizerState failed.\n");
            return false;
        }

        D3D11_DEPTH_STENCIL_DESC depthDesc{};
        depthDesc.DepthEnable = TRUE;
        depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        // Reverse-Z: nearer surfaces hold larger depth values, so a fragment passes when it is
        // greater than or equal to what is already stored.
        depthDesc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        hr = device->CreateDepthStencilState(&depthDesc, m_depthStencilState.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11ShadowMap::CreateSharedStates: CreateDepthStencilState failed.\n");
            return false;
        }
        return true;
    }

    bool Dx11ShadowMap::CreateConstantBuffers(ID3D11Device* device)
    {
        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = static_cast<UINT>(sizeof(Dx11ShadowConstants));
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = device->CreateBuffer(&cbDesc, nullptr, m_constantBuffer.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11ShadowMap::CreateConstantBuffers: CreateBuffer (b0) failed.\n");
            return false;
        }

        D3D11_BUFFER_DESC boneCbDesc{};
        boneCbDesc.ByteWidth = static_cast<UINT>(sizeof(Dx11ShadowBoneConstants));
        boneCbDesc.Usage = D3D11_USAGE_DYNAMIC;
        boneCbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        boneCbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&boneCbDesc, nullptr, m_boneConstantBuffer.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11ShadowMap::CreateConstantBuffers: CreateBuffer (b1) failed.\n");
            return false;
        }
        return true;
    }

    void Dx11ShadowMap::Render(ID3D11DeviceContext* context,
                               const Dx11ShadowCaster* casters,
                               uint32_t casterCount,
                               const float lightViewProjection[16])
    {
        if (!context || !IsValid()) {
            return;
        }

        context->OMSetRenderTargets(0, nullptr, m_dsv.Get());
        context->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);

        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = static_cast<float>(m_resolution);
        viewport.Height = static_cast<float>(m_resolution);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);

        context->RSSetState(m_rasterizerState.Get());
        context->OMSetDepthStencilState(m_depthStencilState.Get(), 0);
        // Depth-only pass: no pixel shader.
        context->PSSetShader(nullptr, nullptr, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (uint32_t i = 0; i < casterCount; ++i) {
            RenderCaster(context, casters[i], lightViewProjection);
        }
    }

    void Dx11ShadowMap::RenderCaster(ID3D11DeviceContext* context,
                                     const Dx11ShadowCaster& caster,
                                     const float lightViewProjection[16])
    {
        if (!caster.vertexBuffer || caster.vertexStride == 0) {
            return;
        }

        const bool skinned = caster.boneMatrices != nullptr &&
                             m_skinnedVertexShader != nullptr &&
                             m_skinnedInputLayout != nullptr;

        context->VSSetShader(skinned ? m_skinnedVertexShader.Get() : m_staticVertexShader.Get(), nullptr, 0);
        context->IASetInputLayout(skinned ? m_skinnedInputLayout.Get() : m_staticInputLayout.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            Dx11ShadowConstants constants{};
            std::memcpy(constants.lightViewProjection, lightViewProjection, sizeof(constants.lightViewProjection));
            std::memcpy(constants.model, caster.model ? caster.model : kIdentityMatrix, sizeof(constants.model));
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(m_constantBuffer.Get(), 0);
        }
        ID3D11Buffer* constantBuffer = m_constantBuffer.Get();
        context->VSSetConstantBuffers(0, 1, &constantBuffer);

        if (skinned) {
            if (SUCCEEDED(context->Map(m_boneConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, caster.boneMatrices, sizeof(Dx11ShadowBoneConstants));
                context->Unmap(m_boneConstantBuffer.Get(), 0);
            }
            ID3D11Buffer* boneConstantBuffer = m_boneConstantBuffer.Get();
            context->VSSetConstantBuffers(1, 1, &boneConstantBuffer);
        }

        UINT stride = caster.vertexStride;
        UINT offset = 0;
        ID3D11Buffer* vertexBuffer = caster.vertexBuffer;
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);

        if (caster.indexBuffer) {
            const DXGI_FORMAT indexFormat = caster.index32Bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
            context->IASetIndexBuffer(caster.indexBuffer, indexFormat, 0);
            context->DrawIndexed(caster.indexCount, 0, 0);
        } else {
            context->Draw(caster.vertexCount, 0);
        }
    }
}

#endif
