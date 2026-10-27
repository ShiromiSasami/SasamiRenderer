#include "Renderer/Animation/AnimSelfTest.h"

#include "Renderer/Animation/AimIk.h"
#include "Renderer/Animation/AnimMath.h"
#include "Renderer/Animation/AnimNotifyTrack.h"
#include "Renderer/Animation/AnimPose.h"
#include "Renderer/Animation/AnimStateMachine.h"
#include "Renderer/Animation/BlendSpace2D.h"
#include "Renderer/Animation/BoneMask.h"
#include "Renderer/Animation/ClipSampler.h"
#include "Renderer/Animation/FootIk.h"
#include "Renderer/Animation/Inertialization.h"
#include "Renderer/Animation/PoseBlend.h"
#include "Renderer/Animation/PoseModelSpace.h"
#include "Renderer/Animation/RootMotion.h"
#include "Renderer/Animation/SpringDamper.h"
#include "Renderer/Animation/TwoBoneIk.h"
#include "Renderer/Scene/AnimationController.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Anim
{
    namespace
    {
        constexpr float FRAME_DT = 1.0f / 60.0f;

        class Report
        {
        public:
            Report()
            {
                m_out << std::fixed << std::setprecision(6);
            }

            void Check(const char* name, bool ok, const std::string& detail = std::string())
            {
                ++m_total;
                if (ok) {
                    ++m_passed;
                }
                m_out << (ok ? "ok " : "FAIL ") << name;
                if (!detail.empty()) {
                    m_out << ' ' << detail;
                }
                m_out << '\n';
            }

            void CheckBelow(const char* name, float value, float limit)
            {
                std::ostringstream detail;
                detail << std::fixed << std::setprecision(6) << "value=" << value << " limit=" << limit;
                Check(name, std::isfinite(value) && value < limit, detail.str());
            }

            std::string Finish()
            {
                m_out << "anim selftest: " << m_passed << "/" << m_total << " passed\n";
                return m_out.str();
            }

        private:
            std::ostringstream m_out;
            int m_passed = 0;
            int m_total = 0;
        };

        float Vec3Dist(const Vec3& a, const Vec3& b)
        {
            return Length(a - b);
        }

        // Rotation distance that stays accurate for tiny errors and ignores the q / -q ambiguity.
        float QuatDist(const Quat& a, const Quat& b)
        {
            const float dm = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)
                                     + (a.z - b.z) * (a.z - b.z) + (a.w - b.w) * (a.w - b.w));
            const float dp = std::sqrt((a.x + b.x) * (a.x + b.x) + (a.y + b.y) * (a.y + b.y)
                                     + (a.z + b.z) * (a.z + b.z) + (a.w + b.w) * (a.w + b.w));
            return (std::min)(dm, dp);
        }

        float TransformDist(const Transform& a, const Transform& b)
        {
            return (std::max)({ Vec3Dist(a.t, b.t), Vec3Dist(a.s, b.s), QuatDist(a.r, b.r) });
        }

        float PoseMaxDiff(const LocalPose& a, const LocalPose& b)
        {
            if (a.size() != b.size()) {
                return 1e9f;
            }
            float maxDiff = 0.0f;
            for (size_t i = 0; i < a.size(); ++i) {
                maxDiff = (std::max)(maxDiff, TransformDist(a[i], b[i]));
            }
            return maxDiff;
        }

        float Mat4MaxDiff(const Mat4& a, const Mat4& b)
        {
            float maxDiff = 0.0f;
            for (int i = 0; i < 16; ++i) {
                maxDiff = (std::max)(maxDiff, std::fabs(a.m[i] - b.m[i]));
            }
            return maxDiff;
        }

        AnimKeyframe Key(float time, float x, float y, float z, float w = 0.0f)
        {
            return AnimKeyframe{ time, { x, y, z, w } };
        }

        Quat AxisZ(float angle)
        {
            return QuatFromAxisAngle({ 0.0f, 0.0f, 1.0f }, angle);
        }

        // Three bones in a chain; `offsets[i]` is the bind translation of bone i relative to its parent.
        std::shared_ptr<Skeleton> MakeChainSkeleton(const char* const (&names)[3], const Vec3 (&offsets)[3])
        {
            auto skeleton = std::make_shared<Skeleton>();
            skeleton->boneCount = 3;
            Mat4 parentModel;
            for (int i = 0; i < 3; ++i) {
                skeleton->parentIndex[i] = i - 1;
                skeleton->bindLocalT[i][0] = offsets[i].x;
                skeleton->bindLocalT[i][1] = offsets[i].y;
                skeleton->bindLocalT[i][2] = offsets[i].z;
                skeleton->bindLocalR[i][3] = 1.0f;
                skeleton->bindLocalS[i][0] = 1.0f;
                skeleton->bindLocalS[i][1] = 1.0f;
                skeleton->bindLocalS[i][2] = 1.0f;
                std::snprintf(skeleton->boneName[i], sizeof(skeleton->boneName[i]), "%s", names[i]);

                Transform local;
                local.t = offsets[i];
                const Mat4 model = i == 0 ? Mat4FromTransform(local) : Mat4Mul(parentModel, Mat4FromTransform(local));
                const Mat4 inverse = Mat4InverseAffine(model);
                std::memcpy(skeleton->inverseBindPose[i], inverse.m, sizeof(inverse.m));
                parentModel = model;
            }
            return skeleton;
        }

        std::shared_ptr<Skeleton> MakeVerticalChain()
        {
            const char* const names[3] = { "root", "mid", "tip" };
            const Vec3 offsets[3] = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
            return MakeChainSkeleton(names, offsets);
        }

        SkeletonAnimation MakeClip(const char* name, float duration, size_t boneCount)
        {
            SkeletonAnimation clip;
            clip.name = name;
            clip.durationSec = duration;
            clip.boneTracks.resize(boneCount);
            return clip;
        }

        void CheckMath(Report& report)
        {
            const Quat quarterY = QuatFromAxisAngle({ 0.0f, 1.0f, 0.0f }, ANIM_PI * 0.5f);
            const float rotateErr = Vec3Dist(QuatRotate(quarterY, { 0.0f, 0.0f, 1.0f }), { 1.0f, 0.0f, 0.0f });
            report.CheckBelow("math.quat_rotate", rotateErr, 1e-5f);

            const Quat q = QuatFromAxisAngle(Normalize({ 1.0f, 2.0f, 3.0f }), 1.1f);
            const Quat roundTrip = QuatFromScaledAngleAxis(QuatToScaledAngleAxis(QuatAbs(q)));
            report.CheckBelow("math.scaled_angle_axis_roundtrip", QuatDist(roundTrip, q), 1e-5f);

            Transform trs;
            trs.t = { 1.0f, -2.0f, 3.0f };
            trs.r = q;
            trs.s = { 2.0f, 0.5f, 3.0f };
            const Mat4 matrix = Mat4FromTransform(trs);
            const Mat4 identityErr = Mat4Mul(Mat4InverseAffine(matrix), matrix);
            report.CheckBelow("math.inverse_affine", Mat4MaxDiff(identityErr, Mat4{}), 1e-5f);

            const Transform decomposed = Mat4ToTransform(matrix);
            report.CheckBelow("math.mat4_transform_roundtrip", TransformDist(decomposed, trs), 1e-5f);

            const Quat flip = QuatFromTo({ 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f });
            const float flipErr = Vec3Dist(QuatRotate(flip, { 0.0f, 0.0f, 1.0f }), { 0.0f, 0.0f, -1.0f });
            report.CheckBelow("math.quat_from_to_opposite", flipErr, 1e-5f);
        }

        void CheckSamplingAndModelSpace(Report& report)
        {
            const std::shared_ptr<Skeleton> skeleton = MakeVerticalChain();

            SkeletonAnimation clip = MakeClip("sampling", 1.0f, 3);
            clip.boneTracks[1].rotation = { Key(0.0f, 0.0f, 0.0f, 0.0f, 1.0f),
                                            Key(0.5f, AxisZ(0.8f).x, AxisZ(0.8f).y, AxisZ(0.8f).z, AxisZ(0.8f).w),
                                            Key(1.0f, 0.0f, 0.0f, 0.0f, 1.0f) };
            clip.boneTracks[0].translation = { Key(0.0f, 0.0f, 0.0f, 0.0f), Key(1.0f, 1.0f, 0.0f, 0.5f) };

            const float times[] = { 0.0f, 0.25f, 0.6f, 0.9f };
            float maxSkinDiff = 0.0f;
            for (const float time : times) {
                AnimationController controller;
                controller.SetSkeleton(skeleton);
                controller.AddAnimation(clip);
                controller.PlayAnimation(0, true);
                controller.Update(time);
                std::vector<float> reference(Skeleton::kMaxBones * 16, 0.0f);
                controller.GetBoneMatrices(reference.data());

                LocalPose pose = MakeBindPose(*skeleton);
                SampleClip(clip, time, pose);
                ModelPose model;
                LocalToModel(*skeleton, pose, model);
                std::vector<float> mine(Skeleton::kMaxBones * 16, 0.0f);
                ModelToSkinMatrices(*skeleton, model, mine.data());

                for (uint32_t i = 0; i < skeleton->boneCount * 16; ++i) {
                    maxSkinDiff = (std::max)(maxSkinDiff, std::fabs(mine[i] - reference[i]));
                }
            }
            report.CheckBelow("sampling.skin_matrices_vs_controller", maxSkinDiff, 1e-4f);

            LocalPose pose = MakeBindPose(*skeleton);
            SampleClip(clip, 0.6f, pose);
            ModelPose model;
            LocalToModel(*skeleton, pose, model);
            LocalPose recovered;
            ModelToLocal(*skeleton, model, recovered);
            report.CheckBelow("model.local_model_local_roundtrip", PoseMaxDiff(recovered, pose), 1e-4f);
        }

        void CheckBlend(Report& report)
        {
            const std::shared_ptr<Skeleton> skeleton = MakeVerticalChain();
            SkeletonAnimation clip = MakeClip("blend", 1.0f, 3);
            clip.boneTracks[1].rotation = { Key(0.0f, 0.0f, 0.0f, 0.0f, 1.0f),
                                            Key(1.0f, AxisZ(0.9f).x, AxisZ(0.9f).y, AxisZ(0.9f).z, AxisZ(0.9f).w) };
            clip.boneTracks[2].translation = { Key(0.0f, 0.0f, 1.0f, 0.0f), Key(1.0f, 0.5f, 1.5f, 0.0f) };

            const LocalPose bind = MakeBindPose(*skeleton);
            LocalPose posed = bind;
            SampleClip(clip, 0.7f, posed);

            LocalPose blended;
            BlendPoses(bind, posed, 0.0f, blended);
            report.CheckBelow("blend.t0_is_a", PoseMaxDiff(blended, bind), 1e-5f);
            BlendPoses(bind, posed, 1.0f, blended);
            report.CheckBelow("blend.t1_is_b", PoseMaxDiff(blended, posed), 1e-5f);

            LocalPose delta;
            MakeAdditivePose(posed, bind, delta);
            LocalPose rebuilt;
            ApplyAdditivePose(bind, delta, 1.0f, rebuilt);
            report.CheckBelow("blend.additive_roundtrip", PoseMaxDiff(rebuilt, posed), 1e-4f);
        }

        void CheckMask(Report& report)
        {
            const std::shared_ptr<Skeleton> skeleton = MakeVerticalChain();
            const BoneMask mask = BuildSubtreeMask(*skeleton, 1);
            const bool ok = mask.size() == 3 && mask[0] == 0.0f && mask[1] == 1.0f && mask[2] == 1.0f;
            std::ostringstream detail;
            detail << "size=" << mask.size();
            for (const float weight : mask) {
                detail << ' ' << weight;
            }
            report.Check("mask.subtree_of_bone1", ok, detail.str());
        }

        void CheckSprings(Report& report)
        {
            {
                float x = 0.0f;
                float v = 0.0f;
                for (int i = 0; i < 120; ++i) {
                    SimpleSpringDamper(x, v, 3.0f, 0.1f, FRAME_DT);
                }
                report.CheckBelow("spring.converges_to_goal", std::fabs(x - 3.0f), 1e-3f);
            }
            {
                // Critically damped spring released from rest: x(t) = (1 + y t) exp(-y t), y = 2 ln2 / halflife.
                constexpr float EXPECTED = (1.0f + 2.0f * 0.69314718f) * 0.25f;
                float x = 1.0f;
                float v = 0.0f;
                SimpleSpringDamper(x, v, 0.0f, 0.5f, 0.5f);
                std::ostringstream detail;
                detail << std::fixed << std::setprecision(6) << "dist=" << x << " expected=" << EXPECTED;
                report.Check("spring.distance_after_halflife", std::fabs(x - EXPECTED) < 0.05f * EXPECTED, detail.str());
            }
            {
                float xa = 1.0f;
                float va = 0.5f;
                SimpleSpringDamper(xa, va, 0.0f, 0.2f, 0.1f);
                float xb = 1.0f;
                float vb = 0.5f;
                for (int i = 0; i < 10; ++i) {
                    SimpleSpringDamper(xb, vb, 0.0f, 0.2f, 0.01f);
                }
                report.CheckBelow("spring.step_size_independence",
                    (std::max)(std::fabs(xa - xb), std::fabs(va - vb)), 1e-4f);
            }
            {
                constexpr int COUNT = 8;
                constexpr float STEP = 0.05f;
                const Vec3 vGoal{ 0.0f, 0.0f, 2.0f };
                const Vec3 x0{ 0.0f, 0.0f, 0.0f };
                const Vec3 v0{ 1.0f, 0.0f, 0.0f };
                const Vec3 a0{ 0.0f, 0.0f, 0.0f };
                Vec3 predicted[COUNT];
                PredictTrajectory(x0, v0, a0, vGoal, 0.3f, STEP, predicted, COUNT);

                Vec3 x = x0;
                Vec3 v = v0;
                Vec3 a = a0;
                float maxErr = 0.0f;
                for (int i = 0; i < COUNT; ++i) {
                    CharacterSpringUpdate(x, v, a, vGoal, 0.3f, STEP);
                    maxErr = (std::max)(maxErr, Vec3Dist(predicted[i], x));
                }
                report.CheckBelow("spring.predict_trajectory_matches_stepping", maxErr, 1e-4f);
            }
        }

        LocalPose MakeConstantPose(size_t boneCount, const Vec3& translationBase, const Quat& rotation)
        {
            LocalPose pose(boneCount);
            for (size_t i = 0; i < boneCount; ++i) {
                pose[i].t = translationBase * static_cast<float>(i + 1);
                pose[i].r = rotation;
            }
            return pose;
        }

        void CheckInertialization(Report& report)
        {
            constexpr size_t BONES = 3;
            constexpr float HALFLIFE = 0.1f;
            const LocalPose poseA = MakeConstantPose(BONES, { 1.0f, 0.0f, 0.0f }, AxisZ(0.3f));
            const LocalPose poseB = MakeConstantPose(BONES, { 0.0f, 1.0f, 0.5f },
                QuatFromAxisAngle({ 0.0f, 1.0f, 0.0f }, -0.7f));

            PoseVelocity zero;
            zero.linear.assign(BONES, Vec3{});
            zero.angular.assign(BONES, Vec3{});

            PoseInertializer inertializer;
            inertializer.Reset(BONES);
            inertializer.Transition(poseA, zero, poseB, zero);

            LocalPose out;
            inertializer.Update(poseB, HALFLIFE, 0.0f, out);
            report.CheckBelow("inertialization.starts_at_displayed_pose", PoseMaxDiff(out, poseA), 1e-4f);

            for (int i = 0; i < 120; ++i) {
                inertializer.Update(poseB, HALFLIFE, FRAME_DT, out);
            }
            report.CheckBelow("inertialization.converges_to_target", PoseMaxDiff(out, poseB), 1e-3f);
            report.Check("inertialization.inactive_at_end", !inertializer.IsActive());
        }

        struct LocomotionRig
        {
            std::shared_ptr<Skeleton> skeleton;
            SkeletonAnimation idle;
            SkeletonAnimation walk;
        };

        LocomotionRig MakeLocomotionRig()
        {
            LocomotionRig rig;
            rig.skeleton = MakeVerticalChain();

            const Quat idleBend = AxisZ(0.5f);
            const Quat walkBend = AxisZ(-0.5f);
            rig.idle = MakeClip("idle", 1.0f, 3);
            rig.idle.boneTracks[0].translation = { Key(0.0f, 0.0f, 0.0f, 0.0f), Key(1.0f, 0.0f, 0.0f, 0.0f) };
            rig.idle.boneTracks[1].rotation = { Key(0.0f, idleBend.x, idleBend.y, idleBend.z, idleBend.w),
                                                Key(1.0f, idleBend.x, idleBend.y, idleBend.z, idleBend.w) };
            rig.walk = MakeClip("walk", 1.0f, 3);
            rig.walk.boneTracks[0].translation = { Key(0.0f, 0.0f, 0.0f, 0.0f), Key(1.0f, 0.0f, 0.0f, 1.0f) };
            rig.walk.boneTracks[1].rotation = { Key(0.0f, walkBend.x, walkBend.y, walkBend.z, walkBend.w),
                                                Key(1.0f, walkBend.x, walkBend.y, walkBend.z, walkBend.w) };
            return rig;
        }

        void BuildLocomotionMachine(const LocomotionRig& rig, bool withTransitions, TransitionBlendMode mode,
                                    AnimStateMachine& machine)
        {
            machine.Initialize(rig.skeleton.get());
            machine.SetBlendMode(mode);

            AnimState idle;
            idle.name = "Idle";
            idle.clip = &rig.idle;
            AnimState walk;
            walk.name = "Walk";
            walk.clip = &rig.walk;
            machine.AddState(idle);
            machine.AddState(walk);

            if (withTransitions) {
                AnimTransition toWalk;
                toWalk.from = 0;
                toWalk.to = 1;
                toWalk.duration = 0.3f;
                toWalk.conditions.push_back({ "move", ConditionOp::IsTrue, 0.0f });
                machine.AddTransition(toWalk);

                AnimTransition toIdle;
                toIdle.from = 1;
                toIdle.to = 0;
                toIdle.duration = 0.3f;
                toIdle.conditions.push_back({ "move", ConditionOp::IsFalse, 0.0f });
                machine.AddTransition(toIdle);
            }
        }

        float MaxBoneJump(const ModelPose& a, const ModelPose& b)
        {
            float maxJump = 0.0f;
            for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
                maxJump = (std::max)(maxJump, Vec3Dist(Mat4Translation(a[i]), Mat4Translation(b[i])));
            }
            return maxJump;
        }

        // Largest per-frame model-space bone jump after "move" flips on; also returns the final state.
        float RunSwitchScenario(const LocomotionRig& rig, TransitionBlendMode mode, int& outFinalState)
        {
            AnimStateMachine machine;
            BuildLocomotionMachine(rig, true, mode, machine);
            machine.Start(0);

            LocalPose pose = MakeBindPose(*rig.skeleton);
            ModelPose model;
            ModelPose previous;
            for (int i = 0; i < 30; ++i) {
                machine.Update(FRAME_DT, pose);
            }
            LocalToModel(*rig.skeleton, pose, previous);

            machine.SetBool("move", true);
            float maxJump = 0.0f;
            for (int i = 0; i < 90; ++i) {
                machine.Update(FRAME_DT, pose);
                LocalToModel(*rig.skeleton, pose, model);
                maxJump = (std::max)(maxJump, MaxBoneJump(previous, model));
                previous = model;
            }
            outFinalState = machine.CurrentState();
            return maxJump;
        }

        void CheckStateMachine(Report& report)
        {
            const LocomotionRig rig = MakeLocomotionRig();

            LocalPose idlePose = MakeBindPose(*rig.skeleton);
            SampleClip(rig.idle, 0.0f, idlePose);
            LocalPose walkPose = MakeBindPose(*rig.skeleton);
            SampleClip(rig.walk, 0.0f, walkPose);
            ModelPose idleModel;
            ModelPose walkModel;
            LocalToModel(*rig.skeleton, idlePose, idleModel);
            LocalToModel(*rig.skeleton, walkPose, walkModel);
            const float hardCutJump = MaxBoneJump(idleModel, walkModel);

            int crossfadeState = -1;
            int inertialState = -1;
            const float crossfadeJump = RunSwitchScenario(rig, TransitionBlendMode::Crossfade, crossfadeState);
            const float inertialJump = RunSwitchScenario(rig, TransitionBlendMode::Inertialization, inertialState);

            std::ostringstream detail;
            detail << std::fixed << std::setprecision(6) << "hardCut=" << hardCutJump
                   << " crossfade=" << crossfadeJump << " inertial=" << inertialJump;
            report.Check("statemachine.reaches_walk_state", crossfadeState == 1 && inertialState == 1,
                "crossfade=" + std::to_string(crossfadeState) + " inertial=" + std::to_string(inertialState));
            report.Check("statemachine.crossfade_jump_le_hard_cut", crossfadeJump <= hardCutJump + 1e-4f, detail.str());
            report.Check("statemachine.inertial_jump_le_hard_cut", inertialJump <= hardCutJump + 1e-4f, detail.str());

            AnimStateMachine machine;
            BuildLocomotionMachine(rig, false, TransitionBlendMode::Crossfade, machine);
            machine.SetRootMotionEnabled(true);
            machine.Start(1);
            LocalPose pose = MakeBindPose(*rig.skeleton);
            Vec3 position{};
            float yaw = 0.0f;
            for (int i = 0; i < 120; ++i) {
                machine.Update(FRAME_DT, pose);
                ApplyRootMotion(position, yaw, machine.ConsumeRootMotion());
            }
            std::ostringstream travel;
            travel << std::fixed << std::setprecision(6) << "x=" << position.x << " z=" << position.z << " yaw=" << yaw;
            report.Check("rootmotion.accumulates_two_units_forward",
                std::fabs(position.z - 2.0f) < 0.05f && std::fabs(position.x) < 0.05f, travel.str());

            const float rootDrift = std::sqrt(pose[0].t.x * pose[0].t.x + pose[0].t.z * pose[0].t.z);
            report.CheckBelow("rootmotion.pose_root_stays_at_reference", rootDrift, 1e-3f);

            // Notifies through the machine: a point at 0.5 s on a 1 s looping state fires once per cycle.
            AnimNotifyTrack track;
            track.Add({ "footstep", 0.5f, 0.0f });
            AnimStateMachine notifyMachine;
            notifyMachine.Initialize(rig.skeleton.get());
            AnimState walkState;
            walkState.name = "Walk";
            walkState.clip = &rig.walk;
            walkState.notifies = &track;
            notifyMachine.AddState(walkState);
            notifyMachine.Start(0);
            size_t fired = 0;
            for (int i = 0; i < 140; ++i) { // 2.33 s: crosses 0.5 s and 1.5 s
                notifyMachine.Update(FRAME_DT, pose);
                fired += notifyMachine.ConsumeNotifyEvents().size();
            }
            report.Check("statemachine.notify_once_per_cycle", fired == 2, "fired=" + std::to_string(fired));
        }

        float SolveChainTwoBone(const Skeleton& skeleton, const Vec3& target, TwoBoneIkResult& outResult, Vec3& outTip,
                                Vec3& outStart)
        {
            LocalPose pose = MakeBindPose(skeleton);
            ModelPose model;
            LocalToModel(skeleton, pose, model);

            TwoBoneIkInput input;
            input.startJoint = model[0];
            input.midJoint = model[1];
            input.endJoint = model[2];
            input.target = target;
            input.pole = { 0.0f, 1.0f, 0.0f };
            input.midAxis = { 0.0f, 0.0f, 1.0f };
            input.soften = 1.0f;
            input.weight = 1.0f;
            outResult = SolveTwoBoneIk(input);

            ApplyJointCorrection(pose[0].r, outResult.startCorrection);
            ApplyJointCorrection(pose[1].r, outResult.midCorrection);
            LocalToModel(skeleton, pose, model);
            outTip = Mat4Translation(model[2]);
            outStart = Mat4Translation(model[0]);
            return Vec3Dist(outTip, target);
        }

        void CheckTwoBoneIk(Report& report)
        {
            const char* const names[3] = { "upper", "mid", "end" };
            const Vec3 offsets[3] = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
            const std::shared_ptr<Skeleton> skeleton = MakeChainSkeleton(names, offsets);

            TwoBoneIkResult result;
            Vec3 tip;
            Vec3 start;
            const float reachErr = SolveChainTwoBone(*skeleton, { 1.2f, 0.8f, 0.0f }, result, tip, start);
            report.CheckBelow("ik2.reachable_target_hit", reachErr, 1e-3f);
            report.Check("ik2.reachable_flag", result.reached);

            const Vec3 farTarget{ 5.0f, 0.0f, 0.0f };
            SolveChainTwoBone(*skeleton, farTarget, result, tip, start);
            const float alignment = Dot(Normalize(tip - start), Normalize(farTarget));
            std::ostringstream detail;
            detail << std::fixed << std::setprecision(6) << "alignment=" << alignment;
            report.Check("ik2.unreachable_points_at_target", !result.reached && alignment > 0.999f, detail.str());
        }

        void CheckAimIk(Report& report)
        {
            AimIkInput input;
            input.joint = Mat4{};
            input.target = { 1.0f, 0.0f, 1.0f };
            input.forward = { 0.0f, 0.0f, 1.0f };
            input.up = { 0.0f, 1.0f, 0.0f };
            input.pole = { 0.0f, 1.0f, 0.0f };
            const AimIkResult result = SolveAimIk(input);
            const Vec3 aimed = QuatRotate(result.correction, input.forward);
            report.CheckBelow("aim.forward_points_at_target", Vec3Dist(aimed, Normalize(input.target)), 1e-4f);
            report.Check("aim.reached_flag", result.reached);
        }

        void CheckFootIk(Report& report)
        {
            const char* const names[3] = { "hip", "knee", "ankle" };
            const Vec3 offsets[3] = { { 0.0f, 1.0f, 0.0f }, { 0.0f, -0.5f, 0.0f }, { 0.0f, -0.5f, 0.0f } };
            const std::shared_ptr<Skeleton> skeleton = MakeChainSkeleton(names, offsets);

            std::vector<FootIkLeg> legs(1);
            legs[0].hip = 0;
            legs[0].knee = 1;
            legs[0].ankle = 2;
            legs[0].midAxis = { 1.0f, 0.0f, 0.0f };
            FootIkSolver::MeasureLegs(*skeleton, legs);

            constexpr float GROUND = 0.2f;
            const GroundProbe probe = [](float, float, float& outHeight, Vec3& outNormal) {
                outHeight = GROUND;
                outNormal = { 0.0f, 1.0f, 0.0f };
                return true;
            };

            FootIkSettings settings;
            FootIkSolver solver;
            solver.Reset();
            const LocalPose bind = MakeBindPose(*skeleton);
            LocalPose pose;
            ModelPose model;
            for (int i = 0; i < 180; ++i) {
                pose = bind;
                LocalToModel(*skeleton, pose, model);
                solver.Solve(*skeleton, Mat4{}, probe, legs, settings, FRAME_DT, pose, model);
            }
            const float ankleY = Mat4Translation(model[2]).y;
            const float expected = GROUND + legs[0].ankleHeight;
            std::ostringstream detail;
            detail << std::fixed << std::setprecision(6) << "ankleY=" << ankleY << " expected=" << expected
                   << " reached=" << solver.ReachedCount();
            report.Check("foot.ankle_reaches_ground", std::fabs(ankleY - expected) < 0.01f, detail.str());
        }

        void CheckBlendSpace2D(Report& report)
        {
            constexpr int SAMPLE_COUNT = 5;
            const float positions[SAMPLE_COUNT][2] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { -1.0f, 0.0f },
                                                       { 0.0f, 1.0f }, { 0.0f, -1.0f } };
            std::vector<SkeletonAnimation> clips;
            for (int i = 0; i < SAMPLE_COUNT; ++i) {
                clips.push_back(MakeClip("blend2d", 1.0f, 3));
            }

            struct ModeCase
            {
                BlendSpace2DMode mode;
                const char* label;
            };
            const ModeCase modes[] = { { BlendSpace2DMode::FreeformCartesian, "cartesian" },
                                       { BlendSpace2DMode::FreeformDirectional, "directional" } };

            constexpr int GRID_COUNT = 13;
            constexpr float GRID_MIN = -1.5f;
            constexpr float GRID_STEP = 0.25f;
            constexpr float SHIFT = 1e-3f;

            for (const ModeCase& modeCase : modes) {
                BlendSpace2D space;
                space.SetMode(modeCase.mode);
                for (int i = 0; i < SAMPLE_COUNT; ++i) {
                    space.AddSample(&clips[i], positions[i][0], positions[i][1]);
                }
                const std::string prefix = std::string("blend2d.") + modeCase.label;

                std::vector<float> weights;
                float minOwnWeight = 1.0f;
                for (int i = 0; i < SAMPLE_COUNT; ++i) {
                    space.ComputeWeights(positions[i][0], positions[i][1], weights);
                    minOwnWeight = weights.size() == static_cast<size_t>(SAMPLE_COUNT)
                        ? (std::min)(minOwnWeight, weights[i]) : -1.0f;
                }
                std::ostringstream ownDetail;
                ownDetail << std::fixed << std::setprecision(6) << "minOwnWeight=" << minOwnWeight;
                report.Check((prefix + ".weight_at_sample").c_str(), minOwnWeight > 0.999f, ownDetail.str());

                float maxSumErr = 0.0f;
                bool inRange = true;
                float maxJump = 0.0f;
                std::vector<float> shifted;
                for (int iy = 0; iy < GRID_COUNT; ++iy) {
                    for (int ix = 0; ix < GRID_COUNT; ++ix) {
                        const float x = GRID_MIN + GRID_STEP * static_cast<float>(ix);
                        const float y = GRID_MIN + GRID_STEP * static_cast<float>(iy);
                        space.ComputeWeights(x, y, weights);
                        space.ComputeWeights(x + SHIFT, y, shifted);
                        if (weights.size() != static_cast<size_t>(SAMPLE_COUNT) || shifted.size() != weights.size()) {
                            inRange = false;
                            maxSumErr = 1e9f;
                            maxJump = 1e9f;
                            continue;
                        }
                        float sum = 0.0f;
                        for (size_t k = 0; k < weights.size(); ++k) {
                            sum += weights[k];
                            if (!(weights[k] >= 0.0f && weights[k] <= 1.0f)) {
                                inRange = false;
                            }
                            maxJump = (std::max)(maxJump, std::fabs(shifted[k] - weights[k]));
                        }
                        maxSumErr = (std::max)(maxSumErr, std::fabs(sum - 1.0f));
                    }
                }
                std::ostringstream sumDetail;
                sumDetail << std::fixed << std::setprecision(6) << "maxSumErr=" << maxSumErr
                          << " inRange=" << (inRange ? 1 : 0);
                report.Check((prefix + ".sum_is_one").c_str(), inRange && maxSumErr < 1e-5f, sumDetail.str());
                report.CheckBelow((prefix + ".continuous").c_str(), maxJump, 0.05f);
            }

            {
                BlendSpace2D single;
                single.AddSample(&clips[0], 0.3f, -0.2f);
                std::vector<float> weights;
                single.ComputeWeights(1.0f, 1.0f, weights);
                std::ostringstream detail;
                detail << "size=" << weights.size();
                report.Check("blend2d.single_sample", weights.size() == 1 && std::fabs(weights[0] - 1.0f) < 1e-6f,
                    detail.str());
            }
            {
                BlendSpace2D empty;
                std::vector<float> weights{ 1.0f };
                empty.ComputeWeights(0.0f, 0.0f, weights);
                std::ostringstream detail;
                detail << "size=" << weights.size();
                report.Check("blend2d.empty", weights.empty(), detail.str());
            }
        }

        int FindNotifyIndex(const AnimNotifyTrack& track, const char* name)
        {
            const std::vector<AnimNotify>& notifies = track.Notifies();
            for (size_t i = 0; i < notifies.size(); ++i) {
                if (notifies[i].name == name) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        int CountNotifyEvents(const std::vector<AnimNotifyEvent>& events, int notifyIndex, AnimNotifyEventType type)
        {
            int count = 0;
            for (const AnimNotifyEvent& event : events) {
                if (static_cast<int>(event.notifyIndex) == notifyIndex && event.type == type) {
                    ++count;
                }
            }
            return count;
        }

        void CheckAnimNotify(Report& report)
        {
            AnimNotifyTrack track;
            track.Add({ "hit", 0.5f, 0.2f });
            track.Add({ "step", 0.25f, 0.0f });
            track.Add({ "loop0", 0.0f, 0.0f });

            const int stepIndex = FindNotifyIndex(track, "step");
            const int loopIndex = FindNotifyIndex(track, "loop0");
            const int hitIndex = FindNotifyIndex(track, "hit");
            if (stepIndex < 0 || loopIndex < 0 || hitIndex < 0) {
                report.Check("notify.track_lookup", false, "notify name not found in Notifies()");
                return;
            }

            constexpr float DURATION = 1.0f;
            std::vector<AnimNotifyEvent> events;
            const auto detailOf = [&events]() {
                return "events=" + std::to_string(events.size());
            };

            events.clear();
            track.Collect(0.2f, 0.3f, DURATION, true, false, events);
            report.Check("notify.point_once",
                events.size() == 1 && CountNotifyEvents(events, stepIndex, AnimNotifyEventType::Trigger) == 1,
                detailOf());

            events.clear();
            track.Collect(0.2f, 0.25f, DURATION, false, false, events);
            track.Collect(0.25f, 0.3f, DURATION, false, false, events);
            report.Check("notify.no_double_fire",
                CountNotifyEvents(events, stepIndex, AnimNotifyEventType::Trigger) == 1, detailOf());

            {
                events.clear();
                track.Collect(0.4f, 0.6f, DURATION, false, false, events);
                const bool begin = events.size() == 1 && events[0].notifyIndex == static_cast<uint32_t>(hitIndex)
                    && events[0].type == AnimNotifyEventType::Begin;

                events.clear();
                track.Collect(0.6f, 0.8f, DURATION, false, false, events);
                const bool end = events.size() == 1 && events[0].notifyIndex == static_cast<uint32_t>(hitIndex)
                    && events[0].type == AnimNotifyEventType::End;

                events.clear();
                track.Collect(0.4f, 0.8f, DURATION, false, false, events);
                const bool both = events.size() == 2 && events[0].notifyIndex == static_cast<uint32_t>(hitIndex)
                    && events[0].type == AnimNotifyEventType::Begin && events[1].notifyIndex == static_cast<uint32_t>(hitIndex)
                    && events[1].type == AnimNotifyEventType::End;

                std::ostringstream detail;
                detail << "begin=" << begin << " end=" << end << " both=" << both;
                report.Check("notify.state_begin_end", begin && end && both, detail.str());
            }

            events.clear();
            track.Collect(0.9f, 0.1f, DURATION, true, false, events);
            report.Check("notify.wrap",
                CountNotifyEvents(events, loopIndex, AnimNotifyEventType::Trigger) == 1, detailOf());

            events.clear();
            track.Collect(0.6f, 0.3f, DURATION, false, false, events);
            report.Check("notify.no_backwards", events.empty(), detailOf());

            {
                events.clear();
                track.Collect(0.25f, 0.3f, DURATION, false, true, events);
                const bool included = CountNotifyEvents(events, stepIndex, AnimNotifyEventType::Trigger) == 1;
                events.clear();
                track.Collect(0.25f, 0.3f, DURATION, false, false, events);
                const bool excluded = events.empty();
                std::ostringstream detail;
                detail << "included=" << included << " excluded=" << excluded;
                report.Check("notify.include_start", included && excluded, detail.str());
            }

            {
                const uint32_t hit = static_cast<uint32_t>(hitIndex);
                const bool active = track.IsStateActive(hit, 0.6f);
                const bool inactive = !track.IsStateActive(hit, 0.75f);
                const bool pointsNever = !track.IsStateActive(static_cast<uint32_t>(stepIndex), 0.25f)
                    && !track.IsStateActive(static_cast<uint32_t>(loopIndex), 0.0f);
                std::ostringstream detail;
                detail << "active=" << active << " inactive=" << inactive << " pointsNever=" << pointsNever;
                report.Check("notify.state_active", active && inactive && pointsNever, detail.str());
            }
        }
    }

    std::string RunAnimSelfTest()
    {
        Report report;
        CheckMath(report);
        CheckSamplingAndModelSpace(report);
        CheckBlend(report);
        CheckMask(report);
        CheckSprings(report);
        CheckInertialization(report);
        CheckStateMachine(report);
        CheckTwoBoneIk(report);
        CheckAimIk(report);
        CheckFootIk(report);
        CheckBlendSpace2D(report);
        CheckAnimNotify(report);
        return report.Finish();
    }
}
