#include "Gameplay/FollowCamera.h"

#include "Renderer/Animation/SpringDamper.h"

#include <algorithm>

// References: UE USpringArmComponent (probe, lag, shoulder offset); Holden, "Spring-It-On" for the
// critically damped pivot and arm-extension springs.
namespace SasamiEngine::App::Gameplay
{
    using namespace Render::Anim;

    void FollowCamera::AddOrbitInput(float deltaYaw, float deltaPitch)
    {
        m_yaw = WrapAngle(m_yaw + deltaYaw);
        m_pitch = std::clamp(m_pitch + deltaPitch, m_settings.minPitch, m_settings.maxPitch);
    }

    void FollowCamera::AddZoomInput(float deltaDistance)
    {
        m_settings.distance = std::clamp(m_settings.distance + deltaDistance, m_settings.minDistance, m_settings.maxDistance);
    }

    void FollowCamera::SetOrbit(float yaw, float pitch)
    {
        m_yaw = WrapAngle(yaw);
        m_pitch = std::clamp(pitch, m_settings.minPitch, m_settings.maxPitch);
    }

    void FollowCamera::Snap(const Vec3& targetPosition)
    {
        m_target = targetPosition;
        Solve(0.0f, true);
    }

    void FollowCamera::Update(float dt, const Vec3& targetPosition)
    {
        m_target = targetPosition;
        Solve(dt, false);
    }

    void FollowCamera::Solve(float dt, bool snap)
    {
        const Vec3 desiredPivot = m_target + Vec3{ 0.0f, m_settings.pivotHeight, 0.0f };
        if (snap) {
            m_pivot = desiredPivot;
            m_pivotVelocity = {};
        } else if (dt > 0.0f) {
            SimpleSpringDamper(m_pivot, m_pivotVelocity, desiredPivot, m_settings.pivotHalflife, dt);
        }

        // Positive pitch looks down at the target, so the camera sits above it.
        const Quat rot = QuatMul(QuatFromYaw(m_yaw), QuatFromAxisAngle({ 1.0f, 0.0f, 0.0f }, m_pitch));
        const Vec3 forward = QuatRotate(rot, { 0.0f, 0.0f, 1.0f });
        const Vec3 right = QuatRotate(rot, { 1.0f, 0.0f, 0.0f });

        const Vec3 shoulderPivot = m_pivot + right * m_settings.shoulderOffset;
        const float desiredLen = m_settings.distance;

        float allowed = desiredLen;
        if (m_collision) {
            const float fraction = std::clamp(
                m_collision(m_pivot, shoulderPivot - forward * desiredLen, m_settings.collisionRadius), 0.0f, 1.0f);
            allowed = desiredLen * fraction;
        }

        if (snap || allowed < m_armLength) {
            m_armLength = allowed;
            m_armVelocity = 0.0f;
        } else if (dt > 0.0f) {
            SimpleSpringDamper(m_armLength, m_armVelocity, allowed, m_settings.armExtendHalflife, dt);
        }
        m_armLength = std::clamp(m_armLength, 0.0f, desiredLen);

        m_position = shoulderPivot - forward * m_armLength;
        m_lookAt = shoulderPivot + forward * 1.0f;
    }

    Vec3 FollowCamera::Forward() const
    {
        return Normalize(m_lookAt - m_position, { 0.0f, 0.0f, 1.0f });
    }
}
