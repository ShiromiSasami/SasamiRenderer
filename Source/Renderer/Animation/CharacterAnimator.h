#pragma once
#include "Renderer/Animation/AimIk.h"
#include "Renderer/Animation/AnimStateMachine.h"
#include "Renderer/Animation/FootIk.h"
#include "Renderer/Structures/Skeleton.h"

#include <optional>
#include <vector>

// Per-character animation pipeline, in the order UE's AnimGraph / ozz samples run it:
//   state machine (clips, blend spaces, transitions) -> root motion to the world transform
//   -> look-at (aim IK chain) -> foot IK -> skinning palette.
namespace SasamiEngine::Render::Anim
{
    struct CharacterAnimatorSettings
    {
        bool applyRootMotion = true;
        bool lookAtEnabled = false;
        LookAtChain lookAt;
        bool footIkEnabled = false;
        std::vector<FootIkLeg> legs;
        FootIkSettings footIk;
    };

    class CharacterAnimator
    {
    public:
        // `skeleton` must outlive the animator.
        void Initialize(const Skeleton* skeleton);

        // Guesses look-at and leg chains from common bone-name fragments (mixamo "Head"/"Neck"/
        // "Spine", "UpLeg"/"Leg"/"Foot"; "thigh"/"calf"/"foot"; "Hip"/"Knee"/"Ankle").
        // Returns false and leaves `settings` untouched for anything it cannot find.
        static bool AutoConfigureHumanoid(const Skeleton& skeleton, CharacterAnimatorSettings& settings);

        AnimStateMachine& StateMachine() { return m_stateMachine; }
        CharacterAnimatorSettings& Settings() { return m_settings; }

        void SetWorldTransform(const Vec3& position, float yaw, float scale);
        Vec3 Position() const { return m_position; }
        float Yaw() const { return m_yaw; }
        float Scale() const { return m_scale; }
        // world = translate(position) * rotateY(yaw) * scale
        Mat4 ModelToWorld() const;

        void SetLookAtTarget(const std::optional<Vec3>& worldTarget) { m_lookAtTarget = worldTarget; }
        void SetGroundProbe(GroundProbe probe) { m_groundProbe = std::move(probe); }

        // Runs the whole pipeline. Root motion moves Position()/Yaw() when applyRootMotion is set.
        void Update(float dt);

        const LocalPose& Pose() const { return m_pose; }
        const ModelPose& Model() const { return m_model; }
        // Skeleton::kMaxBones * 16 floats, AnimationController::GetBoneMatrices layout.
        const float* SkinMatrices() const { return m_skin.data(); }
        // Root motion applied during the last Update, in the heading frame.
        const RootMotionDelta& LastRootMotion() const { return m_lastMotion; }
        const FootIkSolver& FootIk() const { return m_footIk; }

    private:
        const Skeleton* m_skeleton = nullptr;
        AnimStateMachine m_stateMachine;
        CharacterAnimatorSettings m_settings;
        FootIkSolver m_footIk;
        GroundProbe m_groundProbe;
        std::optional<Vec3> m_lookAtTarget;

        Vec3 m_position{};
        float m_yaw = 0.0f;
        float m_scale = 1.0f;

        LocalPose m_pose;
        ModelPose m_model;
        std::vector<float> m_skin;
        RootMotionDelta m_lastMotion{};
    };
}

namespace SasamiEngine { using namespace Render; }
