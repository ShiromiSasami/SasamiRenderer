#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/RayTracing/RayTracingScene.h"

#include <wrl.h>

#include <cstdint>
#include <cstddef>
#include <vector>

namespace SasamiRenderer
{
    class DxrRayTracer
    {
    public:
        struct DescriptorSet
        {
            CpuDescriptorHandle outputUavCpu{};
            UINT outputDescriptorIndex = 0;
            CpuDescriptorHandle tlasSrvCpu{};
            UINT tlasDescriptorIndex = 0;
            CpuDescriptorHandle vertexSrvCpu{};
            UINT vertexDescriptorIndex = 0;
            CpuDescriptorHandle indexSrvCpu{};
            UINT indexDescriptorIndex = 0;
            CpuDescriptorHandle materialSrvCpu{};
            UINT materialDescriptorIndex = 0;
            CpuDescriptorHandle instanceSrvCpu{};
            UINT instanceDescriptorIndex = 0;
        };

        bool Initialize(IRHIDevice& device, const DescriptorSet& descriptors);
        bool IsSupported() const { return m_supported; }
        void UpdateScene(IRHIDevice& device, const RayTracingScene& scene);
        bool Render(IRHIDevice& device,
                    CommandList& cmdList,
                    DescriptorHeap& srvHeap,
                    Resource& outputTexture,
                    const RayTracingFrameDesc& frameDesc,
                    RayTracingRuntimeStats& outStats);

    private:
        static constexpr uint32_t kMaxPointLights = 16u;
        static constexpr uint32_t kMaxSpotLights = 16u;

        struct GpuVertex
        {
            float position[3];
            float normal[3];
            float color[4];
            float uv[2];
        };

        struct GpuMaterial
        {
            int32_t albedoDescriptorIndex = -1;
            int32_t occlusionDescriptorIndex = -1;
            float metallic = 0.0f;
            float roughness = 0.5f;
            float baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            float emissiveOcclusionStrength[4] = {};
        };

        struct GpuInstance
        {
            uint32_t vertexOffset = 0;
            uint32_t indexOffset = 0;
            uint32_t materialIndex = 0;
            uint32_t padding = 0;
        };

        struct GpuPointLight
        {
            float posRange[4] = {};
            float colorIntensity[4] = {};
        };

        struct GpuSpotLight
        {
            float posRange[4] = {};
            float dirCosInner[4] = {};
            float colorIntensity[4] = {};
            float params[4] = {};
        };

        struct alignas(16) FrameConstants
        {
            uint32_t renderWidth = 0;
            uint32_t renderHeight = 0;
            uint32_t outputWidth = 0;
            uint32_t outputHeight = 0;
            uint32_t outputDescriptorIndex = 0;
            uint32_t vertexDescriptorIndex = 0;
            uint32_t indexDescriptorIndex = 0;
            uint32_t materialDescriptorIndex = 0;
            uint32_t instanceDescriptorIndex = 0;
            uint32_t pointLightCount = 0;
            uint32_t spotLightCount = 0;
            uint32_t pointLightBudget = 0;
            uint32_t spotLightBudget = 0;
            uint32_t qualityTier = 0;
            uint32_t debugView = 0;
            uint32_t flags = 0;
            uint32_t maxBounceCount = kDefaultRayTracingBounceCount;
            float dynamicResolutionScale = 1.0f;
            float padding[2] = {};
            float cameraPosition[4] = {};
            float inverseViewProjection[16] = {};
            float directionalLightDirection[4] = {};
            float directionalLightColorIntensity[4] = {};
            float directionalLightMarkerParams[4] = {};
            GpuPointLight pointLights[kMaxPointLights];
            GpuSpotLight spotLights[kMaxSpotLights];
        };
        static_assert(offsetof(FrameConstants, cameraPosition) == 80u,
                      "FrameConstants.cameraPosition must match HLSL cbuffer packing.");
        static_assert(offsetof(FrameConstants, inverseViewProjection) == 96u,
                      "FrameConstants.inverseViewProjection must match HLSL cbuffer packing.");

        struct MeshRecord
        {
            uint32_t vertexOffset = 0;
            uint32_t vertexCount = 0;
            uint32_t indexOffset = 0;
            uint32_t indexCount = 0;
            Resource blas;
        };

        bool CompileShadersAndCreateStateObject(IRHIDevice& device);
        bool CreateShaderTables(IRHIDevice& device);
        bool UploadSceneBuffers(IRHIDevice& device);
        bool BuildAccelerationStructures(IRHIDevice& device);
        bool EnsureFrameConstantBuffer(IRHIDevice& device);
        void FillFrameConstants(const RayTracingFrameDesc& frameDesc, FrameConstants& outConstants) const;

        DescriptorSet m_descriptors{};
        bool m_supported = false;
        bool m_pipelineReady = false;
        bool m_sceneDirty = true;
        RayTracingScene m_scene;
        std::vector<MeshRecord> m_meshRecords;
        float m_lastSceneBuildMs = 0.0f;
        bool m_reportSceneBuildCost = false;
        uint64_t m_uploadedGeometryVersion = 0;
        uint64_t m_uploadedMaterialVersion = 0;
        uint64_t m_uploadedInstanceVersion = 0;

        Resource m_vertexBuffer;
        Resource m_indexBuffer;
        Resource m_materialBuffer;
        Resource m_instanceBuffer;
        Resource m_tlas;

        Resource m_frameConstantsBuffer;
        uint8_t* m_frameConstantsPtr = nullptr;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_globalRootSignature;
        Microsoft::WRL::ComPtr<ID3D12StateObject> m_stateObject;
        Microsoft::WRL::ComPtr<ID3D12StateObjectProperties> m_stateObjectProperties;

        Resource m_rayGenShaderTable;
        Resource m_missShaderTable;
        Resource m_hitGroupShaderTable;
        UINT m_rayGenShaderRecordSize = 0;
        UINT m_missShaderRecordSize = 0;
        UINT m_hitGroupShaderRecordSize = 0;
    };
}
