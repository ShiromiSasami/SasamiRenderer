#include "Renderer/Animation/BoneMask.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace SasamiEngine::Render::Anim
{
    namespace
    {
        constexpr size_t BONE_NAME_LEN = sizeof(Skeleton::boneName[0]);

        char ToLowerAscii(char c)
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }

        uint32_t ClampBoneCount(const Skeleton& skeleton)
        {
            return std::min(skeleton.boneCount, Skeleton::kMaxBones);
        }
    }

    int FindBone(const Skeleton& skeleton, const char* name)
    {
        if (!name)
            return -1;

        const uint32_t count = ClampBoneCount(skeleton);
        for (uint32_t i = 0; i < count; ++i)
        {
            if (std::strncmp(skeleton.boneName[i], name, BONE_NAME_LEN) == 0)
                return static_cast<int>(i);
        }
        return -1;
    }

    int FindBoneContaining(const Skeleton& skeleton, const char* substring)
    {
        if (!substring || !*substring)
            return -1;

        const size_t needleLen = std::strlen(substring);
        const uint32_t count = ClampBoneCount(skeleton);
        for (uint32_t i = 0; i < count; ++i)
        {
            const char* name = skeleton.boneName[i];
            const size_t nameLen = strnlen(name, BONE_NAME_LEN);
            if (needleLen > nameLen)
                continue;

            for (size_t start = 0; start + needleLen <= nameLen; ++start)
            {
                size_t k = 0;
                while (k < needleLen && ToLowerAscii(name[start + k]) == ToLowerAscii(substring[k]))
                    ++k;
                if (k == needleLen)
                    return static_cast<int>(i);
            }
        }
        return -1;
    }

    bool IsDescendantOf(const Skeleton& skeleton, int bone, int ancestor)
    {
        const int count = static_cast<int>(ClampBoneCount(skeleton));
        if (bone < 0 || bone >= count || ancestor < 0 || ancestor >= count)
            return false;

        // The step bound guards against malformed parent cycles.
        int current = bone;
        for (int step = 0; step <= count && current >= 0 && current < count; ++step)
        {
            if (current == ancestor)
                return true;
            current = skeleton.parentIndex[current];
        }
        return false;
    }

    BoneMask BuildSubtreeMask(const Skeleton& skeleton, int rootBone, float weight)
    {
        const int count = static_cast<int>(ClampBoneCount(skeleton));
        BoneMask mask(static_cast<size_t>(count), 0.0f);
        if (rootBone < 0 || rootBone >= count)
            return mask;

        std::vector<bool> inSubtree(static_cast<size_t>(count), false);
        for (int i = 0; i < count; ++i)
        {
            const int parent = skeleton.parentIndex[i];
            const bool in = i == rootBone || (parent >= 0 && parent < i && inSubtree[parent]);
            inSubtree[i] = in;
            if (in)
                mask[i] = weight;
        }
        return mask;
    }
}
