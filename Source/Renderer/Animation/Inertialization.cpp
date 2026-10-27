#include "Renderer/Animation/Inertialization.h"

#include "Renderer/Animation/SpringDamper.h"

#include <algorithm>
#include <cmath>

// Inertialization: Bollo, GDC 2018 "Inertialization: High-Performance Animation Transitions in Gears of War";
// offset + decay-spring form after Holden, "Spring-It-On" (theorangeduck.com).
namespace SasamiEngine::Render::Anim
{
    void ComputePoseVelocity(const LocalPose& prev, const LocalPose& curr, float dt, PoseVelocity& out)
    {
        const size_t n = curr.size();
        out.linear.assign(n, Vec3{});
        out.angular.assign(n, Vec3{});
        if (dt <= 0.0f)
            return;

        const float invDt = 1.0f / dt;
        const size_t count = std::min(n, prev.size());
        for (size_t i = 0; i < count; ++i)
        {
            out.linear[i] = (curr[i].t - prev[i].t) * invDt;
            out.angular[i] = QuatToScaledAngleAxis(QuatAbs(QuatMul(curr[i].r, QuatInverse(prev[i].r)))) * invDt;
        }
    }

    void PoseInertializer::Reset(size_t boneCount)
    {
        m_offsetT.assign(boneCount, Vec3{});
        m_offsetTVel.assign(boneCount, Vec3{});
        m_offsetR.assign(boneCount, Quat{});
        m_offsetRVel.assign(boneCount, Vec3{});
    }

    void PoseInertializer::Transition(const LocalPose& displayedPose, const PoseVelocity& displayedVel,
                                      const LocalPose& targetPose, const PoseVelocity& targetVel)
    {
        const size_t n = targetPose.size();
        if (m_offsetT.size() != n)
            Reset(n);

        for (size_t i = 0; i < n; ++i)
        {
            const Transform& target = targetPose[i];
            const Transform displayed = i < displayedPose.size() ? displayedPose[i] : target;

            const Vec3 dispLin = i < displayedVel.linear.size() ? displayedVel.linear[i] : Vec3{};
            const Vec3 dispAng = i < displayedVel.angular.size() ? displayedVel.angular[i] : Vec3{};
            const Vec3 targLin = i < targetVel.linear.size() ? targetVel.linear[i] : Vec3{};
            const Vec3 targAng = i < targetVel.angular.size() ? targetVel.angular[i] : Vec3{};

            m_offsetT[i] = displayed.t - target.t;
            m_offsetTVel[i] = dispLin - targLin;
            m_offsetR[i] = QuatAbs(QuatMul(displayed.r, QuatInverse(target.r)));
            m_offsetRVel[i] = dispAng - targAng;
        }
    }

    void PoseInertializer::Update(const LocalPose& in, float halflife, float dt, LocalPose& out)
    {
        const size_t n = in.size();
        if (m_offsetT.size() != n)
            Reset(n);

        out.resize(n);
        for (size_t i = 0; i < n; ++i)
        {
            DecaySpringDamper(m_offsetT[i], m_offsetTVel[i], halflife, dt);
            DecaySpringDamperQuat(m_offsetR[i], m_offsetRVel[i], halflife, dt);

            const Transform src = in[i];
            out[i].t = src.t + m_offsetT[i];
            out[i].r = QuatNormalize(QuatMul(m_offsetR[i], src.r));
            out[i].s = src.s;
        }
    }

    bool PoseInertializer::IsActive(float threshold) const
    {
        for (size_t i = 0; i < m_offsetT.size(); ++i)
        {
            if (Length(m_offsetT[i]) > threshold)
                return true;
            if (Length(QuatToScaledAngleAxis(QuatAbs(m_offsetR[i]))) > threshold)
                return true;
            if (Length(m_offsetTVel[i]) > threshold || Length(m_offsetRVel[i]) > threshold)
                return true;
        }
        return false;
    }
}
