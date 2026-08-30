// DxrRayTracerSetup.cpp
// DXR initialization, shader compilation, shader tables.

#include "Renderer/RayTracing/DxrRayTracer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <dxcapi.h>
#include <windows.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "Foundation/Math/MathUtil.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/RayTracing/DxrRayTracerUtility.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "d3dx12.h"

namespace SasamiRenderer
{
    using Microsoft::WRL::ComPtr;

    bool DxrRayTracer::Initialize(IRHIDevice& device, const DescriptorSet& descriptors)
    {
        m_descriptors = descriptors;
        m_supported = device.SupportsHardwareRayTracing();
        if (!m_supported) {
            return true;
        }

        if (!CompileShadersAndCreatePipeline(device)) {
            m_supported = false;
            return false;
        }
        if (!CreateShaderBindingTable(device)) {
            m_supported = false;
            return false;
        }
        if (!EnsureFrameConstantBuffer(device)) {
            m_supported = false;
            return false;
        }

        return true;
    }

    void DxrRayTracer::UpdateScene(IRHIDevice& device, const RayTracingScene& scene)
    {
        const bool geometryChanged = scene.geometryVersion != m_uploadedGeometryVersion;
        const bool materialChanged = scene.materialVersion != m_uploadedMaterialVersion;
        const bool instanceChanged = scene.instanceVersion != m_uploadedInstanceVersion;
        const bool sceneChanged = geometryChanged || materialChanged || instanceChanged;
        m_scene = scene;
        // UploadSceneBuffers rebuilds mesh records and owns the BLAS resource handles.
        // Keep TLAS/BLAS lifetime conservative: any material update may also change
        // transparent instance masks, so rebuild AS instead of leaving TLAS pointing at
        // mesh records recreated by the upload step.
        m_sceneDirty = sceneChanged;
        if (!m_supported) {
            return;
        }

        if (!sceneChanged) {
            return;
        }

        // The previous frame's Render() may still be executing on the GPU with the SRV
        // bound to the current TLAS/BLAS buffers; destroying them without waiting causes
        // "deleted prior to closing/executing the command list" GPU-based validation errors.
        device.WaitForGPU();

        // Destroy stale acceleration structures before rebuilding
        if (m_tlasHandle.id != 0) {
            device.DestroyRhiAccelerationStructure(m_tlasHandle);
            m_tlasHandle = {};
        }
        for (MeshRecord& mesh : m_meshRecords) {
            if (mesh.blasHandle.id != 0) {
                device.DestroyRhiAccelerationStructure(mesh.blasHandle);
                mesh.blasHandle = {};
            }
        }

        const auto buildStartTime = std::chrono::high_resolution_clock::now();
        if (!UploadSceneBuffers(device)) {
            DebugLog("DxrRayTracer::UpdateScene: failed to upload scene buffers.\n");
            return;
        }
        if (m_scene.instances.empty() || m_scene.meshes.empty()) {
            m_tlasHandle = {};
        } else if (!BuildAccelerationStructures(device)) {
            DebugLog("DxrRayTracer::UpdateScene: failed to build acceleration structures.\n");
            return;
        }
        const auto buildEndTime = std::chrono::high_resolution_clock::now();
        m_lastSceneBuildMs = std::chrono::duration<float, std::milli>(buildEndTime - buildStartTime).count();
        m_reportSceneBuildCost = true;
        m_uploadedGeometryVersion = scene.geometryVersion;
        m_uploadedMaterialVersion = scene.materialVersion;
        m_uploadedInstanceVersion = scene.instanceVersion;
        m_sceneDirty = false;
    }

    bool DxrRayTracer::EnsureFrameConstantBuffer(IRHIDevice& device)
    {
        if (m_frameConstantsBuffer.IsValid() && m_frameConstantsPtr) {
            return true;
        }

        return ResourceUploadUtility::CreateUploadBuffer(device,
                                                         (sizeof(FrameConstants) + 255u) & ~255u,
                                                         m_frameConstantsBuffer,
                                                         reinterpret_cast<void**>(&m_frameConstantsPtr));
    }

