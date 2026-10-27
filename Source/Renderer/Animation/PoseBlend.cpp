#include "Renderer/Animation/PoseBlend.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace SasamiEngine::Render::Anim
{
    namespace
    {
        constexpr float DIVIDE_EPS = 1e-8f;

        Transform LerpTransform(const Transform& a, const Transform& b, float w)
        {
            Transform o;
            o.t = Lerp(a.t, b.t, w);
            o.r = QuatNlerp(a.r, b.r, w);
            o.s = Lerp(a.s, b.s, w);
            return o;
        }

        float SafeDivide(float v, float ref)
        {
            return std::fabs(ref) < DIVIDE_EPS ? 1.0f : v / ref;
        }

        Transform MakeDelta(const Transform& pose, const Transform& ref)
        {
            Transform o;
            o.t = pose.t - ref.t;
            o.r = QuatMul(QuatConjugate(ref.r), pose.r);
            o.s = { SafeDivide(pose.s.x, ref.s.x), SafeDivide(pose.s.y, ref.s.y), SafeDivide(pose.s.z, ref.s.z) };
            return o;
        }

        Transform ApplyDelta(const Transform& base, const Transform& delta, float w)
        {
            const Vec3 one{ 1.0f, 1.0f, 1.0f };
            Transform o;
            o.t = base.t + delta.t * w;
            o.r = QuatNormalize(QuatMul(base.r, QuatNlerp(Quat{}, delta.r, w)));
            o.s = MulComponents(base.s, Lerp(one, delta.s, w));
            return o;
        }

        float MaskWeight(const BoneMask& mask, size_t i)
        {
            return i < mask.size() ? mask[i] : 0.0f;
        }
    }

    void BlendPoses(const LocalPose& a, const LocalPose& b, float t, LocalPose& out)
    {
        const size_t count = std::min(a.size(), b.size());
        LocalPose result(count);
        for (size_t i = 0; i < count; ++i)
            result[i] = LerpTransform(a[i], b[i], t);
        out = std::move(result);
    }

    void BlendPosesMasked(const LocalPose& a, const LocalPose& b, const BoneMask& mask, float t, LocalPose& out)
    {
        const size_t count = std::min(a.size(), b.size());
        LocalPose result(count);
        for (size_t i = 0; i < count; ++i)
            result[i] = LerpTransform(a[i], b[i], t * MaskWeight(mask, i));
        out = std::move(result);
    }

    void MakeAdditivePose(const LocalPose& pose, const LocalPose& reference, LocalPose& outDelta)
    {
        const size_t count = std::min(pose.size(), reference.size());
        LocalPose result(count);
        for (size_t i = 0; i < count; ++i)
            result[i] = MakeDelta(pose[i], reference[i]);
        outDelta = std::move(result);
    }

    void ApplyAdditivePose(const LocalPose& base, const LocalPose& delta, float weight, LocalPose& out)
    {
        const size_t count = std::min(base.size(), delta.size());
        LocalPose result(count);
        for (size_t i = 0; i < count; ++i)
            result[i] = ApplyDelta(base[i], delta[i], weight);
        out = std::move(result);
    }

    void ApplyAdditivePoseMasked(const LocalPose& base, const LocalPose& delta, const BoneMask& mask, float weight,
                                 LocalPose& out)
    {
        const size_t count = std::min(base.size(), delta.size());
        LocalPose result(count);
        for (size_t i = 0; i < count; ++i)
            result[i] = ApplyDelta(base[i], delta[i], weight * MaskWeight(mask, i));
        out = std::move(result);
    }
}
