#pragma once
#include "Renderer/Animation/AnimMath.h"

#include <functional>

// Third-person orbit camera with a spring arm (UE USpringArmComponent: probe, lag, shoulder offset).
// The arm shortens instantly when blocked and extends back with a spring, so the camera never
// clips through walls but does not pop outward either.
namespace SasamiEngine::App::Gameplay
{
    using Render::Anim::Vec3;

    // Sphere sweep from `from` to `to`; returns the free fraction in [0, 1] (1 = no hit).
    using CameraCollisionQuery = std::function<float(const Vec3& from, const Vec3& to, float radius)>;

    struct FollowCameraSettings
    {
        float distance = 3.5f;
        float minDistance = 0.4f;
        float maxDistance = 8.0f;
        float minPitch = -1.2f;             // radians, negative = looking up from below
        float maxPitch = 1.2f;
        float pivotHeight = 1.5f;           // above the followed position
        float shoulderOffset = 0.35f;       // to the camera's right
        float pivotHalflife = 0.08f;        // lag of the pivot behind the target
        float armExtendHalflife = 0.25f;    // how fast the arm grows back after a collision
        float collisionRadius = 0.2f;
    };

    class FollowCamera
    {
    public:
        void SetSettings(const FollowCameraSettings& settings) { m_settings = settings; }
        FollowCameraSettings& Settings() { return m_settings; }
        void SetCollisionQuery(CameraCollisionQuery query) { m_collision = std::move(query); }

        // yaw/pitch deltas in radians (mouse or stick, already scaled by sensitivity).
        void AddOrbitInput(float deltaYaw, float deltaPitch);
        void AddZoomInput(float deltaDistance);
        void SetOrbit(float yaw, float pitch);

        // Places the pivot and arm on the target without lag.
        void Snap(const Vec3& targetPosition);
        void Update(float dt, const Vec3& targetPosition);

        Vec3 Position() const { return m_position; }
        Vec3 LookAt() const { return m_lookAt; }
        Vec3 Forward() const;
        float Yaw() const { return m_yaw; }
        float Pitch() const { return m_pitch; }
        float ArmLength() const { return m_armLength; }

    private:
        void Solve(float dt, bool snap);

        FollowCameraSettings m_settings;
        CameraCollisionQuery m_collision;
        float m_yaw = 0.0f;
        float m_pitch = 0.3f;
        Vec3 m_target{};
        Vec3 m_pivot{};
        Vec3 m_pivotVelocity{};
        float m_armLength = 3.5f;
        float m_armVelocity = 0.0f;
        Vec3 m_position{};
        Vec3 m_lookAt{};
    };
}

namespace SasamiEngine { using namespace App; }
