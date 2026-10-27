#pragma once
#include "Renderer/Animation/AnimPose.h"
#include "Renderer/Structures/Skeleton.h"
#include "Renderer/Structures/SkeletonAnimation.h"

namespace SasamiEngine::Render::Anim
{
    // Bind-pose local transforms (Skeleton::bindLocalT/R/S), boneCount entries.
    LocalPose MakeBindPose(const Skeleton& skeleton);

    // Looping clips wrap into [0, duration); one-shot clips clamp into [0, duration].
    // A non-positive duration returns 0.
    float WrapClipTime(float time, float durationSec, bool loop);

    // Samples `clip` at `time` (seconds, not wrapped here) into `inOut`.
    // Translation/scale are lerped, rotation is slerped along the shortest path, times outside the
    // key range clamp to the first/last key (same as AnimationController::SampleVec3/SampleQuat).
    // A channel the clip does not drive keeps the value already in `inOut`, so pass a bind pose
    // (MakeBindPose) to get AnimationController's fallback behaviour.
    // `inOut.size()` must be >= the number of bones that have tracks; extra tracks are ignored.
    void SampleClip(const SkeletonAnimation& clip, float time, LocalPose& inOut);
}

namespace SasamiEngine { using namespace Render; }
