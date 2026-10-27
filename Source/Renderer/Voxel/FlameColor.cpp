#include "Renderer/Voxel/FlameColor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        constexpr float ROOT_KELVIN = 2600.0f;
        constexpr float TIP_KELVIN = 1000.0f;

        float Clamp255(float v)
        {
            return (std::min)((std::max)(v, 0.0f), 255.0f);
        }

        float Hash(int32_t x, int32_t y)
        {
            uint32_t h = static_cast<uint32_t>(x) * 0x8DA6B343u ^ static_cast<uint32_t>(y) * 0xD8163841u;
            h ^= h >> 13;
            h *= 0x85EBCA6Bu;
            h ^= h >> 16;
            return static_cast<float>(h >> 8) / 16777216.0f;
        }
    }

    void BlackbodySrgb(float kelvin, float outRgb[3])
    {
        const float t = (std::min)((std::max)(kelvin, 1000.0f), 40000.0f) / 100.0f;
        if (t <= 66.0f) {
            outRgb[0] = 255.0f;
            outRgb[1] = Clamp255(99.4708025861f * std::log(t) - 161.1195681661f);
        } else {
            outRgb[0] = Clamp255(329.698727446f * std::pow(t - 60.0f, -0.1332047592f));
            outRgb[1] = Clamp255(288.1221695283f * std::pow(t - 60.0f, -0.0755148492f));
        }
        if (t >= 66.0f) {
            outRgb[2] = 255.0f;
        } else if (t <= 19.0f) {
            outRgb[2] = 0.0f;
        } else {
            outRgb[2] = Clamp255(138.5177312231f * std::log(t - 10.0f) - 305.0447927307f);
        }
    }

    float FlameTemperature(float age01)
    {
        const float t = (std::min)((std::max)(age01, 0.0f), 1.0f);
        // Cools quickly at first, so most of the tongue is orange and only the root is yellow-white.
        const float cool = 1.0f - (1.0f - t) * (1.0f - t);
        return ROOT_KELVIN + (TIP_KELVIN - ROOT_KELVIN) * cool;
    }

    float ValueNoise2(float x, float y)
    {
        const float fx = std::floor(x);
        const float fy = std::floor(y);
        const int32_t ix = static_cast<int32_t>(fx);
        const int32_t iy = static_cast<int32_t>(fy);
        float u = x - fx;
        float v = y - fy;
        u = u * u * (3.0f - 2.0f * u);
        v = v * v * (3.0f - 2.0f * v);
        const float a = Hash(ix, iy);
        const float b = Hash(ix + 1, iy);
        const float c = Hash(ix, iy + 1);
        const float d = Hash(ix + 1, iy + 1);
        const float top = a + (b - a) * u;
        const float bottom = c + (d - c) * u;
        return top + (bottom - top) * v;
    }

    float FlameNoise(float x, float y)
    {
        const float n = 0.65f * ValueNoise2(x, y) + 0.35f * ValueNoise2(2.03f * x + 17.1f, 2.03f * y - 5.3f);
        return 2.0f * n - 1.0f;
    }
}
