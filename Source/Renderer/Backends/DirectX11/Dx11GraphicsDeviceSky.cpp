// Dx11GraphicsDeviceSky.cpp
// The analytic sky drawn before the meshes. Shares ProceduralSky.hlsli with the other backends.
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
    bool Dx11GraphicsDevice::EnsureSkyResources()
    {
        if (m_skyVertexShader && m_skyPixelShader && m_skyConstantBuffer && m_skyEnvSampler) {
            return true;
        }
        if (m_skyResourcesFailed) {
            return false;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
        if (!CompileDx11Shader(L"Native/Dx11/NativeSky_VS.hlsl", "VSMain", "vs_5_0", vsBlob) ||
            !CompileDx11Shader(L"Native/Dx11/NativeSky_PS.hlsl", "PSMain", "ps_5_0", psBlob)) {
            m_skyResourcesFailed = true;
            return false;
        }
        if (FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                                nullptr, &m_skyVertexShader)) ||
            FAILED(m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                               nullptr, &m_skyPixelShader))) {
            m_skyResourcesFailed = true;
            return false;
        }

        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = sizeof(float) * (16 + 4 + 4 + 4 + 4);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_device->CreateBuffer(&cbDesc, nullptr, &m_skyConstantBuffer))) {
            m_skyResourcesFailed = true;
            return false;
        }

        // Clamped rather than wrapped: the cube is sampled by direction, and clamping keeps a
        // face's edge texels from bleeding in should the hardware ever fall back to per-face
        // addressing. Linear with no mip chain, matching how Skybox builds the cube (one mip).
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(m_device->CreateSamplerState(&samplerDesc, m_skyEnvSampler.GetAddressOf()))) {
            m_skyResourcesFailed = true;
            return false;
        }
        return true;
    }

    void Dx11GraphicsDevice::RenderNativeSky(const RhiBackendMeshFrameDesc& desc)
    {
        if (!EnsureSkyResources()) {
            return;
        }

        struct SkyCB
        {
            float invViewProjection[16];
            float cameraPos[4];
            float sunDirection[4];
            float sunColor[4];
            float exposureParams[4];
        } cb{};
        std::memcpy(cb.invViewProjection, desc.invViewProjection, sizeof(cb.invViewProjection));
        cb.cameraPos[0] = desc.cameraPos[0];
        cb.cameraPos[1] = desc.cameraPos[1];
        cb.cameraPos[2] = desc.cameraPos[2];
        cb.sunDirection[0] = desc.sunDir[0];
        cb.sunDirection[1] = desc.sunDir[1];
        cb.sunDirection[2] = desc.sunDir[2];
        cb.sunDirection[3] = desc.sunIntensity;
        cb.exposureParams[0] = desc.exposure;   // RenderSettings::exposure
        cb.sunColor[0] = desc.sunColor[0];
        cb.sunColor[1] = desc.sunColor[1];
        cb.sunColor[2] = desc.sunColor[2];
        // sunColor.w is otherwise unused, so it carries the "an HDR environment cube is bound" flag
        // rather than growing the constant buffer. The cube only exists once Skybox's asynchronous
        // HDR load completes, so this is false for the first frames of every run.
        const bool hasEnvironmentCube =
            desc.environmentCubeSrv != 0 && m_rhiSrvs.count(desc.environmentCubeSrv) != 0;
        cb.sunColor[3] = hasEnvironmentCube ? 1.0f : 0.0f;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_context->Map(m_skyConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return;
        }
        std::memcpy(mapped.pData, &cb, sizeof(cb));
        m_context->Unmap(m_skyConstantBuffer.Get(), 0);

        // No input layout, no vertex or index buffer: the vertex shader builds the triangle from
        // SV_VertexID. Depth is left unbound so the sky cannot be depth-tested against a buffer
        // the meshes have not filled yet, and cannot write depth that would then reject them.
        ID3D11RenderTargetView* rtv = m_backBufferRtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, nullptr);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_skyVertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(m_skyPixelShader.Get(), nullptr, 0);
        ID3D11Buffer* cbs[] = { m_skyConstantBuffer.Get() };
        m_context->PSSetConstantBuffers(0, 1, cbs);

        // Bound unconditionally so the slot is never left holding whatever the previous pass put
        // there; when no cube is available this is null and sunColor.w keeps the shader on the
        // procedural branch, which never reads the texture.
        ID3D11ShaderResourceView* envCubeSrv =
            hasEnvironmentCube ? m_rhiSrvs.at(desc.environmentCubeSrv).Get() : nullptr;
        ID3D11SamplerState* envSampler = m_skyEnvSampler.Get();
        m_context->PSSetShaderResources(0, 1, &envCubeSrv);
        m_context->PSSetSamplers(0, 1, &envSampler);

        m_context->Draw(3, 0);

        // Leave no dangling reference to the Skybox cube: the resource can be released and rebuilt
        // when the HDR source changes, and a stale binding here would outlive it.
        ID3D11ShaderResourceView* nullSrv = nullptr;
        m_context->PSSetShaderResources(0, 1, &nullSrv);
    }
} // namespace SasamiEngine::Render
#endif
