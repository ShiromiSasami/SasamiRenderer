#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/RHI/RhiTypes.h"
#include "Renderer/Resources/ShaderBlobCache.h"
#include "d3dx12.h"

namespace SasamiEngine::Render
{
    // Common per-PSO defaults shared by every graphics PSO desc in this cache
    // (SampleMask/PrimitiveTopologyType/SampleDesc.Count are identical everywhere;
    // only the topology occasionally differs between triangle and patch pipelines).
    inline void ApplyCommonPsoDefaults(D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
        D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
    {
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = topology;
        desc.SampleDesc.Count = 1;
    }

    // Linear-filter, wrap-addressed static sampler shared by the opaque/gbuffer
    // and mesh-shader root signatures (identical field values in both).
    inline D3D12_STATIC_SAMPLER_DESC MakeLinearWrapSampler(
        UINT shaderRegister = 0, UINT registerSpace = 0,
        D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_PIXEL)
    {
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        // Anisotropic filtering keeps grazing-angle surfaces (roads, walls seen at
        // shallow angles) sharp across the mip chain instead of blurring/aliasing.
        // MaxAnisotropy must be set whenever D3D12_FILTER_ANISOTROPIC is used; 8x is
        // a good quality/cost tradeoff.
        sampler.Filter = D3D12_FILTER_ANISOTROPIC;
        sampler.MaxAnisotropy = 8;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.ShaderRegister = shaderRegister;
        sampler.RegisterSpace = registerSpace;
        sampler.ShaderVisibility = visibility;
        // A zero-initialised D3D12_STATIC_SAMPLER_DESC leaves MaxLOD at 0, which clamps
        // every fetch to mip 0 and makes an uploaded mip chain inert -- minified surfaces
        // then sample the full-resolution image and alias. Open the range explicitly.
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        return sampler;
    }

    // Comparison sampler for shadow-map PCF, shared by the opaque, deferred and skinned
    // root signatures. Depth must be compared *before* filtering -- sampling a depth map
    // through a plain linear sampler and thresholding the interpolated value afterwards
    // interpolates depths rather than occlusion, which is not PCF at all.
    // Clamp addressing keeps the edge taps of the PCF kernel on the same shadow-map slice
    // (cube face for point lights) instead of folding them around to the opposite side.
    // GREATER_EQUAL matches the reverse-Z convention (near=1.0, far=0.0): a fragment is lit
    // when storedDepth >= fragmentDepth. This sampler is shared by directional, spot, and
    // point shadow lookups, so all three follow the same reverse-Z comparison.
    inline D3D12_STATIC_SAMPLER_DESC MakeShadowComparisonSampler(
        UINT shaderRegister = 1, UINT registerSpace = 0,
        D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_PIXEL)
    {
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = shaderRegister;
        sampler.RegisterSpace = registerSpace;
        sampler.ShaderVisibility = visibility;
        return sampler;
    }

    // Point-filter, clamp-addressed static sampler shared by the SSAO and
    // SSAO-blur root signatures (identical field values in both).
    inline D3D12_STATIC_SAMPLER_DESC MakePointClampSampler(
        UINT shaderRegister = 0, UINT registerSpace = 0,
        D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_PIXEL)
    {
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister = shaderRegister;
        sampler.RegisterSpace = registerSpace;
        sampler.ShaderVisibility = visibility;
        return sampler;
    }

    class RenderPipelineStateCache
    {
    public:
        bool Initialize(GraphicsDevice& device);
        inline PipelineState& GetPipelineState() { return m_pipelineState; }
        inline PipelineState& GetGBufferPipelineState() { return m_gbufferPipelineState; }
        inline PipelineState& GetToonOutlinePipelineState() { return m_toonOutlinePipelineState; }
        inline PipelineState& GetWireframePipelineState() { return m_wireframePipelineState; }
        inline PipelineState& GetDeferredLightingPipelineState() { return m_deferredLightingPipelineState; }
        inline PipelineState& GetGBufferDebugPipelineState() { return m_gbufferDebugPipelineState; }
        // Currently unused; reserved for future sorted forward transparency.
        inline PipelineState& GetTransparentPipelineState() { return m_transparentPipelineState; }
        inline PipelineState& GetTransparentOitPipelineState() { return m_transparentOitPipelineState; }
        inline PipelineState& GetTransparentOitCompositePipelineState() { return m_transparentOitCompositePipelineState; }
        inline PipelineState& GetTransparentBackfaceDistancePipelineState() { return m_transparentBackfaceDistancePipelineState; }
        inline PipelineState& GetShadowPipelineState() { return m_shadowPipelineState; }
        // D16_UNORM DSV 版(スポット/ポイントライトの512x512シャドウマップ用)。DSVFormat 以外は GetShadowPipelineState() と同一。
        inline PipelineState& GetShadowD16PipelineState() { return m_shadowD16PipelineState; }
        inline PipelineState& GetShadowVsmPipelineState() { return m_shadowVsmPipelineState; }
        // glTF alphaMode MASK casters: full G-Buffer VS + ShadowAlphaMask_PS (alpha test). D16 = spot/point maps.
        inline PipelineState& GetShadowAlphaMaskPipelineState() { return m_shadowAlphaMaskPipelineState; }
        inline PipelineState& GetShadowAlphaMaskD16PipelineState() { return m_shadowAlphaMaskD16PipelineState; }
        inline PipelineState& GetShadowVsmBlurHPipelineState() { return m_vsmBlurHPso; }
        inline PipelineState& GetShadowVsmBlurVPipelineState() { return m_vsmBlurVPso; }
        inline RootSignature& GetShadowVsmBlurRootSignature() { return m_vsmBlurRootSignature; }
        inline PipelineState& GetTessellationPipelineState() { return m_tessPipelineState; }
        inline PipelineState& GetTessellationGBufferPipelineState() { return m_tessGBufferPipelineState; }
        inline PipelineState& GetTessellationWireframePipelineState() { return m_tessWireframePipelineState; }
        inline PipelineState& GetTessellationGBufferWireframePipelineState() { return m_tessGBufferWireframePipelineState; }
        inline PipelineState& GetTessellationShadowPipelineState() { return m_tessShadowPipelineState; }
        // D16_UNORM DSV 版(スポット/ポイントライトの512x512シャドウマップ用)。DSVFormat 以外は GetTessellationShadowPipelineState() と同一。
        inline PipelineState& GetTessellationShadowD16PipelineState() { return m_tessShadowD16PipelineState; }
        inline PipelineState& GetTessellationDebugPipelineState() { return m_tessDebugPipelineState; }
        // Backward compatibility: default skybox pipeline is HDR path.
        inline PipelineState& GetSkyboxPipelineState() { return m_skyboxHdrPipelineState; }
        inline PipelineState& GetSkyboxHdrPipelineState() { return m_skyboxHdrPipelineState; }
        inline PipelineState& GetSkyboxLdrPipelineState() { return m_skyboxLdrPipelineState; }
        inline PipelineState& GetSsaoPipelineState() { return m_ssaoPipelineState; }
        inline PipelineState& GetSsaoBlurPipelineState() { return m_ssaoBlurPipelineState; }
        inline PipelineState& GetProceduralSkyPipelineState() { return m_proceduralSkyPipelineState; }
        inline PipelineState& GetRayMarchPipelineState() { return m_rayMarchPipelineState; }
        inline PipelineState& GetVolumetricCloudPipelineState() { return m_volumetricCloudPipelineState; }
        inline PipelineState& GetSwrtReflectionCompositePipelineState() { return m_swrtReflectionCompositePipelineState; }
        inline PipelineState& GetScreenSpaceReflectionPipelineState() { return m_screenSpaceReflectionPipelineState; }
        inline PipelineState& GetToneMapPipelineState() { return m_toneMapPipelineState; }
        // Same ToneMap_PS.hlsl, bound only when the swap chain is actually running HDR10
        // (Renderer::m_hdrOutputActive): its RTVFormats[0] is DXGI_FORMAT_R10G10B10A2_UNORM
        // instead of R8G8B8A8_UNORM, matching the HDR10 swap chain's backbuffer format -- a
        // graphics PSO's declared RTV formats must exactly match the RTV actually bound at Draw.
        inline PipelineState& GetToneMapHdr10PipelineState() { return m_toneMapHdr10PipelineState; }
        inline PipelineState& GetFxaaPipelineState() { return m_fxaaPipelineState; }
        inline PipelineState& GetBloomDownsamplePipelineState() { return m_bloomDownsamplePipelineState; }
        inline PipelineState& GetBloomUpsamplePipelineState() { return m_bloomUpsamplePipelineState; }
        inline PipelineState& GetLightShaftPipelineState() { return m_lightShaftPipelineState; }
        inline PipelineState& GetLensFlarePipelineState() { return m_lensFlarePipelineState; }
        inline PipelineState& GetLensFlareRayTracedPipelineState() { return m_lensFlareRayTracedPipelineState; }
        inline PipelineState& GetFxaaHdr10PipelineState() { return m_fxaaHdr10PipelineState; }
        inline PipelineState& GetLightShaftHdr10PipelineState() { return m_lightShaftHdr10PipelineState; }
        inline PipelineState& GetLensFlareHdr10PipelineState() { return m_lensFlareHdr10PipelineState; }
        inline PipelineState& GetLensFlareRayTracedHdr10PipelineState() { return m_lensFlareRayTracedHdr10PipelineState; }
        inline RootSignature& GetRootSignature() { return m_rootSignature; }
        inline RootSignature& GetDeferredLightingRootSignature() { return m_deferredLightingRootSignature; }
        inline RootSignature& GetRayMarchRootSignature()  { return m_rayMarchRootSignature;  }
        inline RootSignature& GetVolumetricCloudRootSignature() { return m_volumetricCloudRootSignature; }
        inline RootSignature& GetSsaoRootSignature() { return m_ssaoRootSignature; }
        inline RootSignature& GetSsaoBlurRootSignature() { return m_ssaoBlurRootSignature; }
        inline RootSignature& GetScreenSpaceReflectionRootSignature() { return m_screenSpaceReflectionRootSignature; }

        // Mesh shader pipeline (AS + MS + PS). Available only on DX12 Ultimate hardware.
        // Returns a null PipelineState if mesh shaders are not supported.
        inline PipelineState& GetMeshShaderPipelineState()  { return m_meshShaderPipelineState; }
        inline PipelineState& GetMeshShaderShadowPipelineState()  { return m_meshShaderShadowPipelineState; }
        inline RootSignature& GetMeshShaderRootSignature()  { return m_meshShaderRootSignature; }

        // Bindless root signature + depth-only mesh-shader PSO for the indirect (GPU-driven)
        // cluster shadow dispatch. No AS: the visible cluster list is produced by a separate
        // compute cull pass. Returns a null PipelineState if mesh shaders are not supported.
        inline RootSignature& GetClusterIndirectRootSignature()  { return m_clusterIndirectRootSignature; }
        inline PipelineState& GetClusterIndirectShadowPipelineState()  { return m_clusterIndirectShadowPipelineState; }

        // Bindless root signature + GBuffer mesh-shader PSO (MS + PS) for the indirect main-view
        // cluster dispatch. Differs from the shadow one by a PIXEL-visible material-table SRV and
        // a static sampler. Returns a null PipelineState if mesh shaders are not supported.
        inline RootSignature& GetClusterIndirectGBufferRootSignature()  { return m_clusterIndirectGBufferRootSignature; }
        inline PipelineState& GetClusterIndirectGBufferPipelineState()  { return m_clusterIndirectGBufferPipelineState; }

        // Skinned mesh pipelines (GPU skinning with bone matrix CB at b3 / root param [14])
        inline PipelineState& GetSkinnedPipelineState()            { return m_skinnedPipelineState; }
        inline PipelineState& GetSkinnedGBufferPipelineState()     { return m_skinnedGBufferPipelineState; }
        // Currently unused; reserved for future sorted forward transparency.
        inline PipelineState& GetSkinnedTransparentPipelineState() { return m_skinnedTransparentPipelineState; }
        inline PipelineState& GetSkinnedTransparentOitPipelineState() { return m_skinnedTransparentOitPipelineState; }
        inline PipelineState& GetSkinnedTransparentBackfaceDistancePipelineState() { return m_skinnedTransparentBackfaceDistancePipelineState; }
        inline PipelineState& GetSkinnedShadowPipelineState()      { return m_skinnedShadowPipelineState; }
        // D16_UNORM DSV 版(スポット/ポイントライトの512x512シャドウマップ用)。DSVFormat 以外は GetSkinnedShadowPipelineState() と同一。
        inline PipelineState& GetSkinnedShadowD16PipelineState()   { return m_skinnedShadowD16PipelineState; }
        inline PipelineState& GetSkinnedShadowAlphaMaskPipelineState()    { return m_skinnedShadowAlphaMaskPipelineState; }
        inline PipelineState& GetSkinnedShadowAlphaMaskD16PipelineState() { return m_skinnedShadowAlphaMaskD16PipelineState; }
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
        bool InitializeScreenSpaceReflectionPipeline(GraphicsDevice& device, const std::string& computeProfile);

        // Bundles the PSO building blocks that Initialize() assembles on its own stack
        // (root params/samplers/base descs/blend/rasterizer states) which
        // InitializeSkinnedPipelines() reuses. Members are references into
        // Initialize()'s local variables and are only valid for the duration of the
        // single InitializeSkinnedPipelines() call made from within Initialize().
        struct SkinnedPipelineInputs
        {
            const D3D12_ROOT_PARAMETER (&rootParams)[19];
            const D3D12_STATIC_SAMPLER_DESC (&staticSamplers)[2];
            const std::string& vertexProfile;
            const D3D12_GRAPHICS_PIPELINE_STATE_DESC& psoDesc;
            const D3D12_GRAPHICS_PIPELINE_STATE_DESC& psoGBuffer;
            const D3D12_BLEND_DESC& blendDesc;
            const D3D12_BLEND_DESC& transparentBlend;
            const D3D12_BLEND_DESC& oitBlend;
            const ComPtr<ID3DBlob>& forwardColorOnlyPS;
            const ComPtr<ID3DBlob>& transparentOitPS;
            const ComPtr<ID3DBlob>& transparentBackfaceDistancePS;
            const D3D12_RASTERIZER_DESC& backfaceDistanceRast;
            const D3D12_RASTERIZER_DESC& shadowRast;
            const D3D12_RASTERIZER_DESC& pointSpotShadowRast;
            const ComPtr<ID3DBlob>& shadowAlphaMaskPS;
        };
        bool InitializeSkinnedPipelines(GraphicsDevice& device, const SkinnedPipelineInputs& inputs);

        PipelineState m_pipelineState;
        PipelineState m_gbufferPipelineState;
        PipelineState m_toonOutlinePipelineState;
        PipelineState m_wireframePipelineState;
        PipelineState m_deferredLightingPipelineState;
        PipelineState m_gbufferDebugPipelineState;
        PipelineState m_transparentPipelineState;
        PipelineState m_transparentOitPipelineState;
        PipelineState m_transparentOitCompositePipelineState;
        PipelineState m_transparentBackfaceDistancePipelineState;
        PipelineState m_shadowPipelineState;
        PipelineState m_shadowD16PipelineState;
        PipelineState m_shadowVsmPipelineState;
        PipelineState m_shadowAlphaMaskPipelineState;
        PipelineState m_shadowAlphaMaskD16PipelineState;
        PipelineState m_vsmBlurHPso;
        PipelineState m_vsmBlurVPso;
        RootSignature m_vsmBlurRootSignature;
        PipelineState m_tessPipelineState;
        PipelineState m_tessGBufferPipelineState;
        PipelineState m_tessWireframePipelineState;
        PipelineState m_tessGBufferWireframePipelineState;
        PipelineState m_tessShadowPipelineState;
        PipelineState m_tessShadowD16PipelineState;
        PipelineState m_tessDebugPipelineState;
        PipelineState m_skyboxHdrPipelineState;
        PipelineState m_skyboxLdrPipelineState;
        PipelineState m_ssaoPipelineState;
        PipelineState m_ssaoBlurPipelineState;
        PipelineState m_proceduralSkyPipelineState;
        PipelineState m_rayMarchPipelineState;
        PipelineState m_volumetricCloudPipelineState;
        PipelineState m_swrtReflectionCompositePipelineState;
        PipelineState m_screenSpaceReflectionPipelineState;
        PipelineState m_toneMapPipelineState;
        PipelineState m_toneMapHdr10PipelineState;
        PipelineState m_fxaaPipelineState;
        PipelineState m_bloomDownsamplePipelineState;
        PipelineState m_bloomUpsamplePipelineState;
        PipelineState m_lightShaftPipelineState;
        PipelineState m_lensFlarePipelineState;
        PipelineState m_lensFlareRayTracedPipelineState;
        PipelineState m_fxaaHdr10PipelineState;
        PipelineState m_lightShaftHdr10PipelineState;
        PipelineState m_lensFlareHdr10PipelineState;
        PipelineState m_lensFlareRayTracedHdr10PipelineState;
        RootSignature m_rootSignature;
        RootSignature m_deferredLightingRootSignature;
        RootSignature m_rayMarchRootSignature;
        RootSignature m_volumetricCloudRootSignature;
        RootSignature m_ssaoRootSignature;
        RootSignature m_ssaoBlurRootSignature;
        RootSignature m_screenSpaceReflectionRootSignature;
        PipelineState m_meshShaderPipelineState;
        PipelineState m_meshShaderShadowPipelineState;
        RootSignature m_meshShaderRootSignature;
        RootSignature m_clusterIndirectRootSignature;
        PipelineState m_clusterIndirectShadowPipelineState;
        RootSignature m_clusterIndirectGBufferRootSignature;
        PipelineState m_clusterIndirectGBufferPipelineState;
        PipelineState m_skinnedPipelineState;
        PipelineState m_skinnedGBufferPipelineState;
        PipelineState m_skinnedTransparentPipelineState;
        PipelineState m_skinnedTransparentOitPipelineState;
        PipelineState m_skinnedTransparentBackfaceDistancePipelineState;
        PipelineState m_skinnedShadowPipelineState;
        PipelineState m_skinnedShadowD16PipelineState;
        PipelineState m_skinnedShadowAlphaMaskPipelineState;
        PipelineState m_skinnedShadowAlphaMaskD16PipelineState;
        RootSignature m_skinnedRootSignature;

        // Memoizes shader blobs across the main array loop and the sub-init
        // functions below so each (source, entry, target) resolves once per run.
        ShaderBlobCache m_shaderBlobCache;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
