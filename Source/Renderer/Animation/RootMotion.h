#pragma once
#include "Renderer/Animation/AnimPose.h"
#include "Renderer/Structures/Skeleton.h"
#include "Renderer/Structures/SkeletonAnimation.h"

// Root motion: the clip's own root-bone travel drives the character, instead of a script speed,
// so feet do not slide (UE "Root Motion", Unity "Apply Root Motion").
// The model is Y-up with +Z forward; motion is reported in the character's heading frame.
namespace SasamiEngine::Render::Anim
{
    struct RootMotionDelta
    {
        Vec3 translation{};   // in the heading frame at the start of the window (x right, z forward)
        float yaw = 0.0f;     // heading change, radians, positive = turn towards +X
    };

    // First root bone (parentIndex < 0) that has children, else bone 0. -1 for an empty skeleton.
    int FindRootMotionBone(const Skeleton& skeleton);

    // Root-bone motion from t0 to t1 (seconds, already advanced; t1 < t0 is not allowed for one-shot clips).
    // Looping clips: the window may cross the end any number of times, e.g. t0 = 0.9, t1 = 2.3 with a
    // 1 s clip composes [0.9,1) + one full cycle + [0,0.3). `horizontalOnly` zeroes translation.y.
    RootMotionDelta ExtractRootMotion(const SkeletonAnimation& clip, const Skeleton& skeleton, int rootBone,
                                      float t0, float t1, bool loop, bool horizontalOnly = true);

    // Removes the motion that ExtractRootMotion reports from the root bone of `pose`:
    // root XZ translation is reset to `reference` (vertical bob stays), and the root yaw is replaced
    // by the reference yaw while pitch/roll stay. Use the bind pose of the root as `reference`.
    void StripRootMotion(LocalPose& pose, int rootBone, const Transform& reference, bool keepVertical = true);

    // worldPosition += rotateYaw(worldYaw) * delta.translation * scale; worldYaw += delta.yaw.
    // `scale` converts skeleton units to world units (e.g. the model's uniform scale).
    void ApplyRootMotion(Vec3& worldPosition, float& worldYaw, const RootMotionDelta& delta, float scale = 1.0f);

    // Accumulates weighted deltas (translation and yaw summed with weights). Used to merge the root
    // motion of several blended clips.
    RootMotionDelta BlendRootMotion(const RootMotionDelta& a, const RootMotionDelta& b, float t);
}

namespace SasamiEngine { using namespace Render; }
