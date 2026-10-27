#pragma once
#include "Renderer/Animation/AnimPose.h"
#include "Renderer/Structures/Skeleton.h"

namespace SasamiEngine::Render::Anim
{
    // Exact (case-sensitive) bone name lookup. -1 when absent.
    int FindBone(const Skeleton& skeleton, const char* name);
    // First bone whose name contains `substring`, compared case-insensitively. -1 when absent.
    // Rigs name bones differently ("b_Neck_04", "mixamorig:Neck"), so callers search by fragment.
    int FindBoneContaining(const Skeleton& skeleton, const char* substring);
    // True when `bone` is `ancestor` or below it.
    bool IsDescendantOf(const Skeleton& skeleton, int bone, int ancestor);

    // boneCount entries: `weight` for `rootBone` and all its descendants, 0 elsewhere.
    // An invalid `rootBone` gives an all-zero mask.
    BoneMask BuildSubtreeMask(const Skeleton& skeleton, int rootBone, float weight = 1.0f);
}

namespace SasamiEngine { using namespace Render; }
