#pragma once
#include <string>
#include <vector>

namespace SasamiRenderer
{
    // Single keyframe for one component (translation xyz, rotation xyzw, or scale xyz)
    struct AnimKeyframe
    {
        float time;
        float value[4]; // xyz for T/S; xyzw quaternion for R
    };

    struct BoneTrack
    {
        std::vector<AnimKeyframe> translation; // xyz
        std::vector<AnimKeyframe> rotation;    // quaternion xyzw
        std::vector<AnimKeyframe> scale;       // xyz
    };

    struct SkeletonAnimation
    {
        std::string           name;
        float                 durationSec = 0.0f;
        std::vector<BoneTrack> boneTracks; // one entry per bone (indexed by bone index)
    };
}
