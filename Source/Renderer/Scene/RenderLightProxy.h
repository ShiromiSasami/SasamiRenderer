#pragma once

#include <cstdint>

namespace SasamiEngine::Render
{
    enum class DirectionalShadowMode : uint32_t
    {
        Single = 0u,
        Csm4   = 1u,
        Vsm    = 2u,  // VSM 1 cascade
        Vsm4   = 3u,  // VSM 4 cascades
    };

    enum class DirectionalShadowDepthRangeMode : uint32_t
    {
        Stable    = 0u,
        Optimized = 1u,
    };

    struct RenderDirectionalLight
    {
        float yaw = 0.7f;
        float pitch = 1.0f;
        float distance = 15.0f;
        float orthoHalf = 8.0f;
        float nearZ = 0.1f;
        float farZ = 40.0f;
        float color[3] = { 1.0f, 1.0f, 1.0f };
        float intensity = 1.0f;
        DirectionalShadowMode shadowMode = DirectionalShadowMode::Csm4;
        float shadowDistance = 48.0f;
        float cascadeDistributionExponent = 2.0f;
        float cascadeBlendFraction = 0.1f;
        DirectionalShadowDepthRangeMode depthRangeMode = DirectionalShadowDepthRangeMode::Stable;
        float depthBias = 1000.0f;
        float slopeScaleBias = 2.0f;
        float normalBias = 0.015f;
        float farBiasScale = 1.35f;
        // Screen-space contact shadows are multiplied into the directional visibility term.
        // Exposed so the two can be separated when diagnosing shadow artefacts.
        bool  contactShadowEnabled = true;
    };

    struct RenderPointLight
    {
        float pos[3] = { 0.0f, 1.5f, 0.0f };
        float range = 5.0f;
        float color[3] = { 1.0f, 1.0f, 1.0f };
        float intensity = 1.0f;
    };

    struct RenderSpotLight
    {
        float pos[3] = { 0.0f, 2.0f, -2.0f };
        float range = 6.0f;
        float yaw = 0.0f;
        float pitch = -0.6f;
        float color[3] = { 1.0f, 1.0f, 1.0f };
        float intensity = 1.0f;
        float innerAngle = 0.261799f; // 15 deg
        float outerAngle = 0.436332f; // 25 deg
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
