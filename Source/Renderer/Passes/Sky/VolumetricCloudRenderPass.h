#pragma once

#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

namespace SasamiRenderer
{
    // Volumetric cloud render pass.
    //
    // Renders a raymarched cloud layer over the existing sky background using
    // path-tracing style single scattering (Beer-Lambert + Henyey-Greenstein).
    // Draws a fullscreen triangle at NDC z=1 with alpha blending and a
    // depth test (LESS_EQUAL, no write) so it composites only on sky pixels.
    //
    // Parameters (adjustable at runtime via setters):
    //   cloudCover     Efraction of sky covered [0..1]  (default 0.45)
    //   cloudDensity   Eoverall optical thickness scale  (default 2.0)
    //   windSpeed      Eanimation speed in m/s           (default 8.0)
    //   cloudBaseAlt   Ecloud layer base altitude in m   (default 1500)
    //   cloudTopAlt    Ecloud layer top  altitude in m   (default 5000)
    class VolumetricCloudRenderPass : public IRenderPass
    {
    public:
        std::string_view Tag()      const override { return "VolumetricCloud"; }
        std::string_view PhaseTag() const override { return "Scene"; }

        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;
        void Setup(RenderGraphBuilder& builder) const override;
        bool Execute(const RenderPassContextView& context) const override;

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
