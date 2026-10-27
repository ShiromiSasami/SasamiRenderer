#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Scene/RenderLightProxy.h"
#include "Renderer/Scene/ShadowMapManager.h"
#include "Renderer/Structures/RendererEnums.h"

#include <functional>
#include <vector>

namespace SasamiEngine::Render
{
    class LightSystem
    {
    public:
        static constexpr uint32_t kDirectionalCascadeCount = 4u;
        using DirectionalLightSettings = RenderDirectionalLight;
        using PointLight = RenderPointLight;
        using SpotLight = RenderSpotLight;

        struct FrameResources
        {
            Resource lightCB;
            RhiBufferHandle lightCBHandle{};
            Resource* lightCBCompat = nullptr;
            void* lightCBPtr = nullptr;

            Resource pointLightBuffer;
            Resource spotLightBuffer;
            RhiBufferHandle pointLightBufferHandle{};
            RhiBufferHandle spotLightBufferHandle{};
            Resource* pointLightBufferCompat = nullptr;
            Resource* spotLightBufferCompat = nullptr;
            void* pointLightBufferPtr = nullptr;
            void* spotLightBufferPtr = nullptr;
            UINT pointLightCapacity = 0;
            UINT spotLightCapacity = 0;

            CpuDescriptorHandle pointSrvCpu{};
            CpuDescriptorHandle spotSrvCpu{};
            GpuDescriptorHandle lightSrvTable{};

            Resource* GetLightCBResource()
            {
                return lightCBCompat ? lightCBCompat : &lightCB;
            }

            const Resource* GetLightCBResource() const
            {
                return lightCBCompat ? lightCBCompat : &lightCB;
            }

            Resource* GetPointLightResource()
            {
                return pointLightBufferCompat ? pointLightBufferCompat : &pointLightBuffer;
            }

            Resource* GetSpotLightResource()
            {
                return spotLightBufferCompat ? spotLightBufferCompat : &spotLightBuffer;
            }
        };

        struct ShadowPassContext
        {
            float lightViewProjection[16];
            float cascadeLightViewProjection[kDirectionalCascadeCount][16];
            float cascadeSplitDepths[kDirectionalCascadeCount];
            float cascadeTexelSize[kDirectionalCascadeCount][2];
            float cascadeBlendFraction = 0.1f;
            uint32_t cascadeCount = 1u;
            uint32_t activeCascadeIndex = 0u;
            bool d16Target = false; // true for the spot/point D16_UNORM maps
            // Draw callbacks may switch glTF MASK items to the alpha-tested shadow PSOs (and back to the
            // plain Shadow/ShadowD16 PSO afterwards). Left false for VSM and tessellation passes,
            // which have no alpha-tested variant yet.
            bool allowAlphaMaskPipeline = false;
            // Set on the vertex-path call that follows the cluster callback: draw only the
            // dynamic-vertex items, which have no clusters.
            bool dynamicVerticesOnly = false;
        };

        using AllocateSrvRangeCallback = std::function<bool(UINT count,
                                                            CpuDescriptorHandle& outCpu,
                                                            GpuDescriptorHandle& outGpu)>;
        using DrawShadowCallback = std::function<void(const ShadowPassContext& context)>;

        bool Initialize(IRHIDevice& device, const AllocateSrvRangeCallback& allocateSrvRange);
        bool InitializeFrameResources(FrameResources& frame, const AllocateSrvRangeCallback& allocateSrvRange);
        void ShutdownFrameResources(FrameResources& frame);

        void UpdateFrameLighting(FrameResources& frame,
                                 const float cameraPos[3],
                                 const float cameraPV[16],
                                 bool iblEnabled,
                                 float iblIntensity,
                                 float iblPrefilterMaxMip,
                                 bool hasDiffuseSh,
                                 const float (*diffuseShCoefficients)[3],
                                 RendererEnums::GBufferDebugView debugView,
                                 uint32_t shadowMapWidth,
                                 uint32_t shadowMapHeight,
                                 float reflectionMode,
                                 float reflectionStrength,
                                 uint32_t renderWidth,
                                 uint32_t renderHeight,
                                 bool useStableDirectionalShadowProjection = false);

