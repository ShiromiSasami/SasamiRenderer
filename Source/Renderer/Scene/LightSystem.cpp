#include "Renderer/Scene/LightSystem.h"

#include <cmath>
#include <cstring>

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Scene/LightCBData.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "d3dx12.h"

namespace
{
    struct PointLightGPU
    {
        float posRange[4];
        float colorIntensity[4];
    };

    struct SpotLightGPU
    {
        float posRange[4];
        float dirCosInner[4];
        float colorIntensity[4];
        float params[4]; // x: cosOuter
    };

}

namespace SasamiRenderer
{
    bool LightSystem::Initialize(IRHIDevice& device, const AllocateSrvRangeCallback& allocateSrvRange)
    {
        ScopedPerfTimer perfTimer("LightSystem::Initialize");
        m_device = &device;

        CpuDescriptorHandle shadowCpu{};
        GpuDescriptorHandle shadowGpu{};
        if (!allocateSrvRange || !allocateSrvRange(1, shadowCpu, shadowGpu)) {
            DebugLogDialog("LightSystem::Initialize: SRV allocation failed for shadow map.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }
        m_shadowSrvCpu = shadowCpu;
        m_shadowSrv = shadowGpu;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        Resource nullResource;
        m_device->CreateShaderResourceView(nullResource, &srvDesc, shadowCpu);

        m_shadowViewport = { 0.0f, 0.0f, static_cast<float>(m_shadowMapSize), static_cast<float>(m_shadowMapSize), 0.0f, 1.0f };
        m_shadowScissor = { 0, 0, static_cast<LONG>(m_shadowMapSize), static_cast<LONG>(m_shadowMapSize) };
        return true;
    }

    bool LightSystem::InitializeFrameResources(FrameResources& frame, const AllocateSrvRangeCallback& allocateSrvRange)
    {
        if (!m_device || !allocateSrvRange) {
            return false;
        }

        const UINT lightCbSize = (sizeof(LightCBLayout::LightCBData) + 255u) & ~255u;
        if (!ResourceUploadUtility::CreateUploadBuffer(*m_device, lightCbSize, frame.lightCB, &frame.lightCBPtr)) {
            DebugLogDialog("LightSystem::InitializeFrameResources: Light CB creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        CpuDescriptorHandle lightSrvCpu{};
        GpuDescriptorHandle lightSrvGpu{};
        if (!allocateSrvRange(2, lightSrvCpu, lightSrvGpu)) {
            DebugLogDialog("LightSystem::InitializeFrameResources: SRV allocation failed for light buffers.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        frame.pointSrvCpu = lightSrvCpu;
        frame.spotSrvCpu = { lightSrvCpu.ptr + static_cast<SIZE_T>(inc) };
        frame.lightSrvTable = lightSrvGpu;
        return true;
    }

    bool LightSystem::EnsureShadowResources()
    {
        if (!m_device) {
            return false;
        }

        if (m_shadowMap.IsValid() && m_dsvHeapShadow.Get()) {
            return true;
        }

        ScopedPerfTimer perfTimer("LightSystem::EnsureShadowResources");

        D3D12_RESOURCE_DESC smDesc = {};
        smDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        smDesc.Width = m_shadowMapSize;
        smDesc.Height = m_shadowMapSize;
        smDesc.DepthOrArraySize = 1;
        smDesc.MipLevels = 1;
        smDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        smDesc.SampleDesc.Count = 1;
        smDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        smDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f;
        clear.DepthStencil.Stencil = 0;

        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = m_device->CreateCommittedResource(&heap,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &smDesc,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                       &clear,
                                                       m_shadowMap);
        if (FAILED(hr)) {
            DebugLogDialog("LightSystem::EnsureShadowResources: Shadow map resource creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
        dsvDesc.NumDescriptors = 1;
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = m_device->CreateDescriptorHeap(dsvDesc, m_dsvHeapShadow);
        if (FAILED(hr)) {
            m_shadowMap.Reset();
            DebugLogDialog("LightSystem::EnsureShadowResources: Shadow DSV heap creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsv.Flags = D3D12_DSV_FLAG_NONE;
        m_device->CreateDepthStencilView(m_shadowMap, &dsv, m_dsvHeapShadow->GetCPUDescriptorHandleForHeapStart());

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_shadowMap, &srvDesc, m_shadowSrvCpu);

        return true;
    }

    void LightSystem::ShutdownFrameResources(FrameResources& frame)
    {
        if (frame.lightCB.IsValid() && frame.lightCBPtr) {
            frame.lightCB->Unmap(0, nullptr);
            frame.lightCBPtr = nullptr;
        }

        if (frame.pointLightBuffer.IsValid() && frame.pointLightBufferPtr) {
            frame.pointLightBuffer->Unmap(0, nullptr);
            frame.pointLightBufferPtr = nullptr;
        }

        if (frame.spotLightBuffer.IsValid() && frame.spotLightBufferPtr) {
            frame.spotLightBuffer->Unmap(0, nullptr);
            frame.spotLightBufferPtr = nullptr;
        }

        frame.lightCB.Reset();
        frame.pointLightBuffer.Reset();
        frame.spotLightBuffer.Reset();
        frame.pointLightCapacity = 0;
        frame.spotLightCapacity = 0;
        frame.pointSrvCpu = {};
        frame.spotSrvCpu = {};
        frame.lightSrvTable = {};
    }

    void LightSystem::EnsureLightBuffers(FrameResources& frame, size_t pointCount, size_t spotCount)
    {
        if (!m_device) {
            return;
        }

        auto growCapacity = [](UINT current, UINT required) -> UINT {
            UINT cap = (current > 0) ? current : 1u;
            while (cap < required) {
                cap *= 2u;
            }
            return cap;
        };

        const UINT requiredPoint = (pointCount > 0) ? static_cast<UINT>(pointCount) : 1u;
        const UINT requiredSpot = (spotCount > 0) ? static_cast<UINT>(spotCount) : 1u;

        if (requiredPoint > frame.pointLightCapacity) {
            frame.pointLightCapacity = growCapacity(frame.pointLightCapacity, requiredPoint);
            if (frame.pointLightBuffer.IsValid() && frame.pointLightBufferPtr) {
                frame.pointLightBuffer->Unmap(0, nullptr);
            }
            frame.pointLightBuffer.Reset();
            frame.pointLightBufferPtr = nullptr;

            const UINT64 byteSize = static_cast<UINT64>(frame.pointLightCapacity) * sizeof(PointLightGPU);
            if (!ResourceUploadUtility::CreateUploadBuffer(*m_device, byteSize, frame.pointLightBuffer, &frame.pointLightBufferPtr)) {
                frame.pointLightCapacity = 0;
            }
        }

        if (requiredSpot > frame.spotLightCapacity) {
            frame.spotLightCapacity = growCapacity(frame.spotLightCapacity, requiredSpot);
            if (frame.spotLightBuffer.IsValid() && frame.spotLightBufferPtr) {
                frame.spotLightBuffer->Unmap(0, nullptr);
            }
            frame.spotLightBuffer.Reset();
            frame.spotLightBufferPtr = nullptr;

            const UINT64 byteSize = static_cast<UINT64>(frame.spotLightCapacity) * sizeof(SpotLightGPU);
            if (!ResourceUploadUtility::CreateUploadBuffer(*m_device, byteSize, frame.spotLightBuffer, &frame.spotLightBufferPtr)) {
                frame.spotLightCapacity = 0;
            }
        }

        if (frame.pointLightBuffer.IsValid()) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = frame.pointLightCapacity;
            srvDesc.Buffer.StructureByteStride = sizeof(PointLightGPU);
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            m_device->CreateShaderResourceView(frame.pointLightBuffer, &srvDesc, frame.pointSrvCpu);
        }

        if (frame.spotLightBuffer.IsValid()) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = frame.spotLightCapacity;
            srvDesc.Buffer.StructureByteStride = sizeof(SpotLightGPU);
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            m_device->CreateShaderResourceView(frame.spotLightBuffer, &srvDesc, frame.spotSrvCpu);
        }
    }

    void LightSystem::ExecuteShadowPass(CommandList* cmdList,
                                        FrameResources& frame,
                                        RenderPipelineStateCache& pipelineStateCache,
                                        DescriptorHeap& srvHeap,
                                        const float cameraPos[3],
                                        bool useTessellationPath,
                                        bool iblEnabled,
                                        float iblIntensity,
                                        float iblPrefilterMaxMip,
                                        bool hasDiffuseSh,
                                        const float (*diffuseShCoefficients)[3],
                                        RendererEnums::GBufferDebugView debugView,
                                        const DrawShadowCallback& drawCallback)
    {
        if (!cmdList || !frame.lightCB.IsValid()) {
            return;
        }

        if (!EnsureShadowResources()) {
            return;
        }

        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMap.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmdList->ResourceBarrier(1, &barrier);

        cmdList->SetGraphicsRootSignature(pipelineStateCache.GetRootSignature());
        if (useTessellationPath) {
            cmdList->SetPipelineState(pipelineStateCache.GetTessellationShadowPipelineState());
        } else {
            cmdList->SetPipelineState(pipelineStateCache.GetShadowPipelineState());
        }

        DescriptorHeap* heaps[] = { &srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootDescriptorTable(1, m_shadowSrv);
        cmdList->SetGraphicsRootDescriptorTable(4, frame.lightSrvTable);
        cmdList->RSSetViewports(1, &m_shadowViewport);
        cmdList->RSSetScissorRects(1, &m_shadowScissor);
        cmdList->IASetPrimitiveTopology(useTessellationPath
            ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
            : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        auto dsv = m_dsvHeapShadow->GetCPUDescriptorHandleForHeapStart();
        cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        float lightVP[16];
        float lightForward[3] = {};
        Math::BuildDirectionalLightViewProjection(m_lightYaw,
                                                  m_lightPitch,
                                                  m_lightDistance,
                                                  m_lightOrthoHalf,
                                                  m_lightNear,
                                                  m_lightFar,
                                                  lightVP,
                                                  lightForward);

        EnsureLightBuffers(frame, m_pointLights.size(), m_spotLights.size());

        if (frame.lightCBPtr) {
            size_t pointCount = m_pointLights.size();
            size_t spotCount = m_spotLights.size();

            if (!frame.pointLightBufferPtr) {
                pointCount = 0;
            } else {
                auto* pointDst = reinterpret_cast<PointLightGPU*>(frame.pointLightBufferPtr);
                for (size_t i = 0; i < pointCount; ++i) {
                    const auto& pl = m_pointLights[i];
                    pointDst[i].posRange[0] = pl.pos[0];
                    pointDst[i].posRange[1] = pl.pos[1];
                    pointDst[i].posRange[2] = pl.pos[2];
                    pointDst[i].posRange[3] = pl.range;
                    pointDst[i].colorIntensity[0] = pl.color[0];
                    pointDst[i].colorIntensity[1] = pl.color[1];
                    pointDst[i].colorIntensity[2] = pl.color[2];
                    pointDst[i].colorIntensity[3] = pl.intensity;
                }
            }

            if (!frame.spotLightBufferPtr) {
                spotCount = 0;
            } else {
                auto* spotDst = reinterpret_cast<SpotLightGPU*>(frame.spotLightBufferPtr);
                for (size_t i = 0; i < spotCount; ++i) {
                    const auto& sl = m_spotLights[i];
                    float spotDir[3] = {};
                    Math::DirectionFromYawPitch(sl.yaw, sl.pitch, spotDir);

                    float inner = sl.innerAngle;
                    float outer = sl.outerAngle;
                    if (inner > outer) {
                        inner = outer;
                    }
                    const float cosInner = std::cos(inner);
                    const float cosOuter = std::cos(outer);

                    spotDst[i].posRange[0] = sl.pos[0];
                    spotDst[i].posRange[1] = sl.pos[1];
                    spotDst[i].posRange[2] = sl.pos[2];
                    spotDst[i].posRange[3] = sl.range;
                    spotDst[i].dirCosInner[0] = spotDir[0];
                    spotDst[i].dirCosInner[1] = spotDir[1];
                    spotDst[i].dirCosInner[2] = spotDir[2];
                    spotDst[i].dirCosInner[3] = cosInner;
                    spotDst[i].colorIntensity[0] = sl.color[0];
                    spotDst[i].colorIntensity[1] = sl.color[1];
                    spotDst[i].colorIntensity[2] = sl.color[2];
                    spotDst[i].colorIntensity[3] = sl.intensity;
                    spotDst[i].params[0] = cosOuter;
                    spotDst[i].params[1] = 0.0f;
                    spotDst[i].params[2] = 0.0f;
                    spotDst[i].params[3] = 0.0f;
                }
            }

            LightCBLayout::LightCBData cb = {};
            std::memcpy(cb.lightVP, lightVP, sizeof(lightVP));
            cb.dirDir[0] = lightForward[0];
            cb.dirDir[1] = lightForward[1];
            cb.dirDir[2] = lightForward[2];
            cb.dirDir[3] = m_dirIntensity;
            cb.dirColor[0] = m_dirColor[0];
            cb.dirColor[1] = m_dirColor[1];
            cb.dirColor[2] = m_dirColor[2];
            cb.dirColor[3] = 1.0f;
            cb.lightCounts[0] = static_cast<float>(pointCount);
            cb.lightCounts[1] = static_cast<float>(spotCount);
            cb.cameraPos[0] = cameraPos[0];
            cb.cameraPos[1] = cameraPos[1];
            cb.cameraPos[2] = cameraPos[2];
            cb.cameraPos[3] = 1.0f;
            cb.iblParams[0] = iblEnabled ? 1.0f : 0.0f;
            cb.iblParams[1] = iblIntensity;
            cb.iblParams[2] = iblPrefilterMaxMip;
            cb.iblParams[3] = (hasDiffuseSh && diffuseShCoefficients != nullptr) ? 1.0f : 0.0f;
            cb.debugParams[0] = static_cast<float>(static_cast<int>(debugView));
            cb.debugParams[1] = 0.0f;
            cb.debugParams[2] = 0.0f;
            cb.debugParams[3] = 0.0f;
            for (size_t i = 0; i < LightCBLayout::kDiffuseShCoefficientCount; ++i) {
                cb.diffuseSh[i][0] =
                    (hasDiffuseSh && diffuseShCoefficients != nullptr) ? diffuseShCoefficients[i][0] : 0.0f;
                cb.diffuseSh[i][1] =
                    (hasDiffuseSh && diffuseShCoefficients != nullptr) ? diffuseShCoefficients[i][1] : 0.0f;
                cb.diffuseSh[i][2] =
                    (hasDiffuseSh && diffuseShCoefficients != nullptr) ? diffuseShCoefficients[i][2] : 0.0f;
                cb.diffuseSh[i][3] = 0.0f;
            }
            std::memcpy(frame.lightCBPtr, &cb, sizeof(cb));
        }

        cmdList->SetGraphicsRootConstantBufferView(3, frame.lightCB->GetGPUVirtualAddress());

        if (drawCallback) {
            ShadowPassContext context{};
            std::memcpy(context.lightViewProjection, lightVP, sizeof(lightVP));
            drawCallback(context);
        }

        barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMap.Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
    }

    LightSystem::DirectionalLightSettings LightSystem::GetDirectionalLightSettings() const
    {
        DirectionalLightSettings settings{};
        settings.yaw = m_lightYaw;
        settings.pitch = m_lightPitch;
        settings.distance = m_lightDistance;
        settings.orthoHalf = m_lightOrthoHalf;
        settings.nearZ = m_lightNear;
        settings.farZ = m_lightFar;
        settings.color[0] = m_dirColor[0];
        settings.color[1] = m_dirColor[1];
        settings.color[2] = m_dirColor[2];
        settings.intensity = m_dirIntensity;
        return settings;
    }

    void LightSystem::SetDirectionalLightSettings(const DirectionalLightSettings& settings)
    {
        m_lightYaw = settings.yaw;
        m_lightPitch = settings.pitch;
        m_lightDistance = settings.distance;
        m_lightOrthoHalf = settings.orthoHalf;
        m_lightNear = settings.nearZ;
        m_lightFar = settings.farZ;
        m_dirColor[0] = settings.color[0];
        m_dirColor[1] = settings.color[1];
        m_dirColor[2] = settings.color[2];
        m_dirIntensity = settings.intensity;
    }
}
