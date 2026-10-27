#include "Renderer/Animation/CharacterAnimator.h"

#include "Renderer/Animation/BoneMask.h"
#include "Renderer/Animation/ClipSampler.h"
#include "Renderer/Animation/PoseModelSpace.h"
#include "Renderer/Animation/RootMotion.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>

// Per-character pipeline in the order of UE's AnimGraph / the ozz samples:
// state machine -> root motion -> look-at (ozz IKAimJob chain) -> foot IK -> skinning palette.
namespace SasamiEngine::Render::Anim
{
    namespace
    {
        constexpr float LOOK_AT_WEIGHTS[3] = { 0.6f, 0.3f, 0.1f };

        int BoneCount(const Skeleton& skeleton)
        {
            return static_cast<int>(std::min<uint32_t>(skeleton.boneCount, Skeleton::kMaxBones));
        }

        bool NameContains(const Skeleton& skeleton, int bone, const char* fragment)
        {
            std::string name(skeleton.boneName[bone]);
            std::string needle(fragment);
            for (char& c : name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (char& c : needle)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return name.find(needle) != std::string::npos;
        }

        bool NameContainsAny(const Skeleton& skeleton, int bone, std::initializer_list<const char*> fragments)
        {
            for (const char* fragment : fragments)
            {
                if (NameContains(skeleton, bone, fragment))
                    return true;
            }
            return false;
        }

        int FindChildContaining(const Skeleton& skeleton, int parent, std::initializer_list<const char*> fragments)
        {
            const int count = BoneCount(skeleton);
            for (int i = 0; i < count; ++i)
            {
                if (skeleton.parentIndex[i] == parent && NameContainsAny(skeleton, i, fragments))
                    return i;
            }
            return -1;
        }

        int FindAncestorContaining(const Skeleton& skeleton, int bone, const char* fragment)
        {
            const int count = BoneCount(skeleton);
            int guard = 0;
            for (int p = bone >= 0 && bone < count ? skeleton.parentIndex[bone] : -1;
                 p >= 0 && p < count && guard < count; p = skeleton.parentIndex[p], ++guard)
            {
                if (NameContains(skeleton, p, fragment))
                    return p;
            }
            return -1;
        }
    }

    void CharacterAnimator::Initialize(const Skeleton* skeleton)
    {
        m_skeleton = skeleton;
        m_stateMachine.Initialize(skeleton);
        m_footIk.Reset();
        m_lastMotion = RootMotionDelta{};

        m_skin.assign(static_cast<size_t>(Skeleton::kMaxBones) * 16, 0.0f);
        for (uint32_t i = 0; i < Skeleton::kMaxBones; ++i)
        {
            float* matrix = m_skin.data() + static_cast<size_t>(i) * 16;
            matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
        }

        m_pose.clear();
        m_model.clear();
        if (!m_skeleton)
            return;
        m_pose = MakeBindPose(*m_skeleton);
        LocalToModel(*m_skeleton, m_pose, m_model);
    }

    bool CharacterAnimator::AutoConfigureHumanoid(const Skeleton& skeleton, CharacterAnimatorSettings& settings)
    {
        const int count = BoneCount(skeleton);
        bool configured = false;

        const int head = FindBoneContaining(skeleton, "head");
        if (head >= 0)
        {
            const int neck = FindBoneContaining(skeleton, "neck");
            const int spine = FindAncestorContaining(skeleton, neck >= 0 ? neck : head, "spine");

            LookAtChain chain;
            const int candidates[3] = { head, neck, spine };
            for (int i = 0; i < 3; ++i)
            {
                if (candidates[i] < 0)
                    continue;
                chain.joints.push_back(candidates[i]);
                chain.jointWeights.push_back(LOOK_AT_WEIGHTS[i]);
            }
            settings.lookAt = chain;
            settings.lookAtEnabled = true;
            configured = true;
        }

        std::vector<FootIkLeg> legs;
        int firstHip = -1;
        for (int i = 0; i < count; ++i)
        {
            if (!NameContainsAny(skeleton, i, { "upleg", "thigh" }))
                continue;
            const int knee = FindChildContaining(skeleton, i, { "leg", "calf", "knee", "shin" });
            if (knee < 0)
                continue;
            const int ankle = FindChildContaining(skeleton, knee, { "foot", "ankle" });
            if (ankle < 0)
                continue;

            FootIkLeg leg;
            leg.hip = i;
            leg.knee = knee;
            leg.ankle = ankle;
            legs.push_back(leg);
            if (firstHip < 0)
                firstHip = i;
        }

        if (legs.size() >= 2)
        {
            FootIkSolver::MeasureLegs(skeleton, legs);
            settings.legs = legs;
            settings.footIk.pelvis = skeleton.parentIndex[firstHip];
            settings.footIkEnabled = true;
            configured = true;
        }

        return configured;
    }

    void CharacterAnimator::SetWorldTransform(const Vec3& position, float yaw, float scale)
    {
        m_position = position;
        m_yaw = yaw;
        m_scale = scale;
    }

    Mat4 CharacterAnimator::ModelToWorld() const
    {
        Transform world;
        world.t = m_position;
        world.r = QuatFromYaw(m_yaw);
        world.s = { m_scale, m_scale, m_scale };
        return Mat4FromTransform(world);
    }

    void CharacterAnimator::Update(float dt)
    {
        if (!m_skeleton)
            return;

        m_stateMachine.Update(dt, m_pose);
        if (m_pose.size() != m_skeleton->boneCount)
            m_pose = MakeBindPose(*m_skeleton);

        m_lastMotion = m_stateMachine.ConsumeRootMotion();
        if (m_settings.applyRootMotion)
            ApplyRootMotion(m_position, m_yaw, m_lastMotion, m_scale);

        LocalToModel(*m_skeleton, m_pose, m_model);

        if (m_settings.lookAtEnabled && m_lookAtTarget && !m_settings.lookAt.joints.empty())
        {
            const Vec3 targetModel = Mat4TransformPoint(Mat4InverseAffine(ModelToWorld()), *m_lookAtTarget);
            SolveLookAt(*m_skeleton, m_settings.lookAt, targetModel, m_pose, m_model);
        }

        if (m_settings.footIkEnabled && m_groundProbe && !m_settings.legs.empty())
        {
            m_footIk.Solve(*m_skeleton, ModelToWorld(), m_groundProbe, m_settings.legs, m_settings.footIk, dt, m_pose,
                           m_model);
        }

        ModelToSkinMatrices(*m_skeleton, m_model, m_skin.data());
    }
}
