#include "Renderer/Toon/FaceShadowThresholdMap.h"

#include "Renderer/Voxel/VoxelDistanceTransform.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace SasamiEngine::Render::Toon
{
    namespace
    {
        constexpr float NO_FEATURE_DISTANCE = 1.0e6f;
        constexpr float FRACTION_MARGIN = 1.0e-4f;
        constexpr uint8_t NEVER_LIT = 255;

        // Euclidean distance, in texels, from every texel to the nearest texel whose lit flag equals `featureLit`.
        void DistanceToState(const FaceShadowMask& mask, bool featureLit, std::vector<float>& distance)
        {
            const float infinity = Voxel::SquaredDistanceInfinity();
            distance.assign(mask.lit.size(), infinity);
            for (size_t i = 0; i < mask.lit.size(); ++i) {
                if ((mask.lit[i] != 0) == featureLit) {
                    distance[i] = 0.0f;
                }
            }
            Voxel::SquaredDistanceTransform3D(distance, mask.width, mask.height, 1);
            for (float& d : distance) {
                d = (d >= infinity * 0.5f) ? NO_FEATURE_DISTANCE : std::sqrt(d);
            }
        }

        bool Fail(std::string* error, const std::string& message)
        {
            if (error) {
                *error = message;
            }
            return false;
        }
    }

    bool BuildFaceShadowThresholdMap(const std::vector<FaceShadowMask>& masks, std::vector<float>& outThreshold, std::string* error)
    {
        outThreshold.clear();
        const size_t count = masks.size();
        if (count < 2 || count > 254) {
            return Fail(error, "need 2..254 masks, got " + std::to_string(count));
        }
        const uint32_t width = masks[0].width;
        const uint32_t height = masks[0].height;
        if (width == 0 || height == 0) {
            return Fail(error, "mask 0 has zero size");
        }
        const size_t texelCount = static_cast<size_t>(width) * height;
        for (size_t k = 0; k < count; ++k) {
            if (masks[k].width != width || masks[k].height != height || masks[k].lit.size() != texelCount) {
                return Fail(error, "mask " + std::to_string(k) + " size differs from mask 0 (" + std::to_string(width) + "x" + std::to_string(height) + ")");
            }
        }

        for (size_t k = 0; k + 1 < count; ++k) {
            for (size_t i = 0; i < texelCount; ++i) {
                if (masks[k].lit[i] != 0 && masks[k + 1].lit[i] == 0) {
                    std::ostringstream message;
                    message << "masks not nested: texel (" << (i % width) << "," << (i / width) << ") is lit in mask " << k
                            << " but unlit in mask " << (k + 1);
                    return Fail(error, message.str());
                }
            }
        }

        std::vector<uint8_t> firstLit(texelCount, NEVER_LIT);
        for (size_t k = 0; k < count; ++k) {
            for (size_t i = 0; i < texelCount; ++i) {
                if (masks[k].lit[i] != 0 && firstLit[i] == NEVER_LIT) {
                    firstLit[i] = static_cast<uint8_t>(k);
                }
            }
        }

        outThreshold.assign(texelCount, 1.0f);
        for (size_t i = 0; i < texelCount; ++i) {
            if (firstLit[i] == 0) {
                outThreshold[i] = 0.0f;
            }
        }

        const float levelScale = static_cast<float>(count - 1);
        std::vector<float> distToLit;
        std::vector<float> distToUnlit;
        for (size_t k = 0; k + 1 < count; ++k) {
            const uint8_t target = static_cast<uint8_t>(k + 1);
            if (std::find(firstLit.begin(), firstLit.end(), target) == firstLit.end()) {
                continue;
            }
            DistanceToState(masks[k], true, distToLit);
            DistanceToState(masks[k + 1], false, distToUnlit);
            for (size_t i = 0; i < texelCount; ++i) {
                if (firstLit[i] != target) {
                    continue;
                }
                const float a = distToLit[i];
                const float b = distToUnlit[i];
                const float fraction = std::clamp(a / (a + b), FRACTION_MARGIN, 1.0f - FRACTION_MARGIN);
                outThreshold[i] = (static_cast<float>(k) + fraction) / levelScale;
            }
        }
        return true;
    }

    float SampleFaceShadow(float threshold, float lightParam01, float softness)
    {
        if (threshold >= 1.0f) {
            return 0.0f;
        }
        if (softness <= 0.0f) {
            return lightParam01 >= threshold ? 1.0f : 0.0f;
        }
        const float t = std::clamp((lightParam01 - (threshold - softness)) / (2.0f * softness), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
}
