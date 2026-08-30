#pragma once
#include <cstdint>

namespace SasamiRenderer
{
    struct Skeleton
    {
        static constexpr uint32_t kMaxBones = 128;

        uint32_t boneCount = 0;
        int32_t  parentIndex[kMaxBones];           // -1 = root
        float    inverseBindPose[kMaxBones][16];   // column-major float4x4
        char     boneName[kMaxBones][64];

        // Bind-pose local TRS per bone, used as the fallback value for animation
        // channels the current clip doesn't drive (e.g. a bone with only a
        // rotation track keeps its bind-pose translation instead of snapping to 0).
        float    bindLocalT[kMaxBones][3];         // translation
        float    bindLocalR[kMaxBones][4];         // quaternion (x,y,z,w)
        float    bindLocalS[kMaxBones][3];         // scale
    };
}
