#include "Renderer/Voxel/WindParticleImage.h"

#include "Renderer/Voxel/SdfSphereTracer.h"
#include "Renderer/Voxel/WindParticleSystem.h"
#include "Foundation/Math/MathUtil.h"

#include <algorithm>
#include <cmath>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        constexpr float MIN_VIEW_DEPTH = 0.05f;
        constexpr float DEPTH_BIAS = 0.02f;
        constexpr float MAX_TRAIL_FADE = 0.85f;
        constexpr float MAX_SEGMENT_STEPS = 4096.0f;

        struct ScreenPoint
        {
            float column = 0.0f;
            float row = 0.0f;
            float dist = 0.0f;
        };

        using Foundation::Math::Dot3;

        bool ProjectPoint(const SdfTraceCamera& camera, uint32_t width, uint32_t height, const float p[3],
                          ScreenPoint& out)
        {
            const float rel[3] = { p[0] - camera.position[0], p[1] - camera.position[1], p[2] - camera.position[2] };
            const float z = Dot3(rel, camera.forward);
            if (z < MIN_VIEW_DEPTH) {
                return false;
            }
            const float sx = Dot3(rel, camera.right) / (z * camera.tanHalfFovY * camera.aspectRatio);
            const float sy = Dot3(rel, camera.up) / (z * camera.tanHalfFovY);
            out.column = (sx + 1.0f) * 0.5f * static_cast<float>(width) - 0.5f;
            out.row = (1.0f - sy) * 0.5f * static_cast<float>(height) - 0.5f;
            out.dist = std::sqrt(Dot3(rel, rel));
            return std::isfinite(out.column) && std::isfinite(out.row);
        }

        void SpeedColor(float speed, float referenceSpeed, float outBgr[3])
        {
            const float s = referenceSpeed > 0.0f ? std::clamp(speed / referenceSpeed, 0.0f, 2.0f) : 1.0f;
            static const float BLUE[3] = { 255.0f, 110.0f, 40.0f };
            static const float WHITE[3] = { 255.0f, 255.0f, 255.0f };
            static const float YELLOW[3] = { 40.0f, 230.0f, 255.0f };
            const float* from = s <= 1.0f ? BLUE : WHITE;
            const float* to = s <= 1.0f ? WHITE : YELLOW;
            const float t = s <= 1.0f ? s : s - 1.0f;
            for (int c = 0; c < 3; ++c) {
                outBgr[c] = from[c] + (to[c] - from[c]) * t;
            }
        }
    }

    uint64_t DrawWindParticleTrails(const WindParticleSystem& particles, const SdfTraceCamera& camera,
                                    uint32_t width, uint32_t height, const std::vector<float>& depth,
                                    float referenceSpeed, std::vector<uint8_t>& inOutBgra)
    {
        const size_t pixelCount = static_cast<size_t>(width) * height;
        if (width == 0 || height == 0 || depth.size() < pixelCount || inOutBgra.size() < pixelCount * 4) {
            return 0;
        }

        uint64_t written = 0;
        for (uint32_t i = 0; i < particles.Count(); ++i) {
            const uint32_t trailCount = particles.TrailCount(i);
            if (trailCount < 2) {
                continue;
            }
            float color[3];
            SpeedColor(particles.Speed(i), referenceSpeed, color);

            for (uint32_t k = 0; k + 1 < trailCount; ++k) {
                ScreenPoint a;
                ScreenPoint b;
                if (!ProjectPoint(camera, width, height, particles.TrailPosition(i, k), a) ||
                    !ProjectPoint(camera, width, height, particles.TrailPosition(i, k + 1), b)) {
                    continue;
                }
                const float alpha = 1.0f - (static_cast<float>(k) / static_cast<float>(WindParticleSystem::TRAIL_LENGTH)) * MAX_TRAIL_FADE;

                const float dx = b.column - a.column;
                const float dy = b.row - a.row;
                const float steps = std::min(std::ceil(std::max(std::fabs(dx), std::fabs(dy))), MAX_SEGMENT_STEPS);
                const uint32_t stepCount = static_cast<uint32_t>(steps);
                for (uint32_t n = 0; n <= stepCount; ++n) {
                    const float t = stepCount > 0 ? static_cast<float>(n) / static_cast<float>(stepCount) : 0.0f;
                    const int column = static_cast<int>(std::lround(a.column + dx * t));
                    const int row = static_cast<int>(std::lround(a.row + dy * t));
                    if (column < 0 || row < 0 || column >= static_cast<int>(width) || row >= static_cast<int>(height)) {
                        continue;
                    }
                    const size_t index = static_cast<size_t>(row) * width + static_cast<size_t>(column);
                    const float dist = a.dist + (b.dist - a.dist) * t;
                    if (!(dist < depth[index] - DEPTH_BIAS)) {
                        continue;
                    }
                    uint8_t* pixel = inOutBgra.data() + index * 4;
                    for (int c = 0; c < 3; ++c) {
                        const float old = static_cast<float>(pixel[c]);
                        const float blended = old + (color[c] - old) * alpha;
                        pixel[c] = static_cast<uint8_t>(std::clamp(blended + 0.5f, 0.0f, 255.0f));
                    }
                    pixel[3] = 255;
                    ++written;
                }
            }
        }
        return written;
    }
}