        void BuildShadowPassContext(ShadowPassContext& outContext,
                                    const float cameraPos[3],
                                    const float cameraPV[16],
                                    uint32_t shadowMapWidth,
                                    uint32_t shadowMapHeight) const;

        void BuildStableDirectionalShadowPassContext(ShadowPassContext& outContext,
                                                     uint32_t shadowMapWidth,
                                                     uint32_t shadowMapHeight) const;

        void ExecuteShadowPass(CommandList* cmdList,
                               FrameResources& frame,
                               RenderPipelineStateCache& pipelineStateCache,
                               DescriptorHeap& srvHeap,
                               const float cameraPos[3],
                               const float cameraPV[16],
                               bool useTessellationPath,
                               bool iblEnabled,
                               float iblIntensity,
                               float iblPrefilterMaxMip,
                               bool hasDiffuseSh,
                               const float (*diffuseShCoefficients)[3],
                               RendererEnums::GBufferDebugView debugView,
                               uint32_t shadowMapWidth,
                               uint32_t shadowMapHeight,
                               float reflectionMode,
                               float reflectionStrength,
                               uint32_t renderWidth,
                               uint32_t renderHeight,
                               const DrawShadowCallback& drawCallback,
                               const DrawShadowCallback& drawSkinnedCallback,
                               bool vsmBlurEnabled = true,
                               const DrawShadowCallback& drawClusterCallback = {});

        // Renders spot-light and point-light (cube) shadow maps. Split out of ExecuteShadowPass
        // so callers that replace only the directional cascade rendering (e.g. SWRT) can still
        // invoke local-light shadow rendering, which must run regardless of the directional path.
        void ExecuteLocalLightShadowPasses(CommandList* cmdList,
                                           FrameResources& frame,
                                           RenderPipelineStateCache& pipelineStateCache,
                                           DescriptorHeap& srvHeap,
                                           bool useTessellationPath,
                                           const DrawShadowCallback& drawCallback,
                                           const DrawShadowCallback& drawSkinnedCallback);

        GpuDescriptorHandle GetShadowSrv()      const { return m_shadowMapManager.GetShadowSrv(); }
        // Pixel size of the raster directional cascade maps (the cluster shadow path converts
        // mesh-space LOD error into shadow-map texels with it).
        UINT GetDirectionalShadowMapSize() const { return m_shadowMapManager.GetShadowMapSize(); }
        GpuDescriptorHandle GetSpotShadowSrv()  const { return m_shadowMapManager.GetSpotShadowSrv(); }
        GpuDescriptorHandle GetPointShadowSrv() const { return m_shadowMapManager.GetPointShadowSrv(); }
        GpuDescriptorHandle GetVsmSrv()         const { return m_shadowMapManager.GetVsmSrv(); }
        ShadowMapManager& GetShadowMapManager() { return m_shadowMapManager; }
        const ShadowMapManager& GetShadowMapManager() const { return m_shadowMapManager; }

        // Lazily creates the raster directional shadow map resources/SRV without rendering into
        // them. Used by Renderer::BuildRenderPassFrameInputs to guarantee a dimension-matching,
        // validly-allocated fallback descriptor for ShadowMapTex when the SWRT directional shadow
        // path is enabled but its texture is not trustworthy this frame (its content is never
        // sampled in that case, see SetDirectionalShadowSourceState / dirColor.w mode 2).
        bool EnsureDirectionalShadowResources() { return m_shadowMapManager.EnsureShadowResources(); }

        // Records whether the SWRT directional shadow path is active this frame and, if so,
        // whether its texture currently holds trustworthy visibility data. Must be called before
        // UpdateFrameLighting() runs each frame (see Renderer::BuildRenderPassExecutionPolicy);
        // consumed by UpdateFrameLighting() to set the LightCB dirColor.w shadow-source-mode flag.
        void SetDirectionalShadowSourceState(bool useSwrt, bool swrtDataTrustworthy)
        {
            m_directionalShadowSourceMode = !useSwrt ? 0.0f : (swrtDataTrustworthy ? 1.0f : 2.0f);
        }

