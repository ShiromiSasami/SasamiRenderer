#pragma once
#include "Renderer/Animation/AnimPose.h"
#include "Renderer/Animation/RootMotion.h"
#include "Renderer/Structures/Skeleton.h"
#include "Renderer/Structures/SkeletonAnimation.h"

#include <vector>

// 1D blend space (UE "Blend Space 1D", Unity "1D Blend Tree"): clips placed on a parameter axis
// (e.g. speed), the two neighbours of the parameter blended linearly.
// Clips are phase-synchronized: every clip is sampled at the same normalized phase, so a walk and
// a run cycle keep their foot contacts aligned (UE "Sync Groups").
namespace SasamiEngine::Render::Anim
{
    struct BlendSpaceSample
    {
        const SkeletonAnimation* clip = nullptr;
        float position = 0.0f;
    };

    class BlendSpace1D
    {
    public:
        // Samples are kept sorted by position. `clip` must outlive the blend space.
        void AddSample(const SkeletonAnimation* clip, float position);
        const std::vector<BlendSpaceSample>& Samples() const { return m_samples; }

        // One weight per sample, sum 1, at most two non-zero; the parameter clamps to the axis ends.
        void ComputeWeights(float parameter, std::vector<float>& outWeights) const;

        // Weighted mean of the sample durations: the cycle length to advance the phase with.
        float BlendedDuration(float parameter) const;

        // Samples every weighted clip at phase * duration_i and blends them into `out`.
        // `phase` is normalized [0, 1). `bindPose` supplies undriven channels.
        void Evaluate(float parameter, float phase, const LocalPose& bindPose, LocalPose& out) const;

        // Weighted root motion from phase0 to phase1 (normalized, phase1 may exceed 1 to wrap).
        RootMotionDelta EvaluateRootMotion(float parameter, float phase0, float phase1, const Skeleton& skeleton,
                                           int rootBone) const;

    private:
        std::vector<BlendSpaceSample> m_samples;
        mutable LocalPose m_scratch;
        mutable std::vector<float> m_weights;
    };
}

namespace SasamiEngine { using namespace Render; }
