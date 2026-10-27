#pragma once
#include "Renderer/Animation/AnimMath.h"

#include <functional>

// Third-person character locomotion driven by critically damped springs (Holden, "Spring-It-On" and
// "Code vs Data Driven Displacement", theorangeduck.com): input -> desired velocity -> spring on
// velocity and facing, plus gravity/jump. Collision against geometry is limited to a ground query.
namespace SasamiEngine::App::Gameplay
{
    using Render::Anim::Quat;
    using Render::Anim::Vec3;

    // World-space vertical query at (x, z). Returns false when there is no ground.
    using GroundQuery = std::function<bool(float x, float z, float& outHeight)>;

    struct MovementInput
    {
        float moveX = 0.0f;         // stick right, [-1, 1]
        float moveY = 0.0f;         // stick forward, [-1, 1]
        float cameraYaw = 0.0f;     // input is camera relative: forward = rotateY(cameraYaw) * +Z
        bool sprint = false;
        bool jump = false;          // edge: true on the frame the button went down
        bool strafe = false;        // face the camera yaw instead of the movement direction
    };

    struct CharacterMovementSettings
    {
        float walkSpeed = 1.6f;
        float runSpeed = 4.0f;
        float velocityHalflife = 0.2f;
        float rotationHalflife = 0.12f;
        float airControl = 0.3f;        // multiplies the velocity spring strength while airborne
        float gravity = -9.81f;
        float jumpSpeed = 4.5f;
        float stepHeight = 0.3f;        // ground this far above the feet is stepped onto
        float groundSnap = 0.2f;        // ground this far below the feet is snapped to while grounded
        float coyoteTime = 0.1f;        // jump still allowed this long after leaving the ground
    };

    class CharacterMovement
    {
    public:
        void SetSettings(const CharacterMovementSettings& settings) { m_settings = settings; }
        const CharacterMovementSettings& Settings() const { return m_settings; }
        void SetGroundQuery(GroundQuery query) { m_ground = std::move(query); }

        void Teleport(const Vec3& position, float yaw);
        void Update(float dt, const MovementInput& input);

        Vec3 Position() const { return m_position; }
        Vec3 Velocity() const { return m_velocity; }
        float Yaw() const;
        Quat Rotation() const { return m_rotation; }
        float HorizontalSpeed() const;
        Vec3 DesiredVelocity() const { return m_desiredVelocity; }
        bool Grounded() const { return m_grounded; }
        bool JustJumped() const { return m_justJumped; }
        bool JustLanded() const { return m_justLanded; }

        // Future horizontal positions under the current desired velocity; out[i] at (i + 1) * step.
        void PredictTrajectory(Vec3* outPositions, int count, float stepDt) const;

    private:
        Vec3 DesiredVelocityFromInput(const MovementInput& input) const;

        CharacterMovementSettings m_settings;
        GroundQuery m_ground;
        Vec3 m_position{};
        Vec3 m_velocity{};          // horizontal part is spring driven, y is ballistic
        Vec3 m_acceleration{};      // horizontal spring acceleration
        Vec3 m_desiredVelocity{};
        Quat m_rotation{};
        Vec3 m_angularVelocity{};
        bool m_grounded = true;
        bool m_justJumped = false;
        bool m_justLanded = false;
        float m_timeSinceGrounded = 0.0f;
    };
}

namespace SasamiEngine { using namespace App; }
