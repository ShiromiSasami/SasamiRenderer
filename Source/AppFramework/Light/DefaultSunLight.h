#pragma once

#include "Light/DirectionalLight.h"

namespace SasamiEngine::App
{
    // The warm afternoon sun the sample apps start with. Toon scenes pass a lower intensity.
    inline DirectionalLight MakeDefaultSunLight(float intensity = 2.5f)
    {
        DirectionalLight sun{};
        sun.yaw = 45.0f;
        sun.pitch = 40.0f;
        sun.intensity = intensity;
        sun.color.r = 1.00f;
        sun.color.g = 0.95f;
        sun.color.b = 0.85f;
        return sun;
    }
}