        // Cel shading controls, driven by the toon sample's UI. Global rather than per-material
        // because the GBuffer has no spare channel to carry them per surface.
        struct ToonSettings
        {
            float bandCount = 3.0f;
            float bandSoftness = 0.03f;
            float specularThreshold = 0.85f;
            float specularSoftness = 0.02f;
            float rimStrength = 0.25f;
            float rimPower = 3.0f;
            float specularIntensity = 0.25f;
            float shadowFloor = 0.28f;
            float ambientScale = 0.35f;
            float outlineWidth = 0.02f;
            float outlineColor[3] = { 0.05f, 0.04f, 0.05f };
            // Screen-space depth rim (Genshin-style): sample the GBuffer distance a few pixels
            // along the screen-projected normal; a much farther neighbour marks a silhouette.
            // depthRimWidth is in pixels at 1 m and shrinks with distance (0 disables).
            float depthRimWidth = 0.0f;
            float depthRimThreshold = 0.25f; // metres of distance jump that counts as an edge
            float depthRimStrength = 0.6f;
        };
        ToonSettings GetToonSettings() const
        {
            ToonSettings settings{};
            settings.bandCount = m_toonBandCount;
            settings.bandSoftness = m_toonBandSoftness;
            settings.specularThreshold = m_toonSpecularThreshold;
            settings.specularSoftness = m_toonSpecularSoftness;
            settings.rimStrength = m_toonRimStrength;
            settings.rimPower = m_toonRimPower;
            settings.specularIntensity = m_toonSpecularIntensity;
            settings.shadowFloor = m_toonShadowFloor;
            settings.ambientScale = m_toonAmbientScale;
            settings.outlineWidth = m_toonOutlineWidth;
            settings.outlineColor[0] = m_toonOutlineColor[0];
            settings.outlineColor[1] = m_toonOutlineColor[1];
            settings.outlineColor[2] = m_toonOutlineColor[2];
            settings.depthRimWidth = m_toonDepthRimWidth;
            settings.depthRimThreshold = m_toonDepthRimThreshold;
            settings.depthRimStrength = m_toonDepthRimStrength;
            return settings;
        }
        void SetToonSettings(const ToonSettings& settings)
        {
            m_toonBandCount         = (settings.bandCount > 1.0f) ? settings.bandCount : 1.0f;
            m_toonBandSoftness      = settings.bandSoftness;
            m_toonSpecularThreshold = settings.specularThreshold;
            m_toonSpecularSoftness  = settings.specularSoftness;
            m_toonRimStrength       = settings.rimStrength;
            m_toonRimPower          = settings.rimPower;
            m_toonSpecularIntensity = settings.specularIntensity;
            m_toonShadowFloor       = settings.shadowFloor;
            m_toonAmbientScale      = settings.ambientScale;
            m_toonOutlineWidth      = (settings.outlineWidth > 0.0f) ? settings.outlineWidth : 0.0f;
            m_toonOutlineColor[0]   = settings.outlineColor[0];
            m_toonOutlineColor[1]   = settings.outlineColor[1];
            m_toonOutlineColor[2]   = settings.outlineColor[2];
            m_toonDepthRimWidth     = (settings.depthRimWidth > 0.0f) ? settings.depthRimWidth : 0.0f;
            m_toonDepthRimThreshold = (settings.depthRimThreshold > 1e-4f) ? settings.depthRimThreshold : 1e-4f;
            m_toonDepthRimStrength  = settings.depthRimStrength;
        }

        // SDF face shadow (stage 2): materials flagged SurfaceMaterial::toonFaceShadow replace
        // N.L with a threshold-map lookup driven by the light angle in the face's frame
        // (Genshin-style; refs akasaki1211/sdf_shadow_threshold_map, ChiliMilk/URP_Toon).
        // Kept apart from ToonSettings so toon presets do not reset the face frame.
        // The frame is world-space and static: it does not follow a head bone yet.
        struct ToonFaceSettings
        {
            bool enabled = false;
            float forward[3] = { 0.0f, 0.0f, -1.0f };
            float right[3] = { 1.0f, 0.0f, 0.0f };
            float softness = 0.02f;
        };
        const ToonFaceSettings& GetToonFaceSettings() const { return m_toonFace; }
        void SetToonFaceSettings(const ToonFaceSettings& settings) { m_toonFace = settings; }

