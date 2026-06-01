#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RhiTypes.h"
#include "d3dx12.h"

namespace SasamiRenderer
{
    class RenderPipelineStateCache
    {
    public:
        bool Initialize(GraphicsDevice& device);
        inline PipelineState& GetPipelineState() { return m_pipelineState; }
        inline PipelineState& GetBasicPipelineState() { return m_basicPipelineState; }
        inline PipelineState& GetTransparentPipelineState() { return m_transparentPipelineState; }
        inline PipelineState& GetTransparentOitPipelineState() { return m_transparentOitPipelineState; }
        inline PipelineState& GetTransparentOitCompositePipelineState() { return m_transparentOitCompositePipelineState; }
        inline PipelineState& GetTransparentBasicPipelineState() { return m_transparentBasicPipelineState; }
        inline PipelineState& GetTransparentBackfaceDistancePipelineState() { return m_transparentBackfaceDistancePipelineState; }
        inline PipelineState& GetShadowPipelineState() { return m_shadowPipelineState; }
        inline PipelineState& GetShadowVsmPipelineState() { return m_shadowVsmPipelineState; }
        inline PipelineState& GetShadowVsmBlurHPipelineState() { return m_vsmBlurHPso; }
        inline PipelineState& GetShadowVsmBlurVPipelineState() { return m_vsmBlurVPso; }
        inline RootSignature& GetShadowVsmBlurRootSignature() { return m_vsmBlurRootSignature; }
        inline PipelineState& GetTessellationPipelineState() { return m_tessPipelineState; }
        inline PipelineState& GetTessellationWireframePipelineState() { return m_tessWireframePipelineState; }
        inline PipelineState& GetTessellationShadowPipelineState() { return m_tessShadowPipelineState; }
        inline PipelineState& GetTessellationDebugPipelineState() { return m_tessDebugPipelineState; }
        inline PipelineState& GetMeshletDebugPipelineState() { return m_meshletDebugPipelineState; }
        // Backward compatibility: default skybox pipeline is HDR path.
        inline PipelineState& GetSkyboxPipelineState() { return m_skyboxHdrPipelineState; }
        inline PipelineState& GetSkyboxHdrPipelineState() { return m_skyboxHdrPipelineState; }
        inline PipelineState& GetSkyboxLdrPipelineState() { return m_skyboxLdrPipelineState; }
        inline PipelineState& GetSsaoPipelineState() { return m_ssaoPipelineState; }
        inline PipelineState& GetSsaoBlurPipelineState() { return m_ssaoBlurPipelineState; }
        inline PipelineState& GetProceduralSkyPipelineState() { return m_proceduralSkyPipelineState; }
        inline PipelineState& GetSdfFluidPipelineState() { return m_sdfFluidPipelineState; }
        inline PipelineState& GetRayMarchPipelineState() { return m_rayMarchPipelineState; }
        inline PipelineState& GetVolumetricCloudPipelineState() { return m_volumetricCloudPipelineState; }
        inline PipelineState& GetSwrtReflectionCompositePipelineState() { return m_swrtReflectionCompositePipelineState; }
        inline PipelineState& GetToneMapPipelineState() { return m_toneMapPipelineState; }
        inline RootSignature& GetRootSignature() { return m_rootSignature; }
        inline RootSignature& GetSdfFluidRootSignature() { return m_sdfFluidRootSignature; }
        inline RootSignature& GetRayMarchRootSignature()  { return m_rayMarchRootSignature;  }
        inline RootSignature& GetVolumetricCloudRootSignature() { return m_volumetricCloudRootSignature; }
        inline RootSignature& GetSsaoRootSignature() { return m_ssaoRootSignature; }
        inline RootSignature& GetSsaoBlurRootSignature() { return m_ssaoBlurRootSignature; }

        // Mesh shader pipeline (AS + MS + PS). Available only on DX12 Ultimate hardware.
        // Returns a null PipelineState if mesh shaders are not supported.
        inline PipelineState& GetMeshShaderPipelineState()  { return m_meshShaderPipelineState; }
        inline RootSignature& GetMeshShaderRootSignature()  { return m_meshShaderRootSignature; }

