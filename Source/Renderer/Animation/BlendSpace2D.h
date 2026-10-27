#pragma once
#include "Renderer/Animation/AnimPose.h"
#include "Renderer/Animation/RootMotion.h"
#include "Renderer/Structures/Skeleton.h"
#include "Renderer/Structures/SkeletonAnimation.h"

#include <vector>

// 2D blend space over freely placed samples (Unity "2D Freeform Cartesian / Directional" blend trees,
// UE "Blend Space"). Weights come from gradient band interpolation (R. S. Johansen, "Automated
// Semi-Procedural Animation for Character Locomotion", MSc thesis 2009, sec. 6.3): for every sample
// i, h_i(p) = min over j != i of clamp01(1 - dot(p - p_i, p_j - p_i) / |p_j - p_i|^2), then the
// h_i are normalized to sum 1. Directional mode measures the same thing in polar space (magnitude
// difference plus angle difference scaled by DIRECTIONAL_ANGLE_WEIGHT), which is what velocity-driven
// locomotion wants: a sample at (0,0) acts as the idle centre and directions blend by angle.
// Clips are phase-synchronized like BlendSpace1D.
namespace SasamiEngine::Render::Anim
{
    enum class BlendSpace2DMode
    {
        FreeformCartesian,
        FreeformDirectional,
    };

    struct BlendSpace2DSample
    {
        const SkeletonAnimation* clip = nullptr;
        float x = 0.0f;
        float y = 0.0f;
    };

    class BlendSpace2D
    {
    public:
        // Johansen's polar variant scales the angle term so that 1 radian weighs like this much magnitude.
        static constexpr float DIRECTIONAL_ANGLE_WEIGHT = 2.0f;

        // `clip` must outlive the blend space. Samples keep insertion order.
        void AddSample(const SkeletonAnimation* clip, float x, float y);
        const std::vector<BlendSpace2DSample>& Samples() const { return m_samples; }

        void SetMode(BlendSpace2DMode mode) { m_mode = mode; }
        BlendSpace2DMode Mode() const { return m_mode; }

        // One weight per sample, each in [0, 1], sum 1 (a single sample gets 1; no samples -> empty).
        // At a sample's position that sample gets weight 1. Weights are continuous in (x, y).
        void ComputeWeights(float x, float y, std::vector<float>& outWeights) const;

        // Weighted mean of the sample durations.
        float BlendedDuration(float x, float y) const;

        // Same contract as BlendSpace1D::Evaluate: all weighted clips sampled at the same normalized phase.
        void Evaluate(float x, float y, float phase, const LocalPose& bindPose, LocalPose& out) const;

        // Weighted root motion from phase0 to phase1 (normalized, phase1 may exceed 1 to wrap).
        RootMotionDelta EvaluateRootMotion(float x, float y, float phase0, float phase1, const Skeleton& skeleton,
                                           int rootBone) const;

    private:
        std::vector<BlendSpace2DSample> m_samples;
        BlendSpace2DMode m_mode = BlendSpace2DMode::FreeformCartesian;
        mutable LocalPose m_scratch;
        mutable std::vector<float> m_weights;
    };
}

namespace SasamiEngine { using namespace Render; }
