// Dx11GraphicsDeviceSkinnedMesh.cpp
// The skinned native mesh path: bone matrices, its own input layout and vertex shader.
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
    bool Dx11GraphicsDevice::EnsureSkinnedMeshFrameResources(UINT width, UINT height)
    {
        if (!EnsureMeshFrameResources(width, height)) {
            return false;
        }

        if (!m_skinnedMeshVertexShader || !m_skinnedMeshInputLayout) {
            Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
            if (!CompileDx11Shader(L"Native/Dx11/NativeMeshSkinned_VS.hlsl", "VSMain", "vs_5_0", vsBlob)) {
                return false;
            }

            HRESULT hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                                      vsBlob->GetBufferSize(),
                                                      nullptr,
                                                      m_skinnedMeshVertexShader.GetAddressOf());
            if (FAILED(hr)) {
                DebugLog("Dx11GraphicsDevice::EnsureSkinnedMeshFrameResources: CreateVertexShader failed.\n");
                return false;
            }

            D3D11_INPUT_ELEMENT_DESC inputElements[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                // HLSL splits the trailing digit off a semantic, so "JOINTS_0" in the shader is
                // semantic "JOINTS_" at index 0. Declaring the name with the digit attached made
                // CreateInputLayout reject the whole layout every frame. Matches the DX12 layout
                // in RenderPipelineStateCacheSkinned.cpp.
                { "JOINTS_",  0, DXGI_FORMAT_R8G8B8A8_UINT,      0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "WEIGHTS_", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            hr = m_device->CreateInputLayout(inputElements,
                                             _countof(inputElements),
                                             vsBlob->GetBufferPointer(),
                                             vsBlob->GetBufferSize(),
                                             m_skinnedMeshInputLayout.GetAddressOf());
            if (FAILED(hr)) {
                DebugLog("Dx11GraphicsDevice::EnsureSkinnedMeshFrameResources: CreateInputLayout failed.\n");
                return false;
            }
        }

        if (!m_skinnedMeshConstantBuffer) {
            D3D11_BUFFER_DESC cbDesc{};
            cbDesc.ByteWidth = static_cast<UINT>(sizeof(Dx11MeshConstants));
            cbDesc.Usage = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(m_device->CreateBuffer(&cbDesc, nullptr, m_skinnedMeshConstantBuffer.GetAddressOf()))) {
                DebugLog("Dx11GraphicsDevice::EnsureSkinnedMeshFrameResources: CreateBuffer failed.\n");
                return false;
            }
        }

        if (!m_skinnedMeshBoneConstantBuffer) {
            D3D11_BUFFER_DESC cbDesc{};
            cbDesc.ByteWidth = static_cast<UINT>(Skeleton::kMaxBones * 16 * sizeof(float));
            cbDesc.Usage = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(m_device->CreateBuffer(&cbDesc, nullptr, m_skinnedMeshBoneConstantBuffer.GetAddressOf()))) {
                DebugLog("Dx11GraphicsDevice::EnsureSkinnedMeshFrameResources: CreateBuffer failed.\n");
                return false;
            }
        }

        return true;
    }

    bool Dx11GraphicsDevice::RenderSkinnedMeshFrame(const RhiBackendMeshFrameDesc& desc)
    {
        if (!m_context || !m_backBufferRtv || !desc.skinnedDraws || desc.skinnedDrawCount == 0) {
            return false;
        }

        const UINT width = static_cast<UINT>(desc.renderWidth);
        const UINT height = static_cast<UINT>(desc.renderHeight);
        if (!EnsureSkinnedMeshFrameResources(width, height)) {
            return false;
        }

        ID3D11RenderTargetView* rtv = m_backBufferRtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, m_backBufferDsv.Get());
        // If the static-mesh pass already ran this frame it already cleared depth; avoid a second
        // clear here, which would erase the static draws' depth results before skinned draws test
        // against them.
        const bool staticPassRan = (desc.draws != nullptr && desc.drawCount > 0);
        if (!staticPassRan) {
            m_context->ClearDepthStencilView(m_backBufferDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        }

        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = desc.renderWidth;
        viewport.Height = desc.renderHeight;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &viewport);

        m_context->IASetInputLayout(m_skinnedMeshInputLayout.Get());
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->VSSetShader(m_skinnedMeshVertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(m_meshPixelShader.Get(), nullptr, 0);
        m_context->GSSetShader(nullptr, nullptr, 0);
        m_context->HSSetShader(nullptr, nullptr, 0);
        m_context->DSSetShader(nullptr, nullptr, 0);
        m_context->CSSetShader(nullptr, nullptr, 0);
        m_context->RSSetState(m_meshRasterizerState.Get());
        m_context->OMSetDepthStencilState(m_meshDepthStencilState.Get(), 0);
        const float blendFactor[4] = {};
        m_context->OMSetBlendState(m_meshBlendState.Get(), blendFactor, 0xffffffffu);

        ID3D11Buffer* boneCb = m_skinnedMeshBoneConstantBuffer.Get();
        m_context->VSSetConstantBuffers(1, 1, &boneCb);
        ID3D11SamplerState* sampler = m_meshSamplerState.Get();
        m_context->PSSetSamplers(0, 1, &sampler);
        ID3D11SamplerState* shadowSampler = m_shadowMap.ComparisonSampler();
        if (shadowSampler != nullptr) {
            m_context->PSSetSamplers(1, 1, &shadowSampler);
        }
        ID3D11ShaderResourceView* shadowSrv = m_shadowMapReadyThisFrame ? m_shadowMap.Srv() : nullptr;
        m_context->PSSetShaderResources(1, 1, &shadowSrv);

        // desc.ambientCubeSrv is a single frame-wide handle (Skybox owns one environment cube, not
        // one per draw), so it is resolved and bound once here rather than inside the draw loop
        // below. Mirrors the static mesh path's ambient cube gate in Dx11GraphicsDevice.cpp.
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

        for (uint32_t i = 0; i < desc.skinnedDrawCount; ++i) {
            const RhiBackendSkinnedMeshDrawDesc& draw = desc.skinnedDraws[i];
            if (draw.vertexBufferHandle == 0 || draw.boneMatrices == nullptr ||
                (draw.indexCount == 0 && draw.vertexCount == 0)) {
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
            if (FAILED(m_context->Map(m_skinnedMeshConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) {
                continue;
            }
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            m_context->Unmap(m_skinnedMeshConstantBuffer.Get(), 0);

            D3D11_MAPPED_SUBRESOURCE mappedBones{};
            if (FAILED(m_context->Map(m_skinnedMeshBoneConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedBones)) || !mappedBones.pData) {
                continue;
            }
            std::memcpy(mappedBones.pData, draw.boneMatrices, Skeleton::kMaxBones * 16 * sizeof(float));
            m_context->Unmap(m_skinnedMeshBoneConstantBuffer.Get(), 0);

            ID3D11Buffer* cb = m_skinnedMeshConstantBuffer.Get();
            m_context->VSSetConstantBuffers(0, 1, &cb);
            m_context->PSSetConstantBuffers(0, 1, &cb);

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

            // The skinned loader always fills this slot from glTF's dedicated occlusion texture,
            // so occlusionSlotHasAo only carries the asset's occlusionStrength (see
            // RhiBackendSkinnedMeshDrawDesc::occlusionSlotHasAo). When it is off, fall back to the
            // white dummy (materialAo=1.0, i.e. no change from before AO support existed).
            // Mirrors the static mesh path's occlusionSrv gate in Dx11GraphicsDevice.cpp.
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