        // Skinned mesh pipelines (GPU skinning with bone matrix CB at b3 / root param [14])
        inline PipelineState& GetSkinnedPipelineState()            { return m_skinnedPipelineState; }
        inline PipelineState& GetSkinnedTransparentPipelineState() { return m_skinnedTransparentPipelineState; }
        inline PipelineState& GetSkinnedTransparentOitPipelineState() { return m_skinnedTransparentOitPipelineState; }
        inline PipelineState& GetSkinnedTransparentBackfaceDistancePipelineState() { return m_skinnedTransparentBackfaceDistancePipelineState; }
        inline PipelineState& GetSkinnedShadowPipelineState()      { return m_skinnedShadowPipelineState; }
        inline RootSignature& GetSkinnedRootSignature()            { return m_skinnedRootSignature; }

        // Convert D3D12 wrapper objects to backend-opaque RHI handles.
        // The id field stores the raw COM pointer; valid only within the same D3D12 device lifetime.
        static RhiPipelineHandle       MakePipelineHandle(const PipelineState& pso);
        static RhiPipelineLayoutHandle MakeLayoutHandle(const RootSignature& sig);
        static RhiDescriptorHeapHandle MakeDescriptorHeapHandle(const DescriptorHeap& heap);

    private:
        bool InitializeMeshShaderPipeline(GraphicsDevice& device, const std::string& shaderModel);
        bool InitializeEffectPipelines(GraphicsDevice& device, const std::string& vertexProfile, const std::string& pixelProfile);
        bool InitializeSsaoPipelines(GraphicsDevice& device, const std::string& vertexProfile, const std::string& pixelProfile);
        PipelineState m_pipelineState;
        PipelineState m_basicPipelineState;
        PipelineState m_transparentPipelineState;
        PipelineState m_transparentOitPipelineState;
        PipelineState m_transparentOitCompositePipelineState;
        PipelineState m_transparentBasicPipelineState;
        PipelineState m_transparentBackfaceDistancePipelineState;
        PipelineState m_shadowPipelineState;
        PipelineState m_shadowVsmPipelineState;
        PipelineState m_vsmBlurHPso;
        PipelineState m_vsmBlurVPso;
        RootSignature m_vsmBlurRootSignature;
        PipelineState m_tessPipelineState;
        PipelineState m_tessWireframePipelineState;
        PipelineState m_tessShadowPipelineState;
        PipelineState m_tessDebugPipelineState;
        PipelineState m_meshletDebugPipelineState;
        PipelineState m_skyboxHdrPipelineState;
        PipelineState m_skyboxLdrPipelineState;
        PipelineState m_ssaoPipelineState;
        PipelineState m_ssaoBlurPipelineState;
        PipelineState m_proceduralSkyPipelineState;
        PipelineState m_sdfFluidPipelineState;
        PipelineState m_rayMarchPipelineState;
        PipelineState m_volumetricCloudPipelineState;
        PipelineState m_swrtReflectionCompositePipelineState;
        PipelineState m_toneMapPipelineState;
        RootSignature m_rootSignature;
        RootSignature m_sdfFluidRootSignature;
        RootSignature m_rayMarchRootSignature;
        RootSignature m_volumetricCloudRootSignature;
        RootSignature m_ssaoRootSignature;
        RootSignature m_ssaoBlurRootSignature;
        PipelineState m_meshShaderPipelineState;
        RootSignature m_meshShaderRootSignature;
        PipelineState m_skinnedPipelineState;
        PipelineState m_skinnedTransparentPipelineState;
        PipelineState m_skinnedTransparentOitPipelineState;
        PipelineState m_skinnedTransparentBackfaceDistancePipelineState;
        PipelineState m_skinnedShadowPipelineState;
        RootSignature m_skinnedRootSignature;
    };
}
