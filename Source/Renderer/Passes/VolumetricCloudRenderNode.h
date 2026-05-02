#pragma once

#include "Renderer/Passes/IRenderNode.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"

namespace SasamiRenderer
{
    // Volumetric cloud render node.
    //
    // Renders a raymarched cloud layer over the existing sky background using
    // path-tracing style single scattering (Beer-Lambert + Henyey-Greenstein).
    // Draws a fullscreen triangle at NDC z=1 with alpha blending and a
    // depth test (LESS_EQUAL, no write) so it composites only on sky pixels.
    //
    // Parameters (adjustable at runtime via setters):
    //   cloudCover    – fraction of sky covered [0..1]  (default 0.45)
    //   cloudDensity  – overall optical thickness scale  (default 2.0)
    //   windSpeed     – animation speed in m/s           (default 8.0)
    //   cloudBaseAlt  – cloud layer base altitude in m   (default 1500)
    //   cloudTopAlt   – cloud layer top  altitude in m   (default 5000)
    class VolumetricCloudRenderNode : public IRenderNode
    {
    public:
        std::string_view Tag()      const override { return "VolumetricCloud"; }
        std::string_view PhaseTag() const override { return "Scene"; }

        void BuildRequirements(RenderNodeRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderNodeContextView& context) const override;

        bool IsEnabled()       const { return m_enabled; }
        void SetEnabled(bool e)      { m_enabled = e; }

        // Cloud parameters.
        float GetCloudCover()   const { return m_cloudCover;   }
        float GetCloudDensity() const { return m_cloudDensity; }
        float GetWindSpeed()    const { return m_windSpeed;     }
        float GetCloudBaseAlt() const { return m_cloudBaseAlt; }
        float GetCloudTopAlt()  const { return m_cloudTopAlt;  }

        void SetCloudCover(float v)    { m_cloudCover    = v; }
        void SetCloudDensity(float v)  { m_cloudDensity  = v; }
        void SetWindSpeed(float v)     { m_windSpeed     = v; }
        void SetCloudBaseAlt(float v)  { m_cloudBaseAlt  = v; }
        void SetCloudTopAlt(float v)   { m_cloudTopAlt   = v; }

    private:
        bool  m_enabled      = false;
        float m_cloudCover   = 0.45f;
        float m_cloudDensity = 2.0f;
        float m_windSpeed    = 8.0f;
        float m_cloudBaseAlt = 1500.0f;
        float m_cloudTopAlt  = 5000.0f;
    };
}
