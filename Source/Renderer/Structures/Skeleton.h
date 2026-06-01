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
    };
}
