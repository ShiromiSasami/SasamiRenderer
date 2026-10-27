#include "Gameplay/CharacterMovement.h"

#include "Renderer/Animation/SpringDamper.h"

#include <algorithm>
#include <cmath>

// References: Holden, "Spring-It-On: The Game Developer's Spring-Roll-Call" and "Code vs Data Driven
// Displacement" (theorangeduck.com); UE USpringArmComponent for the camera side of the same feature set.
namespace SasamiEngine::App::Gameplay
{
    using namespace Render::Anim;

    void CharacterMovement::Teleport(const Vec3& position, float yaw)
    {
        m_position = position;
        m_rotation = QuatFromYaw(yaw);
        m_velocity = {};
        m_acceleration = {};
        m_desiredVelocity = {};
        m_angularVelocity = {};
        m_grounded = true;
        m_justJumped = false;
        m_justLanded = false;
        m_timeSinceGrounded = 0.0f;
    }

    Vec3 CharacterMovement::DesiredVelocityFromInput(const MovementInput& input) const
    {
        float sx = input.moveX;
        float sy = input.moveY;
        const float len = std::sqrt(sx * sx + sy * sy);
        if (len > 1.0f) {
            sx /= len;
            sy /= len;
        }
        const Vec3 worldDir = QuatRotate(QuatFromYaw(input.cameraYaw), { sx, 0.0f, sy });
        const float speed = input.sprint ? m_settings.runSpeed : m_settings.walkSpeed;
        return worldDir * speed;
    }

    void CharacterMovement::Update(float dt, const MovementInput& input)
    {
        if (dt <= 0.0f) {
            return;
        }
        m_justJumped = false;
        m_justLanded = false;

        m_desiredVelocity = DesiredVelocityFromInput(input);

        // Horizontal: spring on velocity, weaker while airborne.
        {
            const float halflife = m_grounded
                ? m_settings.velocityHalflife
                : m_settings.velocityHalflife / std::max(m_settings.airControl, 1e-3f);
            Vec3 posH{ m_position.x, 0.0f, m_position.z };
            Vec3 velH{ m_velocity.x, 0.0f, m_velocity.z };
            Vec3 accH{ m_acceleration.x, 0.0f, m_acceleration.z };
            const Vec3 desiredH{ m_desiredVelocity.x, 0.0f, m_desiredVelocity.z };
            CharacterSpringUpdate(posH, velH, accH, desiredH, halflife, dt);
            m_position.x = posH.x;
            m_position.z = posH.z;
            m_velocity.x = velH.x;
            m_velocity.z = velH.z;
            m_acceleration = accH;
        }

        // Facing.
        {
            float targetYaw = Yaw();
            if (input.strafe) {
                targetYaw = input.cameraYaw;
            } else if (std::sqrt(m_desiredVelocity.x * m_desiredVelocity.x + m_desiredVelocity.z * m_desiredVelocity.z) > 0.05f) {
                targetYaw = std::atan2(m_desiredVelocity.x, m_desiredVelocity.z);
            }
            SimpleSpringDamperQuat(m_rotation, m_angularVelocity, QuatFromYaw(targetYaw), m_settings.rotationHalflife, dt);
        }

        // Vertical.
        m_timeSinceGrounded = m_grounded ? 0.0f : m_timeSinceGrounded + dt;
        if (input.jump && (m_grounded || m_timeSinceGrounded <= m_settings.coyoteTime)) {
            m_velocity.y = m_settings.jumpSpeed;
            m_grounded = false;
            m_justJumped = true;
            m_timeSinceGrounded = m_settings.coyoteTime + 1.0f;
        }

        const float previousY = m_position.y;
        if (!m_grounded) {
            m_position.y += m_velocity.y * dt + 0.5f * m_settings.gravity * dt * dt;
            m_velocity.y += m_settings.gravity * dt;
        }

        float groundHeight = 0.0f;
        bool hasGround = true;
        if (m_ground) {
            hasGround = m_ground(m_position.x, m_position.z, groundHeight);
        }
        if (m_grounded) {
            if (hasGround
                && groundHeight >= m_position.y - m_settings.groundSnap
                && groundHeight <= m_position.y + m_settings.stepHeight) {
                m_position.y = groundHeight;
                m_velocity.y = 0.0f;
            } else {
                m_grounded = false;
            }
        } else if (hasGround
                   && m_velocity.y <= 0.0f
                   && m_position.y <= groundHeight
                   && groundHeight <= previousY + m_settings.stepHeight) {
            m_position.y = groundHeight;
            m_velocity.y = 0.0f;
            m_grounded = true;
            m_justLanded = true;
            m_timeSinceGrounded = 0.0f;
        }
    }

    float CharacterMovement::Yaw() const
    {
        return QuatYaw(m_rotation);
    }

    float CharacterMovement::HorizontalSpeed() const
    {
        return std::sqrt(m_velocity.x * m_velocity.x + m_velocity.z * m_velocity.z);
    }

    void CharacterMovement::PredictTrajectory(Vec3* outPositions, int count, float stepDt) const
    {
        if (outPositions == nullptr || count <= 0) {
            return;
        }
        const Vec3 posH{ m_position.x, 0.0f, m_position.z };
        const Vec3 velH{ m_velocity.x, 0.0f, m_velocity.z };
        const Vec3 accH{ m_acceleration.x, 0.0f, m_acceleration.z };
        const Vec3 desiredH{ m_desiredVelocity.x, 0.0f, m_desiredVelocity.z };
        Render::Anim::PredictTrajectory(posH, velH, accH, desiredH, m_settings.velocityHalflife, stepDt, outPositions, count);
        for (int i = 0; i < count; ++i) {
            outPositions[i].y = m_position.y;
        }
    }
}
