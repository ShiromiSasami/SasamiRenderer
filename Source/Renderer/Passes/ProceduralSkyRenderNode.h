#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Core/RendererFrameCoordinator.h"
#include "Renderer/Passes/IRenderNode.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"
#include "Renderer/Scene/RenderLightProxy.h"
#include "Renderer/Scene/Skybox.h"

#include <functional>

namespace SasamiRenderer
{
    // Procedural sky render node: SDF-based FBM volumetric clouds + analytic Rayleigh/Mie atmosphere.
    // Acts as an alternative to SkyboxRenderNode. Reuses the skybox cube mesh and root signature;
    // no IBL cubemap required.
    class ProceduralSkyRenderNode : public IRenderNode
    {
    public:
        std::string_view Tag() const override { return "ProceduralSky"; }
        std::string_view PhaseTag() const override { return "Scene"; }
        void BuildRequirements(RenderNodeRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderNodeContextView& context) const override;

        // Cloud coverage (0 = clear, 1 = overcast). Default 0.45.
        void SetCloudCover(float cover) { m_cloudCover = cover; }
        float GetCloudCover() const { return m_cloudCover; }

        // Cloud density multiplier. Default 2.0.
        void SetCloudDensity(float density) { m_cloudDensity = density; }
        float GetCloudDensity() const { return m_cloudDensity; }

    private:
        float m_cloudCover  = 0.45f;
        float m_cloudDensity = 2.0f;
    };
}
