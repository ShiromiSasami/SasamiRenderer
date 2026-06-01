#include "Renderer/Scene/AnimationController.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace SasamiRenderer
{
    // -------------------------------------------------------------------------
    // Public API
    // -------------------------------------------------------------------------

    void AnimationController::SetSkeleton(std::shared_ptr<Skeleton> skeleton)
    {
        m_skeleton = std::move(skeleton);
        if (!m_skeleton) return;

        const uint32_t n = m_skeleton->boneCount;
        m_localPoses.resize(n);
        m_globalMatrices.resize(n);
        m_boneMatrices.resize(n);

        // Default local pose: identity TRS
        for (uint32_t i = 0; i < n; ++i) {
            m_localPoses[i].t[0] = m_localPoses[i].t[1] = m_localPoses[i].t[2] = 0.0f;
            m_localPoses[i].r[0] = m_localPoses[i].r[1] = m_localPoses[i].r[2] = 0.0f;
            m_localPoses[i].r[3] = 1.0f;
            m_localPoses[i].s[0] = m_localPoses[i].s[1] = m_localPoses[i].s[2] = 1.0f;
        }
    }

    void AnimationController::AddAnimation(SkeletonAnimation anim)
    {
        m_animations.push_back(std::move(anim));
    }

    void AnimationController::PlayAnimation(int index, bool loop)
    {
        if (index < 0 || index >= static_cast<int>(m_animations.size())) return;
        m_currentAnim = index;
        m_loop        = loop;
        m_time        = 0.0f;
    }

    void AnimationController::Update(float dt)
    {
        if (!m_skeleton || m_currentAnim < 0) return;

        const SkeletonAnimation& anim = m_animations[m_currentAnim];
        m_time += dt;

        if (m_loop && anim.durationSec > 0.0f) {
            m_time = std::fmod(m_time, anim.durationSec);
        } else {
            m_time = std::min(m_time, anim.durationSec);
        }

        EvaluateLocalPoses(anim, m_time);
        ComputeGlobalMatrices();
    }

    void AnimationController::GetBoneMatrices(float* outMatrices) const
    {
        const uint32_t n = m_skeleton ? m_skeleton->boneCount : 0;

        // Copy skinning matrices for active bones
        for (uint32_t i = 0; i < n; ++i) {
            std::memcpy(outMatrices + i * 16, m_boneMatrices[i].data(), sizeof(float) * 16);
        }

        // Fill remaining slots with identity
        for (uint32_t i = n; i < Skeleton::kMaxBones; ++i) {
            static constexpr float kIdentity[16] = {
                1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
            };
            std::memcpy(outMatrices + i * 16, kIdentity, sizeof(float) * 16);
        }
    }

    // -------------------------------------------------------------------------
    // Private helpers
    // -------------------------------------------------------------------------

    void AnimationController::EvaluateLocalPoses(const SkeletonAnimation& anim, float t)
    {
        const uint32_t n = m_skeleton->boneCount;
        for (uint32_t i = 0; i < n; ++i) {
            if (i >= anim.boneTracks.size()) continue;
            const BoneTrack& track = anim.boneTracks[i];

            if (!track.translation.empty())
                SampleVec3(track.translation, t, m_localPoses[i].t);
            if (!track.rotation.empty())
                SampleQuat(track.rotation, t, m_localPoses[i].r);
            if (!track.scale.empty())
                SampleVec3(track.scale, t, m_localPoses[i].s);
        }
    }

    void AnimationController::ComputeGlobalMatrices()
    {
        const uint32_t n = m_skeleton->boneCount;

        // Bones must be sorted parent-before-child (glTF guarantee)
        for (uint32_t i = 0; i < n; ++i) {
            float local[16];
            TrsToMatrix(m_localPoses[i].t, m_localPoses[i].r, m_localPoses[i].s, local);

            const int32_t parent = m_skeleton->parentIndex[i];
            if (parent < 0) {
                std::memcpy(m_globalMatrices[i].data(), local, sizeof(float) * 16);
            } else {
                Mul4x4(m_globalMatrices[parent].data(), local, m_globalMatrices[i].data());
            }

            Mul4x4(m_globalMatrices[i].data(), m_skeleton->inverseBindPose[i], m_boneMatrices[i].data());
        }
    }

    // Column-major 4x4 multiply: out = a * b
    void AnimationController::Mul4x4(const float* a, const float* b, float* out)
    {
        float tmp[16];
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                    sum += a[k * 4 + row] * b[col * 4 + k];
                tmp[col * 4 + row] = sum;
            }
        }
        std::memcpy(out, tmp, sizeof(float) * 16);
    }

    // Build column-major TRS matrix from separate T, R (quaternion), S components
    void AnimationController::TrsToMatrix(const float* t, const float* r, const float* s, float* out)
    {
        // r = [x, y, z, w] quaternion
        const float x = r[0], y = r[1], z = r[2], w = r[3];
        const float x2 = x*x, y2 = y*y, z2 = z*z;
        const float xy = x*y, xz = x*z, yz = y*z;
        const float wx = w*x, wy = w*y, wz = w*z;

        // Rotation matrix (column-major)
        out[ 0] = (1.0f - 2.0f*(y2+z2)) * s[0];
        out[ 1] = (2.0f*(xy+wz))         * s[0];
        out[ 2] = (2.0f*(xz-wy))         * s[0];
        out[ 3] = 0.0f;

        out[ 4] = (2.0f*(xy-wz))         * s[1];
        out[ 5] = (1.0f - 2.0f*(x2+z2)) * s[1];
        out[ 6] = (2.0f*(yz+wx))         * s[1];
        out[ 7] = 0.0f;

        out[ 8] = (2.0f*(xz+wy))         * s[2];
        out[ 9] = (2.0f*(yz-wx))         * s[2];
        out[10] = (1.0f - 2.0f*(x2+y2)) * s[2];
        out[11] = 0.0f;

        out[12] = t[0];
        out[13] = t[1];
        out[14] = t[2];
        out[15] = 1.0f;
    }

    // Linear interpolation of vec3 keyframes
    void AnimationController::SampleVec3(const std::vector<AnimKeyframe>& keys, float t, float* out)
    {
        if (keys.empty()) return;
        if (t <= keys.front().time) { std::memcpy(out, keys.front().value, sizeof(float)*3); return; }
        if (t >= keys.back().time)  { std::memcpy(out, keys.back().value,  sizeof(float)*3); return; }

        // Binary search for the segment containing t
        auto it = std::upper_bound(keys.begin(), keys.end(), t,
            [](float v, const AnimKeyframe& k) { return v < k.time; });
        const AnimKeyframe& b = *it;
        const AnimKeyframe& a = *(it - 1);

        const float alpha = (t - a.time) / (b.time - a.time);
        for (int i = 0; i < 3; ++i)
            out[i] = a.value[i] + alpha * (b.value[i] - a.value[i]);
    }

    // Spherical linear interpolation of quaternion keyframes
    void AnimationController::SampleQuat(const std::vector<AnimKeyframe>& keys, float t, float* out)
    {
        if (keys.empty()) return;
        if (t <= keys.front().time) { std::memcpy(out, keys.front().value, sizeof(float)*4); return; }
        if (t >= keys.back().time)  { std::memcpy(out, keys.back().value,  sizeof(float)*4); return; }

        auto it = std::upper_bound(keys.begin(), keys.end(), t,
            [](float v, const AnimKeyframe& k) { return v < k.time; });
        const AnimKeyframe& b = *it;
        const AnimKeyframe& a = *(it - 1);

        const float alpha = (t - a.time) / (b.time - a.time);
        SlerpQuat(a.value, b.value, alpha, out);
    }

    void AnimationController::SlerpQuat(const float* a, const float* b, float t, float* out)
    {
        float dot = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];

        // Choose shortest path
        float bx = b[0], by = b[1], bz = b[2], bw = b[3];
        if (dot < 0.0f) { bx=-bx; by=-by; bz=-bz; bw=-bw; dot=-dot; }

        // Fall back to lerp for nearly-parallel quaternions
        if (dot > 0.9995f) {
            out[0] = a[0] + t*(bx - a[0]);
            out[1] = a[1] + t*(by - a[1]);
            out[2] = a[2] + t*(bz - a[2]);
            out[3] = a[3] + t*(bw - a[3]);
            NormalizeQuat(out);
            return;
        }

        const float theta0 = std::acos(dot);
        const float theta  = theta0 * t;
        const float sinT0  = std::sin(theta0);
        const float sinT   = std::sin(theta);
        const float s0     = std::cos(theta) - dot * sinT / sinT0;
        const float s1     = sinT / sinT0;

        out[0] = s0*a[0] + s1*bx;
        out[1] = s0*a[1] + s1*by;
        out[2] = s0*a[2] + s1*bz;
        out[3] = s0*a[3] + s1*bw;
    }

    void AnimationController::NormalizeQuat(float* q)
    {
        const float len = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
        if (len > 1e-6f) { q[0]/=len; q[1]/=len; q[2]/=len; q[3]/=len; }
    }
}
