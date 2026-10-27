#include "Renderer/Toon/FaceShadowSelfTest.h"

#include "Renderer/Toon/FaceShadowThresholdMap.h"

#include <cstdint>
#include <sstream>
#include <vector>

namespace SasamiEngine::Render::Toon
{
    namespace
    {
        FaceShadowMask MakeMask(uint32_t width, uint32_t height)
        {
            FaceShadowMask mask;
            mask.width = width;
            mask.height = height;
            mask.lit.assign(static_cast<size_t>(width) * height, 0);
            return mask;
        }

        // Half-plane x < edge[k], edges increasing.
        std::vector<FaceShadowMask> MakeHalfPlaneMasks(uint32_t size, uint32_t count, std::vector<float>& edges)
        {
            std::vector<FaceShadowMask> masks;
            edges.clear();
            for (uint32_t k = 0; k < count; ++k) {
                const float edge = (k + 1) * (static_cast<float>(size) / (count + 1)) + 0.37f;
                edges.push_back(edge);
                FaceShadowMask mask = MakeMask(size, size);
                for (uint32_t y = 0; y < size; ++y)
                    for (uint32_t x = 0; x < size; ++x)
                        mask.lit[static_cast<size_t>(y) * size + x] = (static_cast<float>(x) < edge) ? 1 : 0;
                masks.push_back(std::move(mask));
            }
            return masks;
        }

        // Ellipses with a shared off-centre centre and growing radii.
        std::vector<FaceShadowMask> MakeEllipseMasks(uint32_t width, uint32_t height, uint32_t count)
        {
            std::vector<FaceShadowMask> masks;
            const float cx = width * 0.42f;
            const float cy = height * 0.45f;
            for (uint32_t k = 0; k < count; ++k) {
                const float scale = 0.12f + 0.17f * k;
                const float rx = scale * width;
                const float ry = scale * height;
                FaceShadowMask mask = MakeMask(width, height);
                for (uint32_t y = 0; y < height; ++y) {
                    for (uint32_t x = 0; x < width; ++x) {
                        const float dx = (x - cx) / rx;
                        const float dy = (y - cy) / ry;
                        mask.lit[static_cast<size_t>(y) * width + x] = (dx * dx + dy * dy < 1.0f) ? 1 : 0;
                    }
                }
                masks.push_back(std::move(mask));
            }
            return masks;
        }

        // Number of (mask, texel) pairs where the map does not reproduce the mask.
        uint64_t CountReconstructionMismatches(const std::vector<FaceShadowMask>& masks, const std::vector<float>& threshold)
        {
            const size_t count = masks.size();
            uint64_t mismatches = 0;
            for (size_t k = 0; k < count; ++k) {
                const float level = static_cast<float>(k) / static_cast<float>(count - 1);
                for (size_t i = 0; i < threshold.size(); ++i) {
                    const bool reconstructed = (k + 1 < count) ? (threshold[i] <= level) : (threshold[i] < 1.0f);
                    if (reconstructed != (masks[k].lit[i] != 0)) {
                        ++mismatches;
                    }
                }
            }
            return mismatches;
        }
    }