    void DxrRayTracer::FillFrameConstants(const RayTracingFrameDesc& frameDesc, FrameConstants& outConstants) const
    {
        std::memset(&outConstants, 0, sizeof(outConstants));
        outConstants.renderWidth = frameDesc.renderWidth;
        outConstants.renderHeight = frameDesc.renderHeight;
        outConstants.outputWidth = frameDesc.width;
        outConstants.outputHeight = frameDesc.height;
        outConstants.outputDescriptorIndex = m_descriptors.outputDescriptorIndex;
        outConstants.vertexDescriptorIndex = m_descriptors.vertexDescriptorIndex;
        outConstants.indexDescriptorIndex = m_descriptors.indexDescriptorIndex;
        outConstants.materialDescriptorIndex = m_descriptors.materialDescriptorIndex;
        outConstants.instanceDescriptorIndex = m_descriptors.instanceDescriptorIndex;
        outConstants.pointLightBudget = frameDesc.pointLightBudget;
        outConstants.spotLightBudget = frameDesc.spotLightBudget;
        outConstants.qualityTier = frameDesc.qualityTier;
        outConstants.debugView = frameDesc.debugView;
        outConstants.flags = frameDesc.flags;
        outConstants.maxBounceCount = std::clamp(frameDesc.maxBounceCount,
                                                 kMinRayTracingBounceCount,
                                                 kMaxRayTracingBounceCount);
        outConstants.dynamicResolutionScale = frameDesc.dynamicResolutionScale;
        outConstants.aoMicroShadowStrength = std::clamp(frameDesc.aoDirectLightingStrength, 0.0f, 1.0f);
        outConstants.cameraPosition[0] = frameDesc.cameraPosition[0];
        outConstants.cameraPosition[1] = frameDesc.cameraPosition[1];
        outConstants.cameraPosition[2] = frameDesc.cameraPosition[2];
        outConstants.cameraPosition[3] = 1.0f;
        std::memcpy(outConstants.inverseViewProjection,
                    frameDesc.inverseViewProjection,
                    sizeof(outConstants.inverseViewProjection));

        float forward[3] = {};
        Math::DirectionFromYawPitch(frameDesc.directionalLight.yaw,
                                    frameDesc.directionalLight.pitch,
                                    forward);
        outConstants.directionalLightDirection[0] = -forward[0];
        outConstants.directionalLightDirection[1] = -forward[1];
        outConstants.directionalLightDirection[2] = -forward[2];
        outConstants.directionalLightDirection[3] = 0.0f;

        outConstants.directionalLightColorIntensity[0] = frameDesc.directionalLight.color[0];
        outConstants.directionalLightColorIntensity[1] = frameDesc.directionalLight.color[1];
        outConstants.directionalLightColorIntensity[2] = frameDesc.directionalLight.color[2];
        outConstants.directionalLightColorIntensity[3] = frameDesc.directionalLight.intensity;
        outConstants.directionalLightMarkerParams[0] = frameDesc.directionalLightMarkerEnabled ? 1.0f : 0.0f;
        outConstants.directionalLightMarkerParams[1] = frameDesc.directionalLightMarkerAngularRadius;
        outConstants.directionalLightMarkerParams[2] = frameDesc.directionalLightMarkerHaloAngularRadius;
        outConstants.directionalLightMarkerParams[3] = frameDesc.directionalLightMarkerBrightness;

        if (frameDesc.pointLights) {
            outConstants.pointLightCount = std::min<uint32_t>(kMaxPointLights, static_cast<uint32_t>(frameDesc.pointLights->size()));
            for (uint32_t i = 0; i < outConstants.pointLightCount; ++i) {
                const RenderPointLight& light = (*frameDesc.pointLights)[i];
                outConstants.pointLights[i].posRange[0] = light.pos[0];
                outConstants.pointLights[i].posRange[1] = light.pos[1];
                outConstants.pointLights[i].posRange[2] = light.pos[2];
                outConstants.pointLights[i].posRange[3] = light.range;
                outConstants.pointLights[i].colorIntensity[0] = light.color[0];
                outConstants.pointLights[i].colorIntensity[1] = light.color[1];
                outConstants.pointLights[i].colorIntensity[2] = light.color[2];
                outConstants.pointLights[i].colorIntensity[3] = light.intensity;
            }
        }

        if (frameDesc.spotLights) {
            outConstants.spotLightCount = std::min<uint32_t>(kMaxSpotLights, static_cast<uint32_t>(frameDesc.spotLights->size()));
            for (uint32_t i = 0; i < outConstants.spotLightCount; ++i) {
                const RenderSpotLight& light = (*frameDesc.spotLights)[i];
                float direction[3] = {};
                Math::DirectionFromYawPitch(light.yaw, light.pitch, direction);
                outConstants.spotLights[i].posRange[0] = light.pos[0];
                outConstants.spotLights[i].posRange[1] = light.pos[1];
                outConstants.spotLights[i].posRange[2] = light.pos[2];
                outConstants.spotLights[i].posRange[3] = light.range;
                outConstants.spotLights[i].dirCosInner[0] = direction[0];
                outConstants.spotLights[i].dirCosInner[1] = direction[1];
                outConstants.spotLights[i].dirCosInner[2] = direction[2];
                outConstants.spotLights[i].dirCosInner[3] = std::cos(light.innerAngle);
                outConstants.spotLights[i].colorIntensity[0] = light.color[0];
                outConstants.spotLights[i].colorIntensity[1] = light.color[1];
                outConstants.spotLights[i].colorIntensity[2] = light.color[2];
                outConstants.spotLights[i].colorIntensity[3] = light.intensity;
                outConstants.spotLights[i].params[0] = std::cos(light.outerAngle);
            }
        }
    }

