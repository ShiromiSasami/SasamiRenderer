#pragma once
#include "Renderer/Structures/Skeleton.h"
#include "Renderer/Structures/SkeletonAnimation.h"

#include <array>
#include <memory>
#include <vector>

namespace SasamiRenderer
{
    class AnimationController
    {
    public:
        void SetSkeleton(std::shared_ptr<Skeleton> skeleton);
        void AddAnimation(SkeletonAnimation anim);

        // Start playing animation by index. Loops by default.
        void PlayAnimation(int index, bool loop = true);

        // Advance time by dt seconds (call once per frame before GetBoneMatrices).
        void Update(float dt);

        // Write final bone matrices (column-major float4x4 × kMaxBones) into outMatrices.
        // outMatrices must point to at least Skeleton::kMaxBones * 16 floats.
        void GetBoneMatrices(float* outMatrices) const;

        bool HasSkeleton()   const { return m_skeleton != nullptr; }
        bool HasAnimations() const { return !m_animations.empty(); }
        int  CurrentAnimation() const { return m_currentAnim; }
        float CurrentTime()     const { return m_time; }

    private:
        struct BonePose { float t[3]; float r[4]; float s[3]; }; // local TRS

        std::shared_ptr<Skeleton>      m_skeleton;
        std::vector<SkeletonAnimation> m_animations;
        int                            m_currentAnim = -1;
        float                          m_time        = 0.0f;
        bool                           m_loop        = true;

        std::vector<BonePose> m_localPoses;      // one per bone
        using Matrix4 = std::array<float, 16>;
        std::vector<Matrix4> m_globalMatrices;  // model-space transform per bone
        std::vector<Matrix4> m_boneMatrices;    // globalMatrix * inverseBindPose

        void EvaluateLocalPoses(const SkeletonAnimation& anim, float t);
        void ComputeGlobalMatrices();

        static void Mul4x4(const float* a, const float* b, float* out);
        static void TrsToMatrix(const float* t, const float* r, const float* s, float* out);
        static void SampleVec3(const std::vector<AnimKeyframe>& keys, float t, float* out);
        static void SampleQuat(const std::vector<AnimKeyframe>& keys, float t, float* out);
        static void SlerpQuat(const float* a, const float* b, float t, float* out);
        static void NormalizeQuat(float* q);
    };
}
