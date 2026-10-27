#include "AppFramework/Placement/InstanceScatter.h"

#include "AppFramework/Placement/PlacementRandom.h"
#include "AppFramework/Placement/PlacementTransform.h"

namespace SasamiEngine::App
{
    namespace
    {
        constexpr uint32_t kAttemptsPerInstance = 256;
        constexpr float kTwoPi = 6.28318530718f;
    }

    DensityScatterResult ScatterByDensity(const DensityScatterDesc& desc)
    {
        DensityScatterResult result;
        if (desc.rgba == nullptr || desc.width == 0 || desc.height == 0 || desc.count == 0) {
            return result;
        }

        const float sizeX = desc.maxX - desc.minX;
        const float sizeZ = desc.maxZ - desc.minZ;
        if (!(sizeX > 0.0f) || !(sizeZ > 0.0f)) {
            return result;
        }

        result.transforms.reserve(desc.count);
        PlacementRandom rng(desc.seed);
        const uint64_t maxAttempts = static_cast<uint64_t>(desc.count) * kAttemptsPerInstance;

        uint64_t attempts = 0;
        while (result.transforms.size() < desc.count && attempts < maxAttempts) {
            ++attempts;
            const float u = rng.NextUnit();
            const float v = rng.NextUnit();
            const float accept = rng.NextUnit();

            // Nearest texel; u, v < 1 keeps the index in range.
            const uint32_t px = static_cast<uint32_t>(u * static_cast<float>(desc.width));
            const uint32_t py = static_cast<uint32_t>(v * static_cast<float>(desc.height));
            const uint8_t density = desc.rgba[(static_cast<size_t>(py) * desc.width + px) * 4];
            if (accept * 255.0f >= static_cast<float>(density)) {
                continue;
            }

            // Drawn even when unused so the placement stream does not depend on these options.
            const float yawUnit = rng.NextUnit();
            const float scaleUnit = rng.NextUnit();
            const float yaw = desc.randomYaw ? yawUnit * kTwoPi : 0.0f;
            const float scale = desc.scaleMin + (desc.scaleMax - desc.scaleMin) * scaleUnit;

            result.transforms.push_back(
                MakeYawScaleTranslation(yaw, scale, desc.minX + u * sizeX, desc.y, desc.minZ + v * sizeZ));
        }

        result.attempts = static_cast<uint32_t>(attempts);
        return result;
    }
}
