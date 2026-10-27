#include "Renderer/Animation/RootMotion.h"

#include <algorithm>
#include <cmath>

// Root motion extraction in the heading frame (UE "Root Motion" / Unity "Apply Root Motion" style).
// Yaw is a rotation about +Y; translation is expressed in the heading frame at the window start.
namespace SasamiEngine::Render::Anim
{
    namespace
    {
        Vec3 SampleVec3Keys(const std::vector<AnimKeyframe>& keys, float time, const Vec3& fallback)
        {
            if (keys.empty())
                return fallback;
            if (keys.size() == 1 || time <= keys.front().time)
                return { keys.front().value[0], keys.front().value[1], keys.front().value[2] };
            if (time >= keys.back().time)
                return { keys.back().value[0], keys.back().value[1], keys.back().value[2] };

            size_t hi = 1;
            while (hi < keys.size() - 1 && keys[hi].time < time)
                ++hi;
            const AnimKeyframe& k0 = keys[hi - 1];
            const AnimKeyframe& k1 = keys[hi];
            const float span = k1.time - k0.time;
            const float f = span > ANIM_EPS ? (time - k0.time) / span : 0.0f;
            return Lerp({ k0.value[0], k0.value[1], k0.value[2] }, { k1.value[0], k1.value[1], k1.value[2] }, f);
        }

        Quat SampleQuatKeys(const std::vector<AnimKeyframe>& keys, float time, const Quat& fallback)
        {
            if (keys.empty())
                return fallback;
            if (keys.size() == 1 || time <= keys.front().time)
                return { keys.front().value[0], keys.front().value[1], keys.front().value[2], keys.front().value[3] };
            if (time >= keys.back().time)
                return { keys.back().value[0], keys.back().value[1], keys.back().value[2], keys.back().value[3] };

            size_t hi = 1;
            while (hi < keys.size() - 1 && keys[hi].time < time)
                ++hi;
            const AnimKeyframe& k0 = keys[hi - 1];
            const AnimKeyframe& k1 = keys[hi];
            const float span = k1.time - k0.time;
            const float f = span > ANIM_EPS ? (time - k0.time) / span : 0.0f;
            return QuatSlerp({ k0.value[0], k0.value[1], k0.value[2], k0.value[3] },
                             { k1.value[0], k1.value[1], k1.value[2], k1.value[3] }, f);
        }

        // Samples only the root bone's track; channels without keys fall back to the skeleton bind pose.
        Transform SampleRootBone(const SkeletonAnimation& clip, const Skeleton& skeleton, int rootBone, float time)
        {
            const Vec3 bindT{ skeleton.bindLocalT[rootBone][0], skeleton.bindLocalT[rootBone][1],
                              skeleton.bindLocalT[rootBone][2] };
            const Quat bindR{ skeleton.bindLocalR[rootBone][0], skeleton.bindLocalR[rootBone][1],
                              skeleton.bindLocalR[rootBone][2], skeleton.bindLocalR[rootBone][3] };

            Transform out;
            out.t = bindT;
            out.r = bindR;
            if (static_cast<size_t>(rootBone) < clip.boneTracks.size())
            {
                const BoneTrack& track = clip.boneTracks[rootBone];
                out.t = SampleVec3Keys(track.translation, time, bindT);
                out.r = SampleQuatKeys(track.rotation, time, bindR);
            }
            return out;
        }

        RootMotionDelta SegmentDelta(const SkeletonAnimation& clip, const Skeleton& skeleton, int rootBone,
                                     float ta, float tb)
        {
            const Transform a = SampleRootBone(clip, skeleton, rootBone, ta);
            const Transform b = SampleRootBone(clip, skeleton, rootBone, tb);
            const float yawA = QuatYaw(a.r);
            const float yawB = QuatYaw(b.r);

            RootMotionDelta d;
            d.translation = QuatRotate(QuatFromYaw(-yawA), b.t - a.t);
            d.yaw = WrapAngle(yawB - yawA);
            return d;
        }

