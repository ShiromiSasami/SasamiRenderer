#pragma once
#include "Renderer/Animation/AnimPose.h"

#include <cstddef>

// Inertialization (Bollo, "Inertialization: High-Performance Animation Transitions in Gears of War",
// GDC 2018), in the offset + decay-spring form of Holden ("Dead Blending" / spring-roll-call).
// Instead of evaluating two animations during a transition (crossfade), the switch happens at once and
// the discontinuity (position and velocity) is stored as a per-bone offset that springs back to zero.
namespace SasamiEngine::Render::Anim
{
    // linear = (curr.t - prev.t) / dt, angular = ToScaledAngleAxis(abs(curr.r * inv(prev.r))) / dt.
    // dt <= 0 gives zero velocities. `out` is resized.
    void ComputePoseVelocity(const LocalPose& prev, const LocalPose& curr, float dt, PoseVelocity& out);

    class PoseInertializer
    {
    public:
        // Clears every offset (no transition in flight) and sizes the buffers.
        void Reset(size_t boneCount);

        // Call at the instant of the switch.
        //   displayedPose/displayedVel: what was shown last (the previous output, offsets included).
        //   targetPose/targetVel: the new source evaluated at the same instant.
        // Offsets become displayed - target, so the next Update output starts exactly at the
        // displayed pose and velocity and then decays towards the new source.
        void Transition(const LocalPose& displayedPose, const PoseVelocity& displayedVel,
                        const LocalPose& targetPose, const PoseVelocity& targetVel);

        // Decays the offsets by `dt` with DecaySpringDamper(halflife) and writes out = in + offset
        // (rotation: out.r = offset.r * in.r). Scale passes through. `out` may alias `in`.
        void Update(const LocalPose& in, float halflife, float dt, LocalPose& out);

        // False once every offset is below `threshold` (position units / radians).
        bool IsActive(float threshold = 1e-4f) const;

    private:
        std::vector<Vec3> m_offsetT;
        std::vector<Vec3> m_offsetTVel;
        std::vector<Quat> m_offsetR;
        std::vector<Vec3> m_offsetRVel;
    };
}

namespace SasamiEngine { using namespace Render; }
