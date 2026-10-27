#pragma once

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    struct SdfTraceCamera;
    class WindParticleSystem;

    // Draws particle trails as 1-pixel line segments into a BGRA8 image (top row first, as
    // RenderSdfTraceImage writes it). depth holds the per-pixel ray distance of the traced surface
    // (world units, +inf on a miss); a segment pixel is drawn only where it is nearer than that.
    // Colour runs blue -> white -> yellow with speed / referenceSpeed in [0, 2]; older trail
    // entries fade toward the background. Returns the number of pixels written.
    uint64_t DrawWindParticleTrails(const WindParticleSystem& particles, const SdfTraceCamera& camera,
                                    uint32_t width, uint32_t height, const std::vector<float>& depth,
                                    float referenceSpeed, std::vector<uint8_t>& inOutBgra);
}

namespace SasamiEngine { using namespace Render; }
