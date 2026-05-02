#pragma once

#include "Foundation/Math/Color.h"
#include "Renderer/Scene/RenderLightProxy.h"

namespace SasamiRenderer
{
    class DirectionalLight
    {
    public:
        float yaw = 0.7f;
        float pitch = 1.0f;
        float distance = 50.0f;
        float orthoHalf = 12.0f;
        float nearZ = 0.1f;
        float farZ = 10.0f;
        Color color = Color(1.0f, 1.0f, 1.0f, 1.0f);
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

        RenderDirectionalLight ToRenderLight() const
        {
            RenderDirectionalLight out{};
            out.yaw = yaw;
            out.pitch = pitch;
            out.distance = distance;
            out.orthoHalf = orthoHalf;
            out.nearZ = nearZ;
            out.farZ = farZ;
            out.color[0] = color.r;
            out.color[1] = color.g;
            out.color[2] = color.b;
            out.intensity = intensity;
            out.shadowMode = shadowMode;
            out.shadowDistance = shadowDistance;
            out.cascadeDistributionExponent = cascadeDistributionExponent;
            out.cascadeBlendFraction = cascadeBlendFraction;
            out.depthRangeMode = depthRangeMode;
            out.depthBias = depthBias;
            out.slopeScaleBias = slopeScaleBias;
            out.normalBias = normalBias;
            out.farBiasScale = farBiasScale;
            return out;
        }

        static DirectionalLight FromRenderLight(const RenderDirectionalLight& in)
        {
            DirectionalLight out{};
            out.yaw = in.yaw;
            out.pitch = in.pitch;
            out.distance = in.distance;
            out.orthoHalf = in.orthoHalf;
            out.nearZ = in.nearZ;
            out.farZ = in.farZ;
            out.color = Color(in.color[0], in.color[1], in.color[2], 1.0f);
            out.intensity = in.intensity;
            out.shadowMode = in.shadowMode;
            out.shadowDistance = in.shadowDistance;
            out.cascadeDistributionExponent = in.cascadeDistributionExponent;
            out.cascadeBlendFraction = in.cascadeBlendFraction;
            out.depthRangeMode = in.depthRangeMode;
            out.depthBias = in.depthBias;
            out.slopeScaleBias = in.slopeScaleBias;
            out.normalBias = in.normalBias;
            out.farBiasScale = in.farBiasScale;
            return out;
        }
    };
}
