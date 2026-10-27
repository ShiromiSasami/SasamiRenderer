#pragma once

// Measures how large the loaded static geometry's triangles actually are.
//
// The shadow pass runs at 241 Mtri/s on an RTX 4070 Ti, an order of magnitude below the part.
// Fill rate, CPU submission, per-draw overhead, bandwidth, a depth-only VS and vertex cache
// locality have each been eliminated by measurement (see TODO.md). The one candidate left is
// rasterizer efficiency on tiny triangles: a triangle smaller than a shadow-map texel still
// costs a full setup and at least one quad, so a mesh of sub-texel triangles processes at the
// triangle setup rate no matter how few pixels it covers.
//
// That is a statement about triangle size, which is measurable on the CPU without a profiler,
// so this accumulates the distribution at load time instead of guessing. Opt-in via
// SASAMI_TRIANGLE_STATS=1 because walking every triangle of a 6.6M-triangle scene is startup
// cost that nothing else needs.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "Renderer/Structures/Mesh.h"

namespace SasamiEngine::App
{
    inline bool IsTriangleStatsEnabled()
    {
        static const bool enabled = [] {
            char value[8] = {};
            const DWORD length = GetEnvironmentVariableA("SASAMI_TRIANGLE_STATS", value, sizeof(value));
            return length > 0 && length < sizeof(value) && value[0] == '1';
        }();
        return enabled;
    }

    // Areas of every triangle of every loaded static mesh, in world units squared. Kept as a raw
    // list rather than a histogram so the thresholds can be chosen after the fact -- the texel
    // footprint depends on cascade extents that are not known at load time.
    inline std::vector<float>& TriangleAreaSamples()
    {
        static std::vector<float> areas;
        return areas;
    }

    inline void AccumulateTriangleAreas(const Mesh& mesh, float uniformScale)
    {
        const size_t indexCount = mesh.indices.size();
        if (indexCount < 3) {
            return;
        }
        // Area scales with the square of a uniform scale; loaders that already baked the scale
        // into the vertices pass 1.0 here.
        const float areaScale = uniformScale * uniformScale;

        std::vector<float>& areas = TriangleAreaSamples();
        areas.reserve(areas.size() + indexCount / 3);
        for (size_t i = 0; i + 2 < indexCount; i += 3) {
            const Vertex& a = mesh.vertices[mesh.indices[i]];
            const Vertex& b = mesh.vertices[mesh.indices[i + 1]];
            const Vertex& c = mesh.vertices[mesh.indices[i + 2]];
            const float ux = b.position[0] - a.position[0];
            const float uy = b.position[1] - a.position[1];
            const float uz = b.position[2] - a.position[2];
            const float vx = c.position[0] - a.position[0];
            const float vy = c.position[1] - a.position[1];
            const float vz = c.position[2] - a.position[2];
            const float cx = uy * vz - uz * vy;
            const float cy = uz * vx - ux * vz;
            const float cz = ux * vy - uy * vx;
            areas.push_back(0.5f * std::sqrt(cx * cx + cy * cy + cz * cz) * areaScale);
        }
    }

    // Percentiles plus the share of triangles below a set of world-space area thresholds. The
    // caller supplies the thresholds because "one texel" is a property of the cascade, not of
    // the mesh.
    inline std::string FormatTriangleAreaReport(const std::vector<float>& texelAreas)
    {
        std::vector<float> areas = TriangleAreaSamples();
        if (areas.empty()) {
            return "ERR no triangle samples (relaunch with SASAMI_TRIANGLE_STATS=1)";
        }
        std::sort(areas.begin(), areas.end());
        const size_t n = areas.size();
        const auto pct = [&](double p) { return areas[std::min<size_t>(n - 1, static_cast<size_t>(n * p))]; };

        std::string out = "OK triangles=" + std::to_string(n);
        out += " areaP01=" + std::to_string(pct(0.01));
        out += " areaP10=" + std::to_string(pct(0.10));
        out += " areaP50=" + std::to_string(pct(0.50));
        out += " areaP90=" + std::to_string(pct(0.90));
        out += " areaP99=" + std::to_string(pct(0.99));
        // Equivalent edge length of the median triangle, easier to reason about than an area.
        out += " medianEdge=" + std::to_string(std::sqrt(2.0f * pct(0.50)));
        for (float texelArea : texelAreas) {
            const size_t below = static_cast<size_t>(
                std::lower_bound(areas.begin(), areas.end(), texelArea) - areas.begin());
            out += " below_" + std::to_string(texelArea) + "=" +
                   std::to_string(static_cast<double>(below) / static_cast<double>(n) * 100.0) + "%";
        }
        return out;
    }
}
namespace SasamiEngine { using namespace App; }
