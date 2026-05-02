#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Core/RendererFrameCoordinator.h"
#include "Renderer/Passes/IRenderNode.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"
#include "Renderer/Scene/RenderLightProxy.h"

namespace SasamiRenderer
{
    // Full-frame SDF fluid renderer.
    // Replaces the rasterized scene pipeline when RenderPathMode::SdfFluid is active.
    // Draws a fullscreen triangle; the PS ray-marches the SDF scene (sky + fluid).
    //
    // fluidMode:
    //   0 = liquid  (Fresnel + refraction + Beer-Lambert absorption)
    //   1 = smoke   (volume scatter + Beer-Lambert)
    //   2 = fire    (volume emission, black-body color)
    //   3 = combined (liquid surface + volumetric interior)
    class SdfFluidRenderNode : public IRenderNode
    {
    public:
        std::string_view Tag() const override { return "SdfFluid"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderNodeRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderNodeContextView& context) const override;

        // Fluid mode (see enum comment above). Default: 0 (liquid).
        void SetFluidMode(int mode) { m_fluidMode = mode; }
        int  GetFluidMode() const { return m_fluidMode; }

        // Fluid object params
        void SetFluidCenter(float x, float y, float z) { m_center[0]=x; m_center[1]=y; m_center[2]=z; }
        void SetFluidRadius(float r) { m_radius = r; }
        void SetFluidColor(float r, float g, float b) { m_color[0]=r; m_color[1]=g; m_color[2]=b; }
        void SetFluidDensity(float d) { m_density = d; }
        void SetFluidSpeed(float s) { m_speed = s; }
        void SetFluidDetail(float d) { m_detail = d; }
        void SetFluidRoughness(float r) { m_roughness = r; }
        void SetFluidIOR(float ior) { m_ior = ior; }
        void SetCloudCover(float c) { m_cloudCover = c; }
        void SetCloudDensity(float d) { m_cloudDensity = d; }

    private:
        int   m_fluidMode  = 0;
        float m_center[3]  = { 0.0f, 0.0f, 0.0f };
        float m_radius     = 2.0f;
        float m_color[3]   = { 0.05f, 0.25f, 0.6f }; // deep water blue
        float m_density    = 1.5f;
        float m_speed      = 0.06f;
        float m_detail     = 3.0f;
        float m_roughness  = 0.05f;  // very smooth water surface
        float m_ior        = 1.33f;  // water IOR
        float m_cloudCover   = 0.35f;
        float m_cloudDensity = 2.0f;
    };
}
