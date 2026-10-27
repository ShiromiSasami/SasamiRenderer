#include "Renderer/Culling/HzbPyramid.h"

#include <algorithm>
#include <cmath>

namespace SasamiEngine::Render::Culling
{
    void HzbPyramid::Build(const float* depth, uint32_t width, uint32_t height)
    {
        m_levels.clear();
        if (depth == nullptr || width == 0 || height == 0) {
            return;
        }

        Level base;
        base.width = width;
        base.height = height;
        base.texels.assign(depth, depth + static_cast<size_t>(width) * height);
        m_levels.push_back(std::move(base));

        while (m_levels.back().width > 1 || m_levels.back().height > 1) {
            const Level& prev = m_levels.back();
            Level next;
            next.width = std::max(1u, prev.width / 2);
            next.height = std::max(1u, prev.height / 2);
            next.texels.resize(static_cast<size_t>(next.width) * next.height);
            for (uint32_t y = 0; y < next.height; ++y) {
                const uint32_t ys = 2 * y;
                const uint32_t ye = ys + ((prev.height & 1u) && y == next.height - 1 ? 2u : 1u);
                for (uint32_t x = 0; x < next.width; ++x) {
                    const uint32_t xs = 2 * x;
                    const uint32_t xe = xs + ((prev.width & 1u) && x == next.width - 1 ? 2u : 1u);
                    float result = prev.texels[std::min(ys, prev.height - 1) * prev.width + std::min(xs, prev.width - 1)];
                    for (uint32_t sy = ys; sy <= ye; ++sy) {
                        const uint32_t ry = std::min(sy, prev.height - 1);
                        for (uint32_t sx = xs; sx <= xe; ++sx) {
                            const uint32_t rx = std::min(sx, prev.width - 1);
                            result = std::min(result, prev.texels[ry * prev.width + rx]);
                        }
                    }
                    next.texels[static_cast<size_t>(y) * next.width + x] = result;
                }
            }
            m_levels.push_back(std::move(next));
        }
    }

    size_t HzbPyramid::ByteSize() const
    {
        size_t bytes = 0;
        for (const Level& level : m_levels) {
            bytes += level.texels.size() * sizeof(float);
        }
        return bytes;
    }

    float HzbPyramid::FootprintMin(const HzbRect& rect, uint32_t* outLevel) const
    {
        if (m_levels.empty()) {
            if (outLevel != nullptr) {
                *outLevel = 0;
            }
            return 0.0f;
        }

        uint32_t level = 0;
        uint32_t x0 = std::min(rect.x0, m_levels[0].width - 1);
        uint32_t y0 = std::min(rect.y0, m_levels[0].height - 1);
        uint32_t x1 = std::min(std::max(rect.x1, x0), m_levels[0].width - 1);
        uint32_t y1 = std::min(std::max(rect.y1, y0), m_levels[0].height - 1);
        while ((x1 - x0 > 1 || y1 - y0 > 1) && level + 1 < LevelCount()) {
            ++level;
            const Level& l = m_levels[level];
            // The last texel of an odd level absorbs three sources, so the low corner clamps too.
            x0 = std::min(x0 / 2, l.width - 1);
            y0 = std::min(y0 / 2, l.height - 1);
            x1 = std::min(x1 / 2, l.width - 1);
            y1 = std::min(y1 / 2, l.height - 1);
        }

        if (outLevel != nullptr) {
            *outLevel = level;
        }
        float result = Texel(level, x0, y0);
        for (uint32_t y = y0; y <= y1; ++y) {
            for (uint32_t x = x0; x <= x1; ++x) {
                result = std::min(result, Texel(level, x, y));
            }
        }
        return result;
    }

    bool HzbPyramid::IsOccluded(const HzbRect& rect, float closestDepth) const
    {
        return closestDepth < FootprintMin(rect);
    }

    HzbRect HzbPyramid::RectFromUv(float u0, float v0, float u1, float v1, uint32_t width, uint32_t height)
    {
        auto toTexel = [](float uv, uint32_t size) {
            const float scaled = std::floor(uv * static_cast<float>(size));
            if (!(scaled > 0.0f)) {
                return 0u;
            }
            return std::min(static_cast<uint32_t>(std::min(scaled, static_cast<float>(size))), size - 1);
        };

        HzbRect rect;
        rect.x0 = toTexel(std::min(u0, u1), width);
        rect.y0 = toTexel(std::min(v0, v1), height);
        rect.x1 = toTexel(std::max(u0, u1), width);
        rect.y1 = toTexel(std::max(v0, v1), height);
        return rect;
    }
}
