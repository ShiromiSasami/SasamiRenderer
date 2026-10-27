#pragma once
#include "Renderer/Animation/AnimPose.h"

namespace SasamiEngine::Render::Anim
{
    // All functions here work bone by bone, so `out` may alias any input.
    // Rotations use shortest-path nlerp (ozz/UE style; cheap and commutative enough for blending).

    // out = lerp(a, b, t) per bone. Sizes must match; `out` is resized.
    void BlendPoses(const LocalPose& a, const LocalPose& b, float t, LocalPose& out);

    // Same as BlendPoses with a per-bone weight t * mask[i] (layered / upper-body blending).
    // Bones past mask.size() use weight 0 (keep `a`).
    void BlendPosesMasked(const LocalPose& a, const LocalPose& b, const BoneMask& mask, float t, LocalPose& out);

    // Additive delta of `pose` relative to `reference` (UE "local space additive"):
    //   t = pose.t - ref.t,  r = conj(ref.r) * pose.r,  s = pose.s / ref.s (component-wise, 0 -> 1).
    void MakeAdditivePose(const LocalPose& pose, const LocalPose& reference, LocalPose& outDelta);

    // out = base (+) weight * delta:
    //   t = base.t + delta.t * w,  r = normalize(base.r * nlerp(identity, delta.r, w)),
    //   s = base.s * lerp(1, delta.s, w).
    void ApplyAdditivePose(const LocalPose& base, const LocalPose& delta, float weight, LocalPose& out);
    // Per-bone weight = weight * mask[i]; bones past mask.size() take no additive.
    void ApplyAdditivePoseMasked(const LocalPose& base, const LocalPose& delta, const BoneMask& mask, float weight,
                                 LocalPose& out);
}

namespace SasamiEngine { using namespace Render; }
