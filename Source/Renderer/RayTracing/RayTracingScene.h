#pragma once

#include "Renderer/Scene/RenderLightProxy.h"
#include "Renderer/Scene/RenderProxy.h"
#include "Renderer/Structures/Mesh.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace SasamiRenderer
{
    static constexpr uint32_t kRayTracingFrameFlagDirectionalShadow = 1u << 0;
    static constexpr uint32_t kMinRayTracingBounceCount = 1u;
    static constexpr uint32_t kDefaultRayTracingBounceCount = 2u;
    static constexpr uint32_t kMaxRayTracingBounceCount = 8u;
    static constexpr uint32_t kMinRayTracingSamplesPerPixel = 1u;
    static constexpr uint32_t kDefaultRayTracingSamplesPerPixel = 1u;
    static constexpr uint32_t kMaxRayTracingSamplesPerPixel = 64u;

    struct RayTracingMaterial
    {
        std::shared_ptr<const CpuTextureRgba8> albedoTexture;
        std::shared_ptr<const CpuTextureRgba8> occlusionTexture;
        SurfaceMaterial material;
        int32_t albedoDescriptorIndex = -1;
        int32_t occlusionDescriptorIndex = -1;
    };

    struct RayTracingMesh
    {
        Mesh mesh;
        float localBoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float localBoundsMax[3] = { 0.0f, 0.0f, 0.0f };
    };

    struct RayTracingInstance
    {
        uint32_t meshIndex = 0;
        uint32_t materialIndex = 0;
        bool transparent = false;
        float model[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
        float inverseModel[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
        float worldBoundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float worldBoundsMax[3] = { 0.0f, 0.0f, 0.0f };
    };

    struct RayTracingScene
    {
        std::vector<RayTracingMesh> meshes;
        std::vector<RayTracingMaterial> materials;
        std::vector<RayTracingInstance> instances;
        uint64_t geometryVersion = 0;
        uint64_t materialVersion = 0;
        uint64_t instanceVersion = 0;
        uint64_t geometryHash = 0;
        uint64_t materialHash = 0;
        uint64_t instanceHash = 0;

        void Clear()
        {
            meshes.clear();
            materials.clear();
            instances.clear();
            geometryHash = 0;
            materialHash = 0;
            instanceHash = 0;
            ++geometryVersion;
            ++materialVersion;
            ++instanceVersion;
        }

        uint32_t TriangleCount() const
        {
            uint32_t triangleCount = 0;
            for (const RayTracingInstance& instance : instances) {
                if (instance.meshIndex >= meshes.size()) {
                    continue;
                }
                const RayTracingMesh& mesh = meshes[instance.meshIndex];
                triangleCount += static_cast<uint32_t>(mesh.mesh.indices.empty()
                    ? (mesh.mesh.vertices.size() / 3u)
                    : (mesh.mesh.indices.size() / 3u));
            }
            return triangleCount;
        }
    };

    struct RayTracingFrameDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
        uint32_t pointLightBudget = 0;
        uint32_t spotLightBudget = 0;
        uint32_t qualityTier = 0;
        uint32_t flags = kRayTracingFrameFlagDirectionalShadow;
        uint32_t maxBounceCount = kDefaultRayTracingBounceCount;
        uint32_t samplesPerPixel = kDefaultRayTracingSamplesPerPixel;
        float dynamicResolutionScale = 1.0f;
        float inverseViewProjection[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
        float cameraPosition[3] = { 0.0f, 0.0f, 0.0f };
        RenderDirectionalLight directionalLight{};
        const std::vector<RenderPointLight>* pointLights = nullptr;
        const std::vector<RenderSpotLight>* spotLights = nullptr;
        uint32_t debugView = 0;
        bool iblEnabled = false;
        float iblIntensity = 0.0f;
        float iblPrefilterMaxMip = 0.0f;
        bool directionalLightMarkerEnabled = false;
        float directionalLightMarkerAngularRadius = 0.02f;
        float directionalLightMarkerHaloAngularRadius = 0.08f;
        float directionalLightMarkerBrightness = 1.0f;
        const float (*diffuseShCoefficients)[3] = nullptr;
        const std::vector<std::vector<float>>* prefilterSubresources = nullptr;
        uint32_t prefilterBaseSize = 0;
        uint32_t prefilterMipLevels = 0;
        const std::vector<float>* brdfLutPixels = nullptr;
        uint32_t brdfLutWidth = 0;
        uint32_t brdfLutHeight = 0;
    };

    struct RayTracingRuntimeStats
    {
        bool usingHardwarePath = false;
        bool usedFallback = false;
        uint32_t bvhNodeCount = 0;
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
        uint32_t qualityTier = 0;
        float dynamicResolutionScale = 1.0f;
        float sceneBuildMs = 0.0f;
        float primaryTraceMs = 0.0f;
        float shadowTraceMs = 0.0f;
        float shadeMs = 0.0f;
        float resolveMs = 0.0f;
        float traceMs = 0.0f;
        float copyMs = 0.0f;
        float lastFrameMs = 0.0f;
    };
}
