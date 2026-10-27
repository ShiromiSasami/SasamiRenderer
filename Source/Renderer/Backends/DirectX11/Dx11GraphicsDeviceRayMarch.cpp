// Dx11GraphicsDeviceRayMarch.cpp
// The ray-march background path: its own shaders, constant buffer and fullscreen draw.
#include "Renderer/Backends/DirectX11/Dx11GraphicsDevice.h"
#include "Renderer/Backends/DirectX11/Dx11ConversionUtils.h"

#if RHI_DIRECTX11

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Structures/Skeleton.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <vector>

#include <d3dcompiler.h>

namespace SasamiEngine::Render
{
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
        if (!CompileDx11Shader(L"Effects/RayMarch/RayMarch_VS.hlsl", "VSMain", "vs_5_0", vsBlob) ||
            !CompileDx11Shader(L"Effects/RayMarch/RayMarch_PS.hlsl", "PSMain", "ps_5_0", psBlob)) {
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
        if (FAILED(hr) || !mapped.pData) {
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
} // namespace SasamiEngine::Render
#endif
