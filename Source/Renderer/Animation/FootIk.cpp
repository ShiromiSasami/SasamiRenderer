#include "Renderer/Animation/FootIk.h"
#include "Renderer/Animation/PoseModelSpace.h"
#include "Renderer/Animation/SpringDamper.h"
#include "Renderer/Animation/TwoBoneIk.h"

#include <algorithm>
#include <cmath>

// Reference: ozz-animation IKTwoBoneJob for the leg solve (see TwoBoneIk.cpp); the pelvis-drop approach
// follows Rune Skovbo Johansen's "Locomotion System" and UE/Unity foot IK.
namespace SasamiEngine::Render::Anim
{
    namespace
    {
        bool IsValidBone(const Skeleton& skeleton, int index)
        {
            return index >= 0 && index < static_cast<int>(skeleton.boneCount);
        }

        bool IsValidLeg(const Skeleton& skeleton, const FootIkLeg& leg)
        {
            return IsValidBone(skeleton, leg.hip) && IsValidBone(skeleton, leg.knee) && IsValidBone(skeleton, leg.ankle);
        }

        LocalPose BuildBindPose(const Skeleton& skeleton)
        {
            LocalPose pose(skeleton.boneCount);
            for (uint32_t i = 0; i < skeleton.boneCount; ++i) {
                Transform& x = pose[i];
                x.t = { skeleton.bindLocalT[i][0], skeleton.bindLocalT[i][1], skeleton.bindLocalT[i][2] };
                x.r = QuatNormalize({ skeleton.bindLocalR[i][0], skeleton.bindLocalR[i][1],
                                      skeleton.bindLocalR[i][2], skeleton.bindLocalR[i][3] });
                x.s = { skeleton.bindLocalS[i][0], skeleton.bindLocalS[i][1], skeleton.bindLocalS[i][2] };
            }
            return pose;
        }

        Quat LimitRotationAngle(const Quat& q, float maxAngle)
        {
            Vec3 aa = QuatToScaledAngleAxis(QuatAbs(q));
            const float len = Length(aa);
            if (len > maxAngle && len > ANIM_EPS) {
                aa = aa * (std::max(maxAngle, 0.0f) / len);
            }
            return QuatFromScaledAngleAxis(aa);
        }
    }

    void FootIkSolver::MeasureLegs(const Skeleton& skeleton, std::vector<FootIkLeg>& legs)
    {
        const LocalPose bind = BuildBindPose(skeleton);
        ModelPose model;
        LocalToModel(skeleton, bind, model);

        for (FootIkLeg& leg : legs) {
            if (!IsValidLeg(skeleton, leg)) {
                continue;
            }
            leg.ankleHeight = std::max(Mat4Translation(model[leg.ankle]).y, 0.0f);
            if (LengthSq(leg.midAxis) < ANIM_EPS) {
                leg.midAxis = ComputeMidAxis(model[leg.hip], model[leg.knee], model[leg.ankle]);
            }
        }
    }

    void FootIkSolver::Reset()
    {
        m_pelvisOffset = 0.0f;
        m_pelvisVelocity = 0.0f;
        std::fill(m_footOffset.begin(), m_footOffset.end(), 0.0f);
        std::fill(m_footVelocity.begin(), m_footVelocity.end(), 0.0f);
        m_reached = 0;
    }

