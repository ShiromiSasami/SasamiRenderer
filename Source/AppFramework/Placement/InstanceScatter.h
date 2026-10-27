#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace SasamiEngine::App
{
    // Generates instance placements on the XZ plane from a density map. Pure data in, matrices out:
    // it knows nothing about models, scenes or files, so the result can be fed straight into
    // StaticModel::SetInstanceTransforms and baked by scene.save as an [instance] section.
    struct DensityScatterDesc
    {
        // RGBA8, row-major, top row first. Only R is read: 0 = never place, 255 = always accept.
        const uint8_t* rgba = nullptr;
        uint32_t width  = 0;
        uint32_t height = 0;

        // World-space rectangle the map covers. Image column 0 is minX, image row 0 is minZ.
        float minX = 0.0f;
        float minZ = 0.0f;
        float maxX = 1.0f;
        float maxZ = 1.0f;
        float y    = 0.0f; // fixed height; there is no terrain projection

        uint32_t count = 0;
        uint32_t seed  = 0;
        float scaleMin = 1.0f;
        float scaleMax = 1.0f;
        bool  randomYaw = true;
    };

    struct DensityScatterResult
    {
        using Transform = std::array<float, 16>;
        std::vector<Transform> transforms; // row-major, row-vector convention, translation in the last row
        uint32_t attempts = 0;
    };

    // Rejection sampling: a uniform point in the rectangle is kept with probability density/255.
    // Gives up after count * kAttemptsPerInstance attempts, so a mostly-black map returns fewer
    // than `count` placements rather than looping; callers compare transforms.size() with count.
    // Deterministic for a given desc: the random stream is a PCG hash chain seeded by `seed`.
    // No minimum spacing is enforced (placements may overlap).
    DensityScatterResult ScatterByDensity(const DensityScatterDesc& desc);
}