        RootMotionDelta ComposeDeltas(const RootMotionDelta& d1, const RootMotionDelta& d2)
        {
            RootMotionDelta d;
            d.translation = d1.translation + QuatRotate(QuatFromYaw(d1.yaw), d2.translation);
            d.yaw = WrapAngle(d1.yaw + d2.yaw);
            return d;
        }
    }

    int FindRootMotionBone(const Skeleton& skeleton)
    {
        const int count = static_cast<int>(std::min<uint32_t>(skeleton.boneCount, Skeleton::kMaxBones));
        for (int i = 0; i < count; ++i)
        {
            if (skeleton.parentIndex[i] >= 0)
                continue;
            for (int j = 0; j < count; ++j)
            {
                if (skeleton.parentIndex[j] == i)
                    return i;
            }
        }
        return count > 0 ? 0 : -1;
    }

    RootMotionDelta ExtractRootMotion(const SkeletonAnimation& clip, const Skeleton& skeleton, int rootBone,
                                      float t0, float t1, bool loop, bool horizontalOnly)
    {
        const float d = clip.durationSec;
        if (rootBone < 0 || rootBone >= static_cast<int>(skeleton.boneCount) || clip.boneTracks.empty() || d <= 0.0f)
            return {};

        RootMotionDelta result;
        if (!loop)
        {
            const float a = std::clamp(t0, 0.0f, d);
            const float b = std::clamp(t1, 0.0f, d);
            result = SegmentDelta(clip, skeleton, rootBone, a, b);
        }
        else
        {
            if (t1 < t0)
                return {};

            const float c0 = std::floor(t0 / d);
            const float c1 = std::floor(t1 / d);
            const float w0 = t0 - c0 * d;
            const float w1 = t1 - c1 * d;
            if (c0 == c1)
            {
                result = SegmentDelta(clip, skeleton, rootBone, w0, w1);
            }
            else
            {
                result = SegmentDelta(clip, skeleton, rootBone, w0, d);
                const int fullCycles = static_cast<int>(c1 - c0) - 1;
                if (fullCycles > 0)
                {
                    const RootMotionDelta full = SegmentDelta(clip, skeleton, rootBone, 0.0f, d);
                    for (int i = 0; i < fullCycles; ++i)
                        result = ComposeDeltas(result, full);
                }
                result = ComposeDeltas(result, SegmentDelta(clip, skeleton, rootBone, 0.0f, w1));
            }
        }

        if (horizontalOnly)
            result.translation.y = 0.0f;
        return result;
    }

    void StripRootMotion(LocalPose& pose, int rootBone, const Transform& reference, bool keepVertical)
    {
        if (rootBone < 0 || static_cast<size_t>(rootBone) >= pose.size())
            return;

        Transform& root = pose[rootBone];
        root.t.x = reference.t.x;
        root.t.z = reference.t.z;
        if (!keepVertical)
            root.t.y = reference.t.y;

        const Quat yawRemoved = QuatMul(QuatFromYaw(-QuatYaw(root.r)), root.r);
        root.r = QuatNormalize(QuatMul(QuatFromYaw(QuatYaw(reference.r)), yawRemoved));
    }

    void ApplyRootMotion(Vec3& worldPosition, float& worldYaw, const RootMotionDelta& delta, float scale)
    {
        worldPosition += QuatRotate(QuatFromYaw(worldYaw), delta.translation * scale);
        worldYaw = WrapAngle(worldYaw + delta.yaw);
    }

    RootMotionDelta BlendRootMotion(const RootMotionDelta& a, const RootMotionDelta& b, float t)
    {
        RootMotionDelta d;
        d.translation = Lerp(a.translation, b.translation, t);
        d.yaw = a.yaw + WrapAngle(b.yaw - a.yaw) * t;
        return d;
    }
}
