#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Core/RendererFrameCoordinator.h"
#include "Renderer/Passes/IRenderNode.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"
#include "Renderer/Scene/RenderLightProxy.h"

namespace SasamiRenderer
{
    // Full-frame SDF raymarching renderer.
    // Renders a procedural scene (sphere, box, capsule, ground plane) entirely via
    // raymarching — no meshes required. Includes Rayleigh-inspired sky, soft shadows,
    // and ambient occlusion.
    class RayMarchRenderNode : public IRenderNode
    {
    public:
        std::string_view Tag() const override { return "RayMarch"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderNodeRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderNodeContextView& context) const override;

        void  SetCloudCover(float c)   { m_cloudCover   = c; }
        void  SetCloudDensity(float d) { m_cloudDensity = d; }
        float GetCloudCover()    const { return m_cloudCover; }
        float GetCloudDensity()  const { return m_cloudDensity; }

        void  SetDebugMode(float m)    { m_debugMode = m; }
        float GetDebugMode()     const { return m_debugMode; }

    private:
        float m_cloudCover   = 0.35f;
        float m_cloudDensity = 2.0f;
        float m_debugMode    = 0.0f;   // 0=off, 1=distance heatmap
    };
}
