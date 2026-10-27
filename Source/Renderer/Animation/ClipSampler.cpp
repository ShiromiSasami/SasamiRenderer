#include "Renderer/Animation/ClipSampler.h"

#include <algorithm>
#include <cmath>

namespace SasamiEngine::Render::Anim
{
    namespace
    {
        // Finds the key pair around `t`. Returns false when `keys` is empty. Outputs a single key
        // (k0 == k1, f = 0) when t lies outside the key range.
        bool FindKeys(const std::vector<AnimKeyframe>& keys, float t, const AnimKeyframe*& k0,
                      const AnimKeyframe*& k1, float& f)
        {
            if (keys.empty())
                return false;

            f = 0.0f;
            if (t <= keys.front().time)
            {
                k0 = k1 = &keys.front();
                return true;
            }
            if (t >= keys.back().time)
            {
                k0 = k1 = &keys.back();
                return true;
            }

            const auto it = std::upper_bound(keys.begin(), keys.end(), t,
                [](float value, const AnimKeyframe& key) { return value < key.time; });
            k1 = &*it;
            k0 = k1 - 1;
            const float span = k1->time - k0->time;
            f = span > 0.0f ? (t - k0->time) / span : 0.0f;
            return true;
        }

        void SampleVec3(const std::vector<AnimKeyframe>& keys, float t, Vec3& inOut)
        {
            const AnimKeyframe* k0 = nullptr;
            const AnimKeyframe* k1 = nullptr;
            float f = 0.0f;
            if (!FindKeys(keys, t, k0, k1, f))
                return;

            const Vec3 a{ k0->value[0], k0->value[1], k0->value[2] };
            const Vec3 b{ k1->value[0], k1->value[1], k1->value[2] };
            inOut = Lerp(a, b, f);
        }

        void SampleQuat(const std::vector<AnimKeyframe>& keys, float t, Quat& inOut)
        {
            const AnimKeyframe* k0 = nullptr;
            const AnimKeyframe* k1 = nullptr;
            float f = 0.0f;
            if (!FindKeys(keys, t, k0, k1, f))
                return;

            const Quat a{ k0->value[0], k0->value[1], k0->value[2], k0->value[3] };
            const Quat b{ k1->value[0], k1->value[1], k1->value[2], k1->value[3] };
            inOut = k0 == k1 ? a : QuatNormalize(QuatSlerp(a, b, f));
        }
    }

    LocalPose MakeBindPose(const Skeleton& skeleton)
    {
        const uint32_t count = std::min(skeleton.boneCount, Skeleton::kMaxBones);
        LocalPose pose(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            Transform& x = pose[i];
            x.t = { skeleton.bindLocalT[i][0], skeleton.bindLocalT[i][1], skeleton.bindLocalT[i][2] };
            x.r = { skeleton.bindLocalR[i][0], skeleton.bindLocalR[i][1], skeleton.bindLocalR[i][2],
                    skeleton.bindLocalR[i][3] };
            x.s = { skeleton.bindLocalS[i][0], skeleton.bindLocalS[i][1], skeleton.bindLocalS[i][2] };
        }
        return pose;
    }

    float WrapClipTime(float time, float durationSec, bool loop)
    {
        if (!(durationSec > 0.0f))
            return 0.0f;

        if (loop)
        {
            float wrapped = std::fmod(time, durationSec);
            if (wrapped < 0.0f)
                wrapped += durationSec;
            // Tiny negative inputs can round up to exactly durationSec.
            return wrapped >= durationSec ? 0.0f : wrapped;
        }
        return std::clamp(time, 0.0f, durationSec);
    }

    void SampleClip(const SkeletonAnimation& clip, float time, LocalPose& inOut)
    {
        const size_t count = std::min(inOut.size(), clip.boneTracks.size());
        for (size_t i = 0; i < count; ++i)
        {
            const BoneTrack& track = clip.boneTracks[i];
            Transform& x = inOut[i];
            SampleVec3(track.translation, time, x.t);
            SampleQuat(track.rotation, time, x.r);
            SampleVec3(track.scale, time, x.s);
        }
    }
}
