#include "Renderer/Animation/AimIk.h"
#include "Renderer/Animation/PoseModelSpace.h"
#include "Renderer/Animation/TwoBoneIk.h"

#include <algorithm>
#include <cmath>

// Reference: ozz-animation IKAimJob (ozz/animation/runtime/ik_aim_job.cc, MIT) and the ozz "look_at"
// sample (chain of joints sharing one aim target).
namespace SasamiEngine::Render::Anim
{
    AimIkResult SolveAimIk(const AimIkInput& input)
    {
        AimIkResult result;

        const Mat4 invJ = Mat4InverseAffine(input.joint);
        const Vec3 targetJs = Mat4TransformPoint(invJ, input.target);
        const Vec3 poleJs = Normalize(Mat4TransformVector(invJ, input.pole));

        // Offsetted forward: the ray from `offset` along `forward` must pass through the target.
        const Vec3 fwd = Normalize(input.forward, { 0.0f, 0.0f, 1.0f });
        const float r2 = LengthSq(targetJs);
        const float ao = Dot(fwd, input.offset);
        const float ac2 = LengthSq(input.offset) - ao * ao;
        if (r2 < ac2) {
            result.reached = false;
            return result;
        }
        const float ai = std::sqrt(r2 - ac2);
        const Vec3 fwdOff = input.offset + fwd * (ai - ao);

        const Quat qForward = QuatFromTo(fwdOff, targetJs);
        const Vec3 correctedUp = QuatRotate(qForward, input.up);

        Quat qPlane{};
        Quat qTwist{};
        if (LengthSq(targetJs) > ANIM_EPS) {
            const Vec3 t = Normalize(targetJs);
            const Vec3 refNormal = Cross(t, poleJs);
            const Vec3 jointNormal = Cross(t, correctedUp);
            if (LengthSq(refNormal) > ANIM_EPS && LengthSq(jointNormal) > ANIM_EPS) {
                const float angle = std::atan2(Dot(Cross(jointNormal, refNormal), t), Dot(jointNormal, refNormal));
                qPlane = QuatFromAxisAngle(t, angle);
            }
            if (input.twistAngle != 0.0f) {
                qTwist = QuatFromAxisAngle(t, input.twistAngle);
            }
        }

        const Quat correction = QuatAbs(QuatMul(qTwist, QuatMul(qPlane, qForward)));
        result.correction = input.weight >= 1.0f
            ? correction
            : QuatNlerp(Quat{}, correction, std::clamp(input.weight, 0.0f, 1.0f));
        result.reached = true;
        return result;
    }

    bool SolveLookAt(const Skeleton& skeleton, const LookAtChain& chain, const Vec3& targetModel,
                     LocalPose& pose, ModelPose& model)
    {
        const int n = static_cast<int>(chain.joints.size());
        if (n == 0 || pose.size() < skeleton.boneCount) {
            return false;
        }

        const int head = chain.joints[0];
        if (head < 0 || head >= static_cast<int>(skeleton.boneCount)) {
            return false;
        }

        bool headReached = false;
        for (int i = 0; i < n; ++i) {
            const int j = chain.joints[i];
            if (j < 0 || j >= static_cast<int>(skeleton.boneCount)) {
                continue;
            }

            float w = chain.weight;
            if (i != n - 1 && i < static_cast<int>(chain.jointWeights.size())) {
                w *= chain.jointWeights[i];
            }

            // Re-express the head-local aim description in joint j's current frame.
            const Mat4 jointToHead = Mat4Mul(Mat4InverseAffine(model[j]), model[head]);

            AimIkInput in;
            in.joint = model[j];
            in.target = targetModel;
            in.forward = Normalize(Mat4TransformVector(jointToHead, chain.forward), { 0.0f, 0.0f, 1.0f });
            in.offset = Mat4TransformPoint(jointToHead, chain.offset);
            in.up = Normalize(Mat4TransformVector(jointToHead, chain.up), { 0.0f, 1.0f, 0.0f });
            in.pole = chain.pole;
            in.weight = std::clamp(w, 0.0f, 1.0f);

            const AimIkResult res = SolveAimIk(in);
            if (i == 0) {
                headReached = res.reached;
            }
            ApplyJointCorrection(pose[j].r, res.correction);
            LocalToModel(skeleton, pose, model);
        }

        LocalToModel(skeleton, pose, model);
        return headReached;
    }
}
