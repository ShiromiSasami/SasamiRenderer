#pragma once
#include <array>
#include <cassert>
#include <windows.h>
#include "Object/SObject.h"
#include "AppFramework/Component/TransformComponent.h"
#include "Renderer/Scene/RenderCameraProxy.h"

namespace SasamiEngine::App
{
    class Camera : public SObject {
    public:
        using CameraMode = RenderCameraMode;
        Camera() { AddComponent<TransformComponent>(); }

        void SetYawPitch(float yaw, float pitch) {
            m_yaw = yaw; 
            m_pitch = ClampPitch(pitch);
            Transform().rotation.pitch = m_pitch;
            Transform().rotation.yaw = m_yaw;
        }
        void AddYawPitch(float dyaw, float dpitch) {
            m_yaw += dyaw; 
            m_pitch = ClampPitch(m_pitch + dpitch);
            Transform().rotation.pitch = m_pitch;
            Transform().rotation.yaw = m_yaw;
        }
        void SetDistance(float d) { m_distance = ClampDistance(d); }
        void AddDistance(float dd) { m_distance = ClampDistance(m_distance + dd); }
        void SetTarget(float x, float y, float z) { Transform().position = Vector3(x, y, z); }
        void AddTarget(float dx, float dy, float dz) { Transform().position += Vector3(dx, dy, dz); }
        TransformComponent& Transform()
        {
            TransformComponent* component = GetComponent<TransformComponent>();
            assert(component && "Camera requires TransformComponent.");
            return *component;
        }
        const TransformComponent& Transform() const
        {
            const TransformComponent* component = GetComponent<TransformComponent>();
            assert(component && "Camera requires TransformComponent.");
            return *component;
        }

        float Yaw() const { return m_yaw; }
        float Pitch() const { return m_pitch; }
        float Distance() const { return m_distance; }
        void SetClipPlanes(float nearZ, float farZ)
        {
            m_nearClip = ClampNear(nearZ);
            m_farClip = ClampFar(farZ, m_nearClip);
        }
        float NearClip() const { return m_nearClip; }
        float FarClip() const { return m_farClip; }
        void SetCameraMode(CameraMode mode) { m_cameraMode = mode; }
        CameraMode GetCameraMode() const { return m_cameraMode; }

        std::array<float,16> ComputeMVP(float viewportWidth, float viewportHeight) const;
        RenderCameraProxy BuildRenderCameraProxy(float viewportWidth, float viewportHeight) const;
        RenderCameraProxy BuildRenderCameraProxy(float viewportWidth,
                                                 float viewportHeight,
                                                 CameraMode mode) const;

        void OnKeyDown(WPARAM key);
        void OnKeyUp(WPARAM key);
        void OnMouseDown(int x, int y);
        void OnMouseUp();
        void OnMouseMove(int x, int y, bool rotateButtonHeld);
        void OnMouseWheel(int delta);
        void Update(float deltaTime);
        void SetMoveSpeed(float speed) { m_moveSpeed = speed; }
        float MoveSpeed() const { return m_moveSpeed; }

    private:
        constexpr static float ClampPitch(float p) {
            const float limit = 1.55f;
            if (p > limit) return limit; 
            if (p < -limit) return -limit; 
            return p;
        }
        constexpr static float ClampDistance(float d) {
            if (d < 0.05f) return 0.05f; 
            if (d > 200.0f) return 200.0f; 
            return d;
        }
        constexpr static float ClampNear(float nearZ)
        {
            if (nearZ < 0.0001f) return 0.0001f;
            return nearZ;
        }
        constexpr static float ClampFar(float farZ, float nearZ)
        {
            const float minFar = nearZ + 0.001f;
            if (farZ < minFar) return minFar;
            return farZ;
        }

        float m_yaw = 0.0f;
        float m_pitch = 0.0f;
        float m_distance = 2.5f;
        bool m_rotating = false;
        bool m_moveForward = false;
        bool m_moveBackward = false;
        bool m_moveLeft = false;
        bool m_moveRight = false;
        float m_moveSpeed = 1.0f;
        // 0.05, not the 0.0005 this used to default to. With far at 500 that old value gave a
        // near/far ratio of 1e6, and non-reversed depth spends nearly all of its precision just
        // in front of the near plane -- almost none was left at the 10-100m range where the
        // scene actually is. Coplanar detail (window frames, cornices, decals) Z-fought, which
        // showed up as dashed black outlines and as speckle in the Normal/NdotL debug views.
        // Verified by sweeping this value from one fixed camera pose: the artifacts are dense
        // at 0.0005 and essentially gone by 0.05 (Tools/Debug/sweep-clip-shadow.ps1).
        // Raise this further if precision is still short; lower it only for a scene that truly
        // needs sub-centimetre geometry right at the lens.
        float m_nearClip = 0.05f;
        float m_farClip = 500.0f;
        CameraMode m_cameraMode = CameraMode::Pbr;
        int m_lastX = 0;
        int m_lastY = 0;
    };

}
namespace SasamiEngine { using namespace App; }
