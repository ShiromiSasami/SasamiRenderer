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
        float distance = 4.0f;
        float orthoHalf = 1.8f;
        float nearZ = 0.1f;
        float farZ = 10.0f;
        Color color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        float intensity = 1.0f;

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
            return out;
        }
    };
}
