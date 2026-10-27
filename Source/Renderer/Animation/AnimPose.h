#pragma once
#include "Renderer/Animation/AnimMath.h"

#include <vector>

namespace SasamiEngine::Render::Anim
{
    // One parent-relative transform per skeleton bone, indexed like Skeleton (parent before child).
    using LocalPose = std::vector<Transform>;
    // One model-space (skeleton-root-space) matrix per bone.
    using ModelPose = std::vector<Mat4>;
    // Per-bone weight in [0, 1], indexed like the skeleton. Used by masked blends and additives.
    using BoneMask = std::vector<float>;

    // Per-bone local velocities: linear in units/s, angular as scaled angle axis per second.
    struct PoseVelocity
    {
        std::vector<Vec3> linear;
        std::vector<Vec3> angular;
    };
}

namespace SasamiEngine { using namespace Render; }
