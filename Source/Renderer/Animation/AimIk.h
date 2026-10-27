#pragma once
#include "Renderer/Animation/AnimPose.h"
#include "Renderer/Structures/Skeleton.h"

// Aim IK and a multi-joint look-at chain, following ozz-animation's IKAimJob
// (ozz/animation/runtime/ik_aim_job.cc, MIT) and its "look_at" sample.
namespace SasamiEngine::Render::Anim
{
    struct AimIkInput
    {
        Mat4 joint;                         // model space joint matrix
        Vec3 target{};                      // model space
        Vec3 forward{ 0.0f, 0.0f, 1.0f };   // joint local, unit: the axis that should point at target
        Vec3 offset{};                      // joint local: origin of the aim ray (e.g. eyes vs neck)
        Vec3 up{ 0.0f, 1.0f, 0.0f };        // joint local, unit
        Vec3 pole{ 0.0f, 1.0f, 0.0f };      // model space direction `up` should face
        float twistAngle = 0.0f;
        float weight = 1.0f;
    };

    struct AimIkResult
    {
        Quat correction{};                  // post-multiply into the joint's local rotation
        bool reached = false;               // false when the target lies inside the offset sphere
    };

    AimIkResult SolveAimIk(const AimIkInput& input);

    struct LookAtChain
    {
        std::vector<int> joints;            // head first, then towards the base (head, neck, spine2, ...)
        std::vector<float> jointWeights;    // same size as joints; the last joint always uses `weight`
        Vec3 forward{ 0.0f, 0.0f, 1.0f };   // head-joint local forward
        Vec3 offset{};                      // head-joint local eye offset
        Vec3 up{ 0.0f, 1.0f, 0.0f };        // head-joint local up
        Vec3 pole{ 0.0f, 1.0f, 0.0f };      // model space
        float weight = 1.0f;
    };

    // Rotates the chain joints in `pose` so the head forward aims at `targetModel`, spread over the chain.
    // `model` must be LocalToModel(pose) on entry; it is recomputed before returning.
    // Returns the head's reached flag.
    bool SolveLookAt(const Skeleton& skeleton, const LookAtChain& chain, const Vec3& targetModel,
                     LocalPose& pose, ModelPose& model);
}

namespace SasamiEngine { using namespace Render; }
