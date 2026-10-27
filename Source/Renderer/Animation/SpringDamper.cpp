#include "Renderer/Animation/SpringDamper.h"

#include <cmath>

// Closed-form springs after Daniel Holden, "Spring-It-On: The Game Developer's Spring-Roll-Call"
// (theorangeduck.com, 2021).
namespace SasamiEngine::Render::Anim
{
    namespace
    {
        constexpr float LN2 = 0.69314718056f;

        // Exact exp, not FastNegExp: the closed forms are only step-size independent (one dt step
        // equals ten dt/10 steps, and PredictTrajectory equals stepping) with the true exponential.
        float NegExp(float x)
        {
            return std::exp(-x);
        }

        void CharacterSpringComponent(float& x, float& v, float& a, float vGoal, float y, float dt)
        {
            const float j0 = v - vGoal;
            const float j1 = a + j0 * y;
            const float iy = 1.0f / y;
            const float e = NegExp(y * dt);

            x = e * (((-j1) * iy * iy) + ((-j0 - j1 * dt) * iy)) + (j1 * iy * iy) + (j0 * iy) + vGoal * dt + x;
            v = e * (j0 + j1 * dt) + vGoal;
            a = e * (a - j1 * y * dt);
        }
    }

    float FastNegExp(float x)
    {
        return 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    }

    float HalflifeToDamping(float halflife, float eps)
    {
        return (4.0f * LN2) / (halflife + eps);
    }

    float DampingToHalflife(float damping, float eps)
    {
        return (4.0f * LN2) / (damping + eps);
    }

    void SimpleSpringDamper(float& x, float& v, float goal, float halflife, float dt)
    {
        const float y = HalflifeToDamping(halflife) / 2.0f;
        const float j0 = x - goal;
        const float j1 = v + j0 * y;
        const float e = NegExp(y * dt);

        x = e * (j0 + j1 * dt) + goal;
        v = e * (v - j1 * y * dt);
    }

    void SimpleSpringDamper(Vec3& x, Vec3& v, const Vec3& goal, float halflife, float dt)
    {
        SimpleSpringDamper(x.x, v.x, goal.x, halflife, dt);
        SimpleSpringDamper(x.y, v.y, goal.y, halflife, dt);
        SimpleSpringDamper(x.z, v.z, goal.z, halflife, dt);
    }

    void SimpleSpringDamperQuat(Quat& x, Vec3& v, const Quat& goal, float halflife, float dt)
    {
        const float y = HalflifeToDamping(halflife) / 2.0f;
        const Vec3 j0 = QuatToScaledAngleAxis(QuatAbs(QuatMul(x, QuatInverse(goal))));
        const Vec3 j1 = v + j0 * y;
        const float e = NegExp(y * dt);

        x = QuatMul(QuatFromScaledAngleAxis((j0 + j1 * dt) * e), goal);
        v = (v - j1 * (y * dt)) * e;
    }

    void CriticalSpringDamper(float& x, float& v, float xGoal, float vGoal, float halflife, float dt)
    {
        const float d = HalflifeToDamping(halflife);
        const float c = xGoal + (d * vGoal) / ((d * d) / 4.0f);
        const float y = d / 2.0f;
        const float j0 = x - c;
        const float j1 = v + j0 * y;
        const float e = NegExp(y * dt);

        x = e * (j0 + j1 * dt) + c;
        v = e * (v - j1 * y * dt);
    }

    void DecaySpringDamper(float& x, float& v, float halflife, float dt)
    {
        const float y = HalflifeToDamping(halflife) / 2.0f;
        const float j1 = v + x * y;
        const float e = NegExp(y * dt);

        x = e * (x + j1 * dt);
        v = e * (v - j1 * y * dt);
    }

    void DecaySpringDamper(Vec3& x, Vec3& v, float halflife, float dt)
    {
        DecaySpringDamper(x.x, v.x, halflife, dt);
        DecaySpringDamper(x.y, v.y, halflife, dt);
        DecaySpringDamper(x.z, v.z, halflife, dt);
    }

    void DecaySpringDamperQuat(Quat& x, Vec3& v, float halflife, float dt)
    {
        const float y = HalflifeToDamping(halflife) / 2.0f;
        const Vec3 j0 = QuatToScaledAngleAxis(QuatAbs(x));
        const Vec3 j1 = v + j0 * y;
        const float e = NegExp(y * dt);

        x = QuatFromScaledAngleAxis((j0 + j1 * dt) * e);
        v = (v - j1 * (y * dt)) * e;
    }

    void CharacterSpringUpdate(Vec3& x, Vec3& v, Vec3& a, const Vec3& vGoal, float halflife, float dt)
    {
        const float y = HalflifeToDamping(halflife) / 2.0f;
        CharacterSpringComponent(x.x, v.x, a.x, vGoal.x, y, dt);
        CharacterSpringComponent(x.y, v.y, a.y, vGoal.y, y, dt);
        CharacterSpringComponent(x.z, v.z, a.z, vGoal.z, y, dt);
    }

    void PredictTrajectory(const Vec3& x, const Vec3& v, const Vec3& a, const Vec3& vGoal, float halflife,
                           float stepDt, Vec3* outPositions, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            Vec3 px = x;
            Vec3 pv = v;
            Vec3 pa = a;
            CharacterSpringUpdate(px, pv, pa, vGoal, halflife, static_cast<float>(i + 1) * stepDt);
            outPositions[i] = px;
        }
    }

    void PredictRotations(const Quat& x, const Vec3& v, const Quat& goal, float halflife, float stepDt,
                          Quat* outRotations, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            Quat px = x;
            Vec3 pv = v;
            SimpleSpringDamperQuat(px, pv, goal, halflife, static_cast<float>(i + 1) * stepDt);
            outRotations[i] = px;
        }
    }
}