    std::string RunFaceShadowSelfTest()
    {
        uint32_t checks = 0;
        uint64_t texelsChecked = 0;
        std::string firstError;
        auto check = [&](bool condition, const std::string& description)
        {
            ++checks;
            if (!condition && firstError.empty()) {
                firstError = description;
            }
        };

        // (a) + (b) Half-plane sweep.
        {
            const uint32_t size = 128;
            const uint32_t count = 8;
            std::vector<float> edges;
            const std::vector<FaceShadowMask> masks = MakeHalfPlaneMasks(size, count, edges);
            std::vector<float> threshold;
            std::string error;
            const bool built = BuildFaceShadowThresholdMap(masks, threshold, &error);
            check(built, "half-plane: build failed: " + error);
            if (built) {
                const uint64_t mismatches = CountReconstructionMismatches(masks, threshold);
                std::ostringstream detail;
                detail << "half-plane: " << mismatches << " reconstruction mismatches";
                check(mismatches == 0, detail.str());
                texelsChecked += static_cast<uint64_t>(size) * size * count;

                uint64_t nonMonotonic = 0;
                uint64_t notBetween = 0;
                for (uint32_t y = 0; y < size; ++y) {
                    for (uint32_t x = 0; x < size; ++x) {
                        const float t = threshold[static_cast<size_t>(y) * size + x];
                        if (x > 0 && t < threshold[static_cast<size_t>(y) * size + x - 1]) {
                            ++nonMonotonic;
                        }
                        // First lit in mask k+1 when edge[k] <= x < edge[k+1].
                        for (uint32_t k = 0; k + 1 < count; ++k) {
                            if (static_cast<float>(x) >= edges[k] && static_cast<float>(x) < edges[k + 1]) {
                                const float low = static_cast<float>(k) / (count - 1);
                                const float high = static_cast<float>(k + 1) / (count - 1);
                                if (!(t > low && t < high)) {
                                    ++notBetween;
                                }
                            }
                        }
                    }
                }
                check(nonMonotonic == 0, "half-plane: threshold must not decrease along x");
                check(notBetween == 0, "half-plane: texels between two edges must lie strictly between the levels");
                check(threshold[0] == 0.0f, "half-plane: texel lit in mask 0 reads 0");
                check(threshold[size - 1] == 1.0f, "half-plane: never-lit texel reads 1");
            }
        }

        // (c) Non-square nested ellipses.
        {
            const std::vector<FaceShadowMask> masks = MakeEllipseMasks(96, 64, 5);
            std::vector<float> threshold;
            std::string error;
            const bool built = BuildFaceShadowThresholdMap(masks, threshold, &error);
            check(built, "ellipses: build failed: " + error);
            if (built) {
                const uint64_t mismatches = CountReconstructionMismatches(masks, threshold);
                std::ostringstream detail;
                detail << "ellipses: " << mismatches << " reconstruction mismatches";
                check(mismatches == 0, detail.str());
                texelsChecked += 96ull * 64ull * 5ull;
            }
        }

        // (d) Error cases.
        {
            std::vector<float> edges;
            std::vector<float> threshold;
            std::string error;

            std::vector<FaceShadowMask> masks = MakeHalfPlaneMasks(16, 3, edges);
            std::swap(masks[0], masks[2]);
            check(!BuildFaceShadowThresholdMap(masks, threshold, &error) && !error.empty(), "non-nested masks must be rejected");

            masks = MakeHalfPlaneMasks(16, 3, edges);
            masks[1] = MakeMask(8, 16);
            error.clear();
            check(!BuildFaceShadowThresholdMap(masks, threshold, &error) && !error.empty(), "size mismatch must be rejected");

            masks = MakeHalfPlaneMasks(16, 1, edges);
            error.clear();
            check(!BuildFaceShadowThresholdMap(masks, threshold, &error) && !error.empty(), "fewer than two masks must be rejected");
            check(!BuildFaceShadowThresholdMap(masks, threshold, nullptr), "null error pointer is allowed");
        }

        // (e) SampleFaceShadow endpoints.
        {
            check(SampleFaceShadow(0.5f, 0.0f, 0.05f) == 0.0f, "sample: far below threshold is shadowed");
            check(SampleFaceShadow(0.5f, 1.0f, 0.05f) == 1.0f, "sample: far above threshold is lit");
            check(SampleFaceShadow(1.0f, 1.0f, 0.05f) == 0.0f, "sample: threshold 1 is always shadowed");
            check(SampleFaceShadow(0.5f, 0.5f, 0.0f) == 1.0f, "sample: hard step is lit at the threshold");
        }

        std::ostringstream out;
        if (firstError.empty()) {
            out << "OK checks=" << checks << " texelsChecked=" << texelsChecked;
        } else {
            out << "ERR " << firstError << " (checks=" << checks << ")";
        }
        return out.str();
    }
}
