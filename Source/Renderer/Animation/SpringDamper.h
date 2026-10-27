#pragma once
#include "Renderer/Animation/AnimMath.h"

// Exact (closed-form) damped springs, after Daniel Holden, "Spring-It-On: The Game Developer's
// Spring-Roll-Call" (theorangeduck.com, 2021). Halflife = time for the distance to the goal to halve.
// Being closed form, every function is stable for any dt and frame-rate independent.
namespace SasamiEngine::Render::Anim
{
    // 1 / (1 + x + 0.48x^2 + 0.235x^3) ~= exp(-x) for x >= 0.
    float FastNegExp(float x);
    // damping = 4 ln2 / (halflife + eps)
    float HalflifeToDamping(float halflife, float eps = 1e-5f);
    float DampingToHalflife(float damping, float eps = 1e-5f);

    // Critically damped spring towards a fixed goal (velocity goal 0).
    void SimpleSpringDamper(float& x, float& v, float goal, float halflife, float dt);
    void SimpleSpringDamper(Vec3& x, Vec3& v, const Vec3& goal, float halflife, float dt);
    // Rotation version: `v` is an angular velocity (scaled angle axis per second).
    void SimpleSpringDamperQuat(Quat& x, Vec3& v, const Quat& goal, float halflife, float dt);

    // Critically damped spring towards a goal that itself moves at `vGoal`.
    void CriticalSpringDamper(float& x, float& v, float xGoal, float vGoal, float halflife, float dt);

    // Spring towards zero: used to decay inertialization offsets.
    void DecaySpringDamper(float& x, float& v, float halflife, float dt);
    void DecaySpringDamper(Vec3& x, Vec3& v, float halflife, float dt);
    // `x` is a rotation offset, `v` its angular velocity; decays towards identity.
    void DecaySpringDamperQuat(Quat& x, Vec3& v, float halflife, float dt);

    // Holden's "spring character controller": position x, velocity v and acceleration a follow
    // the desired velocity `vGoal` with a critically damped response.
    void CharacterSpringUpdate(Vec3& x, Vec3& v, Vec3& a, const Vec3& vGoal, float halflife, float dt);

    // Future positions of the same controller, Clavet-style trajectory for motion matching or
    // debug drawing. outPositions[i] is the position at time (i + 1) * stepDt. Inputs are not changed.
    void PredictTrajectory(const Vec3& x, const Vec3& v, const Vec3& a, const Vec3& vGoal, float halflife,
                           float stepDt, Vec3* outPositions, int count);
    // Future facings of a SimpleSpringDamperQuat towards `goal`; outRotations[i] at (i + 1) * stepDt.
    void PredictRotations(const Quat& x, const Vec3& v, const Quat& goal, float halflife, float stepDt,
                          Quat* outRotations, int count);
}

namespace SasamiEngine { using namespace Render; }