        // Toon hair angel ring: materials flagged SurfaceMaterial::toonHair swap the flat toon
        // highlight for two thresholded Kajiya-Kay bands (Scheuermann 2004). The strand direction
        // is 'up' projected on the shading plane; it is fixed and does not follow the head bone.
        struct ToonHairSettings
        {
            bool enabled = false;
            float up[3] = { 0.0f, 1.0f, 0.0f };
            float primaryShift = 0.0f;
            float secondaryShift = 0.2f;
            float primaryWidth = 0.02f;
            float secondaryWidth = 0.05f;
            float primaryStrength = 0.6f;
            float secondaryStrength = 0.3f;
            float softness = 0.005f;
        };
        const ToonHairSettings& GetToonHairSettings() const { return m_toonHair; }
        void SetToonHairSettings(const ToonHairSettings& settings) { m_toonHair = settings; }

        // Front-hair shadow: non-hair toon pixels march toward the light in screen space and are
        // darkened when they pass behind a SurfaceMaterial::toonHair pixel (Genshin-style screen-space
        // hair shadow; a contact-shadow march that only accepts hair as the occluder).
        struct ToonHairShadowSettings
        {
            bool enabled = false;
            float length = 0.04f;
            float thickness = 0.03f;
            float strength = 1.0f;
        };
        const ToonHairShadowSettings& GetToonHairShadowSettings() const { return m_toonHairShadow; }
        void SetToonHairShadowSettings(const ToonHairShadowSettings& settings) { m_toonHairShadow = settings; }

        // Head sphere normals: inside a sphere around the head, non-hair toon pixels light with a
        // normal pulled toward (P - centre), so the face shades as one smooth volume instead of
        // following its small creases (the proxy-shape normal edit of Motomura, GDC 2015, done at
        // lighting time instead of authored into the mesh). The blend fades out over the outer 20%
        // of the radius. Only lighting sees it; the GBuffer normal is unchanged.
        struct ToonHeadNormalSettings
        {
            bool enabled = false;
            float center[3] = { 0.0f, 1.45f, 0.0f };
            float radius = 0.13f;
            float blend = 0.8f;
        };
        const ToonHeadNormalSettings& GetToonHeadNormalSettings() const { return m_toonHeadNormal; }
        void SetToonHeadNormalSettings(const ToonHeadNormalSettings& settings) { m_toonHeadNormal = settings; }

        DirectionalLightSettings GetDirectionalLightSettings() const;
        void SetDirectionalLightSettings(const DirectionalLightSettings& settings);

        std::vector<PointLight>& GetPointLights() { return m_pointLights; }
        const std::vector<PointLight>& GetPointLights() const { return m_pointLights; }
        std::vector<SpotLight>& GetSpotLights() { return m_spotLights; }
        const std::vector<SpotLight>& GetSpotLights() const { return m_spotLights; }

        float GetAoMinOcclusion() const { return m_aoMinOcclusion; }
        void SetAoMinOcclusion(float v)
        {
            if (v < 0.0f) {
                v = 0.0f;
            } else if (v > 1.0f) {
                v = 1.0f;
            }
            m_aoMinOcclusion = v;
        }

        float GetAoDirectLightingStrength() const { return m_aoDirectLightingStrength; }
        void SetAoDirectLightingStrength(float v)
        {
            if (v < 0.0f) {
                v = 0.0f;
            } else if (v > 1.0f) {
                v = 1.0f;
            }
            m_aoDirectLightingStrength = v;
        }

        RendererEnums::PunctualLightFalloff GetPunctualLightFalloff() const { return m_punctualLightFalloff; }
        void SetPunctualLightFalloff(RendererEnums::PunctualLightFalloff v) { m_punctualLightFalloff = v; }

