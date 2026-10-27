#include "Renderer/Animation/TwoBoneIk.h"

#include <algorithm>
#include <cmath>

// Reference: ozz-animation IKTwoBoneJob (ozz/animation/runtime/ik_two_bone_job.cc, MIT).
// Written independently against the same algorithm: soften, law of cosines at the mid joint,
// start joint aim, then pole-plane alignment and twist around the start->target axis.
namespace SasamiEngine::Render::Anim
{
    namespace
    {
        // Rotation about unit `axis` that takes `from` to `to` (both perpendicular-projected onto the
        // plane normal to `axis`). Identity when either projection is degenerate.
        Quat RotationAroundAxis(const Vec3& axis, const Vec3& from, const Vec3& to)
        {
            const Vec3 fromP = from - axis * Dot(from, axis);
            const Vec3 toP = to - axis * Dot(to, axis);
            if (LengthSq(fromP) < ANIM_EPS || LengthSq(toP) < ANIM_EPS) {
                return Quat{};
            }
            const float angle = std::atan2(Dot(Cross(fromP, toP), axis), Dot(fromP, toP));
            return QuatFromAxisAngle(axis, angle);
        }

        Quat WeightedCorrection(const Quat& q, float weight)
        {
            const Quat a = QuatAbs(q);
            if (weight >= 1.0f) {
                return a;
            }
            return QuatNlerp(Quat{}, a, std::clamp(weight, 0.0f, 1.0f));
        }
    }

    TwoBoneIkResult SolveTwoBoneIk(const TwoBoneIkInput& input)
    {
        TwoBoneIkResult result;

        const Mat4 invS = Mat4InverseAffine(input.startJoint);
        const Mat4 invM = Mat4InverseAffine(input.midJoint);

        const Vec3 midPosMs = Mat4Translation(input.midJoint);
        const Vec3 endPosMs = Mat4Translation(input.endJoint);

        const Vec3 midSs = Mat4TransformPoint(invS, midPosMs);
        const Vec3 endSs = Mat4TransformPoint(invS, endPosMs);
        const Vec3 targetSs = Mat4TransformPoint(invS, input.target);
        const Vec3 poleSs = Mat4TransformVector(invS, input.pole);

        const Vec3 startMidSs = midSs;
        const Vec3 midEndSs = endSs - midSs;
        const float a = Length(startMidSs);
        const float b = Length(midEndSs);
        if (a < ANIM_EPS || b < ANIM_EPS) {
            return result;
        }

        // Soften: compress the last stretch of reach so the chain does not snap straight.
        const float reach = a + b;
        const float da = reach * std::clamp(input.soften, 0.0f, 1.0f);
        const float ds = reach - da;
        const float d = Length(targetSs);
        float dSoft = d;
        Vec3 targetSoftSs = targetSs;
        if (d > da && ds > 0.0f && d > ANIM_EPS) {
            const float alpha = (d - da) / ds;
            const float k = 3.0f / (alpha + 3.0f);
            const float k2 = k * k;
            const float ratio = 1.0f - k2 * k2;
            dSoft = da + ds * ratio;
            targetSoftSs = targetSs * (dSoft / d);
        }

        // Mid joint: change the interior angle from the current one to the one that reaches dSoft.
        const Vec3 midAxisLocal = Normalize(input.midAxis, { 0.0f, 0.0f, 1.0f });
        const Vec3 midAxisMs = Normalize(Mat4TransformVector(input.midJoint, midAxisLocal), { 0.0f, 0.0f, 1.0f });
        const Vec3 midAxisSs = Normalize(Mat4TransformVector(invS, midAxisMs), { 0.0f, 0.0f, 1.0f });

        const float denom = 2.0f * a * b;
        const float cosInitial = std::clamp((a * a + b * b - LengthSq(endSs)) / denom, -1.0f, 1.0f);
        const float cosTarget = std::clamp((a * a + b * b - dSoft * dSoft) / denom, -1.0f, 1.0f);
        const float initialAngle = std::acos(cosInitial);
        const float targetAngle = std::acos(cosTarget);

        // The interior angle shrinks when the bend grows about the mid axis, unless the chain bends
        // the other way around the axis.
        const float sign = Dot(Cross(startMidSs, midEndSs), midAxisSs) < 0.0f ? -1.0f : 1.0f;
        const float midAngleDelta = sign * (initialAngle - targetAngle);
        const Quat midRot = QuatFromAxisAngle(midAxisLocal, midAngleDelta);

        // Start joint: aim the corrected mid->end vector (start space) at the softened target.
        const Vec3 midEndMid = Mat4TransformVector(invM, endPosMs - midPosMs);
        const Mat4 startToMid = Mat4Mul(invS, input.midJoint);
        const Vec3 midEndCorrectedSs = Mat4TransformVector(startToMid, QuatRotate(midRot, midEndMid));
        const Vec3 newEndSs = midSs + midEndCorrectedSs;
        const Quat qAim = QuatFromTo(newEndSs, targetSoftSs);

        Quat qPlane{};
        Quat qTwist{};
        if (Length(targetSoftSs) > ANIM_EPS) {
            const Vec3 t = Normalize(targetSoftSs);
            if (Length(poleSs) > ANIM_EPS) {
                // Knee toward the pole gives a bend normal of cross(pole, target).
                const Vec3 refNormal = Cross(poleSs, t);
                const Vec3 jointNormal = QuatRotate(qAim, midAxisSs * sign);
                qPlane = RotationAroundAxis(t, jointNormal, refNormal);
            }
            if (input.twistAngle != 0.0f) {
                qTwist = QuatFromAxisAngle(t, input.twistAngle);
            }
        }
        const Quat startRot = QuatMul(qTwist, QuatMul(qPlane, qAim));

        result.startCorrection = WeightedCorrection(startRot, input.weight);
        result.midCorrection = WeightedCorrection(midRot, input.weight);
        result.reached = d <= da && d > std::fabs(a - b) && input.weight >= 1.0f;
        return result;
    }

    void ApplyJointCorrection(Quat& localRotation, const Quat& correction)
    {
        localRotation = QuatNormalize(QuatMul(localRotation, correction));
    }

    Vec3 ComputeMidAxis(const Mat4& startJoint, const Mat4& midJoint, const Mat4& endJoint,
                        const Vec3& fallbackModel)
    {
        const Vec3 startMid = Mat4Translation(midJoint) - Mat4Translation(startJoint);
        const Vec3 midEnd = Mat4Translation(endJoint) - Mat4Translation(midJoint);

        Vec3 axisMs = Cross(startMid, midEnd);
        if (LengthSq(axisMs) < ANIM_EPS) {
            axisMs = Cross(startMid, fallbackModel);
            if (LengthSq(axisMs) < ANIM_EPS) {
                return { 0.0f, 0.0f, 1.0f };
            }
        }
        return Normalize(Mat4TransformVector(Mat4InverseAffine(midJoint), axisMs), { 0.0f, 0.0f, 1.0f });
    }
}