    void FootIkSolver::Solve(const Skeleton& skeleton, const Mat4& modelToWorld, const GroundProbe& probe,
                             const std::vector<FootIkLeg>& legs, const FootIkSettings& settings, float dt,
                             LocalPose& pose, ModelPose& model)
    {
        const size_t legCount = legs.size();
        m_footOffset.resize(legCount, 0.0f);
        m_footVelocity.resize(legCount, 0.0f);
        m_reached = 0;

        if (settings.weight <= 0.0f || !probe) {
            SimpleSpringDamper(m_pelvisOffset, m_pelvisVelocity, 0.0f, settings.pelvisHalflife, dt);
            for (size_t i = 0; i < legCount; ++i) {
                SimpleSpringDamper(m_footOffset[i], m_footVelocity[i], 0.0f, settings.footHalflife, dt);
            }
            return;
        }

        const Mat4 worldToModel = Mat4InverseAffine(modelToWorld);

        std::vector<float> ankleYBefore(legCount, 0.0f);
        std::vector<Vec3> normalModel(legCount);
        std::vector<char> hasHit(legCount, 0);

        for (size_t i = 0; i < legCount; ++i) {
            const FootIkLeg& leg = legs[i];
            if (!IsValidLeg(skeleton, leg)) {
                SimpleSpringDamper(m_footOffset[i], m_footVelocity[i], 0.0f, settings.footHalflife, dt);
                continue;
            }

            const Vec3 ankleModel = Mat4Translation(model[leg.ankle]);
            ankleYBefore[i] = ankleModel.y;
            const Vec3 ankleW = Mat4TransformPoint(modelToWorld, ankleModel);

            float footTarget = 0.0f;
            float height = 0.0f;
            Vec3 normal{ 0.0f, 1.0f, 0.0f };
            if (probe(ankleW.x, ankleW.z, height, normal)) {
                const Vec3 groundModel = Mat4TransformPoint(worldToModel, { ankleW.x, height, ankleW.z });
                normalModel[i] = Normalize(Mat4TransformVector(worldToModel, normal), { 0.0f, 1.0f, 0.0f });
                hasHit[i] = 1;
                const float desiredAnkleY = groundModel.y + leg.ankleHeight;
                footTarget = std::min(desiredAnkleY - ankleModel.y, settings.maxFootRaise);
            }
            SimpleSpringDamper(m_footOffset[i], m_footVelocity[i], footTarget, settings.footHalflife, dt);
        }

        // Pelvis: follow the lowest foot, never rise.
        float lowest = 0.0f;
        for (size_t i = 0; i < legCount; ++i) {
            lowest = std::min(lowest, m_footOffset[i]);
        }
        const float pelvisTarget = std::clamp(lowest, -settings.maxPelvisDrop, 0.0f);
        SimpleSpringDamper(m_pelvisOffset, m_pelvisVelocity, pelvisTarget, settings.pelvisHalflife, dt);

        if (IsValidBone(skeleton, settings.pelvis)) {
            const Vec3 delta{ 0.0f, m_pelvisOffset * settings.weight, 0.0f };
            const int parent = skeleton.parentIndex[settings.pelvis];
            const Vec3 deltaLocal = parent >= 0 ? Mat4TransformVector(Mat4InverseAffine(model[parent]), delta) : delta;
            pose[settings.pelvis].t += deltaLocal;
            LocalToModel(skeleton, pose, model);
        }

        for (size_t i = 0; i < legCount; ++i) {
            const FootIkLeg& leg = legs[i];
            if (!IsValidLeg(skeleton, leg)) {
                continue;
            }

            const Vec3 hipPos = Mat4Translation(model[leg.hip]);
            const Vec3 kneePos = Mat4Translation(model[leg.knee]);
            const Vec3 anklePos = Mat4Translation(model[leg.ankle]);

            TwoBoneIkInput in;
            in.startJoint = model[leg.hip];
            in.midJoint = model[leg.knee];
            in.endJoint = model[leg.ankle];
            in.target = { anklePos.x, ankleYBefore[i] + m_footOffset[i], anklePos.z };
            in.pole = Normalize(kneePos - (hipPos + anklePos) * 0.5f, { 0.0f, 0.0f, 1.0f });
            in.midAxis = LengthSq(leg.midAxis) > ANIM_EPS
                ? leg.midAxis
                : ComputeMidAxis(model[leg.hip], model[leg.knee], model[leg.ankle]);
            in.soften = settings.soften;
            in.weight = std::clamp(settings.weight, 0.0f, 1.0f);

            const TwoBoneIkResult res = SolveTwoBoneIk(in);
            ApplyJointCorrection(pose[leg.hip].r, res.startCorrection);
            ApplyJointCorrection(pose[leg.knee].r, res.midCorrection);
            if (res.reached) {
                ++m_reached;
            }
            LocalToModel(skeleton, pose, model);
        }

        if (settings.alignToNormal) {
            for (size_t i = 0; i < legCount; ++i) {
                const FootIkLeg& leg = legs[i];
                if (!IsValidLeg(skeleton, leg) || !hasHit[i]) {
                    continue;
                }

                Quat q = QuatFromTo({ 0.0f, 1.0f, 0.0f }, normalModel[i]);
                q = LimitRotationAngle(q, settings.maxAlignAngle);
                q = QuatNlerp(Quat{}, q, std::clamp(settings.weight, 0.0f, 1.0f));

                const Quat r = Mat4ToTransform(model[leg.ankle]).r;
                const Quat c = QuatMul(QuatConjugate(r), QuatMul(q, r));
                ApplyJointCorrection(pose[leg.ankle].r, c);
            }
        }

        LocalToModel(skeleton, pose, model);
    }
}