        float GetShadowDistance() const { return m_shadowDistance; }
        void SetShadowDistance(float v)
        {
            if (v < 1.0f) {
                v = 1.0f;
            }
            m_shadowDistance = v;
        }

    private:
        void EnsureLightBuffers(FrameResources& frame, size_t pointCount, size_t spotCount);

        // Renders the directional cascade shadow depth (and VSM moments/blur if enabled).
        // Returns false if shadow resources could not be ensured (mirrors the original
        // ExecuteShadowPass early-return, which also skipped local-light shadow passes).
        bool ExecuteDirectionalShadowPass(CommandList* cmdList,
                                          FrameResources& frame,
                                          RenderPipelineStateCache& pipelineStateCache,
                                          DescriptorHeap& srvHeap,
                                          const float cameraPos[3],
                                          const float cameraPV[16],
                                          bool useTessellationPath,
                                          uint32_t shadowMapWidth,
                                          uint32_t shadowMapHeight,
                                          const DrawShadowCallback& drawCallback,
                                          const DrawShadowCallback& drawSkinnedCallback,
                                          bool vsmBlurEnabled,
                                          const DrawShadowCallback& drawClusterCallback = {});

        IRHIDevice* m_device = nullptr;

        ShadowMapManager m_shadowMapManager;

        float m_lightYaw = 3.14159265f;  // 180 deg
        float m_lightPitch = 1.55334306f; // 89 deg
        float m_lightDistance = 50.0f;
        float m_lightOrthoHalf = 14.0f;
        float m_lightNear = 30.0f;
        float m_lightFar = 60.0f;
        float m_dirColor[3] = { 1.0f, 1.0f, 1.0f };
        float m_dirIntensity = 1.0f;
        float m_aoMinOcclusion = 0.0f;
        float m_aoDirectLightingStrength = 0.5f;
        RendererEnums::PunctualLightFalloff m_punctualLightFalloff = RendererEnums::PunctualLightFalloff::InverseSquare;
        // See SetDirectionalShadowSourceState: 0 = raster cascades, 1 = SWRT (trustworthy),
        // 2 = SWRT enabled but not trustworthy this frame (fallback, unshadowed).
        float m_directionalShadowSourceMode = 0.0f;
        DirectionalShadowMode m_shadowMode = DirectionalShadowMode::Csm4;

        // Cel shading controls (see LightCBData.h). Global rather than per-material because the
        // GBuffer has no spare channel; a toon style wants one consistent look anyway.
        // Three bands is the classic light / mid / shadow split.
        float m_toonBandCount         = 3.0f;
        float m_toonBandSoftness      = 0.03f;
        float m_toonSpecularThreshold = 0.85f;
        float m_toonSpecularSoftness  = 0.02f;
        float m_toonRimStrength       = 0.25f;
        float m_toonRimPower          = 3.0f;
        float m_toonSpecularIntensity = 0.25f;
        float m_toonShadowFloor       = 0.28f;
        float m_toonAmbientScale      = 0.35f;
        float m_toonOutlineWidth      = 0.02f;
        float m_toonOutlineColor[3]   = { 0.05f, 0.04f, 0.05f };
        float m_toonDepthRimWidth     = 0.0f;
        float m_toonDepthRimThreshold = 0.25f;
        float m_toonDepthRimStrength  = 0.6f;
        ToonFaceSettings m_toonFace{};
        ToonHairSettings m_toonHair{};
        ToonHairShadowSettings m_toonHairShadow{};
        ToonHeadNormalSettings m_toonHeadNormal{};
        float m_shadowDistance = 80.0f;
        float m_cascadeDistributionExponent = 2.0f;
        float m_cascadeBlendFraction = 0.1f;
        DirectionalShadowDepthRangeMode m_depthRangeMode = DirectionalShadowDepthRangeMode::Stable;
        float m_shadowDepthBias = 1000.0f;
        float m_shadowSlopeScaleBias = 2.0f;
        float m_shadowNormalBias = 0.02f;
        float m_shadowFarBiasScale = 1.5f;
        bool  m_contactShadowEnabled = true;

        std::vector<PointLight> m_pointLights = { PointLight{} };
        std::vector<SpotLight> m_spotLights;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