    bool DxrRayTracer::CompileShadersAndCreatePipeline(IRHIDevice& device)
    {
        if (!m_supported) {
            return false;
        }

        ComPtr<IDxcBlob> shaderLibrary;
        if (!DxrRayTracerUtility::CompileShaderLibrary(DxrRayTracerUtility::GetShaderSourceRoot() / L"RayTracing" / L"DXR" / L"RayTracing.hlsl", shaderLibrary)) {
            return false;
        }

        RhiRayTracingShaderGroupDesc shaderGroups[4] = {};
        // Group 0: RayGen (General)
        shaderGroups[0].type       = RhiRayTracingShaderGroupDesc::Type::General;
        shaderGroups[0].exportName = "RayGenShader";
        // Group 1: Miss (General)
        shaderGroups[1].type       = RhiRayTracingShaderGroupDesc::Type::General;
        shaderGroups[1].exportName = "MissShader";
        // Group 2: Shadow Miss (General)
        shaderGroups[2].type       = RhiRayTracingShaderGroupDesc::Type::General;
        shaderGroups[2].exportName = "ShadowMissShader";
        // Group 3: TrianglesHit group
        shaderGroups[3].type             = RhiRayTracingShaderGroupDesc::Type::TrianglesHit;
        shaderGroups[3].hitGroupExport   = "SceneHitGroup";
        shaderGroups[3].closestHitExport = "ClosestHitShader";

        RhiRayTracingPipelineDesc pipelineDesc{};
        pipelineDesc.libraryBytecode            = static_cast<const uint8_t*>(shaderLibrary->GetBufferPointer());
        pipelineDesc.libraryBytecodeSizeInBytes = shaderLibrary->GetBufferSize();
        pipelineDesc.shaderGroups               = shaderGroups;
        pipelineDesc.shaderGroupCount           = 4u;
        pipelineDesc.maxRecursionDepth          = kMaxRayTracingBounceCount + 1u;
        pipelineDesc.maxPayloadSizeBytes        = 32u;
        pipelineDesc.maxAttributeSizeBytes      = D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES;

        m_rtPipelineHandle = device.CreateRhiRayTracingPipeline(pipelineDesc);
        if (m_rtPipelineHandle.id == 0) {
            DebugLog("DxrRayTracer: failed to create ray tracing pipeline.\n");
            return false;
        }

        m_pipelineReady = true;
        return true;
    }

    bool DxrRayTracer::CreateShaderBindingTable(IRHIDevice& device)
    {
        if (!m_pipelineReady || m_rtPipelineHandle.id == 0) {
            return false;
        }

        const uint32_t missIndices[]     = { 1u, 2u };
        const uint32_t hitGroupIndices[] = { 3u };

        RhiShaderBindingTableDesc sbtDesc{};
        sbtDesc.pipeline         = m_rtPipelineHandle;
        sbtDesc.rayGenGroupIndex = 0u;
        sbtDesc.missGroupIndices = missIndices;
        sbtDesc.missGroupCount   = 2u;
        sbtDesc.hitGroupIndices  = hitGroupIndices;
        sbtDesc.hitGroupCount    = 1u;

        m_sbtHandle = device.CreateRhiShaderBindingTable(sbtDesc);
        if (m_sbtHandle.id == 0) {
            DebugLog("DxrRayTracer: failed to create shader binding table.\n");
            return false;
        }

        return true;
    }


} // namespace SasamiRenderer
