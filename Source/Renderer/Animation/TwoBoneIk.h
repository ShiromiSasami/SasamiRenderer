#pragma once
#include "Renderer/Animation/AnimMath.h"

// Analytic two-bone IK (law of cosines + pole vector), following ozz-animation's IKTwoBoneJob
// (ozz/animation/runtime/ik_two_bone_job.cc, MIT). All inputs are in model space.
namespace SasamiEngine::Render::Anim
{
    struct TwoBoneIkInput
    {
        Mat4 startJoint;            // e.g. thigh / upper arm, model space
        Mat4 midJoint;              // knee / elbow
        Mat4 endJoint;              // ankle / wrist
        Vec3 target{};              // model space position for the end joint
        Vec3 pole{ 0.0f, 1.0f, 0.0f }; // model space direction the chain plane should face
        Vec3 midAxis{ 0.0f, 0.0f, 1.0f }; // bend axis in the mid joint's local space (unit)
        float twistAngle = 0.0f;    // extra rotation of the chain around the start->target axis
        float soften = 1.0f;        // (0,1]: 1 = no softening; 0.97 avoids the knee "pop" near full reach
        float weight = 1.0f;        // [0,1], nlerp from identity
    };

    struct TwoBoneIkResult
    {
        Quat startCorrection{};     // post-multiply into the start joint's local rotation
        Quat midCorrection{};       // post-multiply into the mid joint's local rotation
        bool reached = false;       // target within the (softened) reach and weight >= 1
    };

    TwoBoneIkResult SolveTwoBoneIk(const TwoBoneIkInput& input);

    // localRotation = normalize(localRotation * correction).
    void ApplyJointCorrection(Quat& localRotation, const Quat& correction);

    // Bend axis of a chain in the mid joint's local space: cross(mid - start, end - mid), or the cross
    // with `fallbackModel` when the chain is straight. Returns +Z when everything degenerates.
    Vec3 ComputeMidAxis(const Mat4& startJoint, const Mat4& midJoint, const Mat4& endJoint,
                        const Vec3& fallbackModel = { 1.0f, 0.0f, 0.0f });
}

namespace SasamiEngine { using namespace Render; }
