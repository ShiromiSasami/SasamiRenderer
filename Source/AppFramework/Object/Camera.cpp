#include "Object/Camera.h"
#include <windows.h>
#include <cmath>
#include "Foundation/Math/MathUtil.h"

namespace SasamiRenderer
{
    using Math::Cross;
    using Math::Deg;
    using Math::Mul4x4;
    using Math::Normalize;

    namespace {
        static Vector3 ComputeForward(float yaw, float pitch)
        {
            // Spherical-angle style forward vector.
            // x = sin(yaw) * cos(pitch)
            // y = -sin(pitch)
            // z = cos(yaw) * cos(pitch)
            // (Sign convention is engine-specific: +pitch looks downward here.)
            float cy = std::cos(yaw), sy = std::sin(yaw);
            float cp = std::cos(pitch), sp = std::sin(pitch);
            return Vector3(sy * cp, -sp, cy * cp);
        }
    }

    std::array<float, 16> Camera::ComputeMVP(float viewportWidth, float viewportHeight) const {
        const TransformComponent& transform = Transform();
        Vector3 forward = ComputeForward(m_yaw, m_pitch);
        Normalize(forward);

        const Vector3 upWorld(0.0f, 1.0f, 0.0f);
        // Camera orthonormal basis:
        // right = normalize(upWorld x forward)
        Vector3 right = Cross(upWorld, forward);
        Normalize(right);

        // up = normalize(forward x right)
        Vector3 up = Cross(forward, right);
        Normalize(up);

        const float px = transform.position.x;
        const float py = transform.position.y;
        const float pz = transform.position.z;

        // View matrix (row-major, row-vector convention).
        // Rotational part uses camera basis vectors.
        // Translation part is -dot(cameraPos, basisAxis).
        float view[16] = {
            right.x, up.x, forward.x, 0.0f,
            right.y, up.y, forward.y, 0.0f,
            right.z, up.z, forward.z, 0.0f,
            -(px * right.x + py * right.y + pz * right.z),
            -(px * up.x + py * up.y + pz * up.z),
            -(px * forward.x + py * forward.y + pz * forward.z),
            1.0f,
        };

        // Perspective projection (row-major, LH).
        // yScale = 1 / tan(fov/2)
        // xScale = yScale / aspect
        // z mapping: z' = zf/(zf-zn) * z + (-zn*zf)/(zf-zn)
        const float safeHeight = (viewportHeight > 0.0f) ? viewportHeight : 1.0f;
        float aspect = viewportWidth / safeHeight;
        float fov = Deg(60.0f);
        float zn = m_nearClip;
        float zf = m_farClip;
        float yScale = 1.0f / std::tan(fov * 0.5f);
        float xScale = yScale / aspect;
        float proj[16] = {
            xScale, 0,      0,                          0,
            0,      yScale, 0,                          0,
            0,      0,      zf / (zf - zn),             1,
            0,      0,      (-zn * zf) / (zf - zn),     0,
        };

        // MVP = View * Projection (world is identity for camera-space computation here).
        float mvp[16]; Mul4x4(view, proj, mvp);
        std::array<float, 16> out{};
        for (int i = 0; i < 16; ++i) out[i] = mvp[i];
        return out;
    }

    RenderCameraProxy Camera::BuildRenderCameraProxy(float viewportWidth, float viewportHeight) const
    {
        return BuildRenderCameraProxy(viewportWidth, viewportHeight, m_cameraMode);
    }

    RenderCameraProxy Camera::BuildRenderCameraProxy(float viewportWidth,
                                                     float viewportHeight,
                                                     CameraMode mode) const
    {
        const TransformComponent& transform = Transform();
        RenderCameraProxy proxy{};
        proxy.cameraMode = mode;
        Vector3 forward = ComputeForward(m_yaw, m_pitch);
        Normalize(forward);
        const Vector3 upWorld(0.0f, 1.0f, 0.0f);
        Vector3 right = Cross(upWorld, forward);
        Normalize(right);
        Vector3 up = Cross(forward, right);
        Normalize(up);
        const auto vp = ComputeMVP(viewportWidth, viewportHeight);
        for (int i = 0; i < 16; ++i) {
            proxy.viewProjection[i] = vp[i];
        }
        proxy.cameraPosition[0] = transform.position.x;
        proxy.cameraPosition[1] = transform.position.y;
        proxy.cameraPosition[2] = transform.position.z;
        proxy.nearClip = m_nearClip;
        proxy.farClip = m_farClip;

        // Store the projection matrix separately (needed by SSAO pass for hemisphere sampling).
        const float safeHeight = (viewportHeight > 0.0f) ? viewportHeight : 1.0f;
        const float aspect = viewportWidth / safeHeight;
        const float fov = Deg(60.0f);
        const float zn = m_nearClip;
        const float zf = m_farClip;
        const float yScale = 1.0f / std::tan(fov * 0.5f);
        const float xScale = yScale / aspect;
        const float proj[16] = {
            xScale, 0,      0,                      0,
            0,      yScale, 0,                      0,
            0,      0,      zf / (zf - zn),         1,
            0,      0,      (-zn * zf) / (zf - zn), 0,
        };
        for (int i = 0; i < 16; ++i) {
            proxy.projection[i] = proj[i];
        }
        proxy.rayMarchCameraRight[0] = right.x;
        proxy.rayMarchCameraRight[1] = right.y;
        proxy.rayMarchCameraRight[2] = right.z;
        proxy.rayMarchCameraUp[0] = up.x;
        proxy.rayMarchCameraUp[1] = up.y;
        proxy.rayMarchCameraUp[2] = up.z;
        proxy.rayMarchCameraForward[0] = forward.x;
        proxy.rayMarchCameraForward[1] = forward.y;
        proxy.rayMarchCameraForward[2] = forward.z;
        proxy.rayMarchTanHalfFovY = std::tan(fov * 0.5f);
        proxy.rayMarchAspectRatio = aspect;

        return proxy;
    }
    // Input handling
    void Camera::OnKeyDown(WPARAM key)
    {
        const float step = 0.05f;       // radians
        switch (key) {
        case VK_LEFT:  AddYawPitch(-step, 0.0f); break;
        case VK_RIGHT: AddYawPitch(+step, 0.0f); break;
        case VK_UP:    AddYawPitch(0.0f, +step); break;
        case VK_DOWN:  AddYawPitch(0.0f, -step); break;
        case 'W':
        case 'w': m_moveForward = true; break;
        case 'S':
        case 's': m_moveBackward = true; break;
        case 'A':
        case 'a': m_moveLeft = true; break;
        case 'D':
        case 'd': m_moveRight = true; break;
        default: break;
        }
    }

    void Camera::OnKeyUp(WPARAM key)
    {
        switch (key) {
        case 'W':
        case 'w': m_moveForward = false; break;
        case 'S':
        case 's': m_moveBackward = false; break;
        case 'A':
        case 'a': m_moveLeft = false; break;
        case 'D':
        case 'd': m_moveRight = false; break;
        default: break;
        }
    }

    void Camera::OnMouseDown(int x, int y)
    {
        m_rotating = true;
        m_lastX = x; 
        m_lastY = y;
    }

    void Camera::OnMouseUp()
    {
        m_rotating = false;
    }

    void Camera::OnMouseMove(int x, int y, bool rotateButtonHeld)
    {
        if (!rotateButtonHeld || !m_rotating) {
            m_lastX = x;
            m_lastY = y;
            return;
        }
        int dx = x - m_lastX; 
        int dy = y - m_lastY;
        m_lastX = x;
        m_lastY = y;
        const bool pan = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (pan) {
            Vector3 forward = ComputeForward(m_yaw, m_pitch);
            const Vector3 upWorld(0.0f, 1.0f, 0.0f);
            // Pan axes built from current view basis.
            Vector3 right = Cross(upWorld, forward);
            Normalize(right);
            Vector3 up = Cross(right, forward);
            Normalize(up);
            // Scale by camera distance so farther camera pans faster in world units.
            float panSpeed = m_distance * 0.002f;
            AddTarget(-right.x * dx * panSpeed - up.x * dy * panSpeed,
                      -right.y * dx * panSpeed - up.y * dy * panSpeed,
                      -right.z * dx * panSpeed - up.z * dy * panSpeed);
        } else {
            const float sens = 0.005f;
            AddYawPitch(dx * sens, -dy * sens);
        }
    }

    void Camera::OnMouseWheel(int delta)
    {
        const Vector3 forward = ComputeForward(m_yaw, m_pitch);
        const float zoomStep = 0.2f;
        float dir = (delta > 0) ? 1.0f : -1.0f;
        AddTarget(forward.x * zoomStep * dir,
                  forward.y * zoomStep * dir,
                  forward.z * zoomStep * dir);
    }

    void Camera::Update(float deltaTime)
    {
        if (!m_moveForward && !m_moveBackward && !m_moveLeft && !m_moveRight) return;

        const Vector3 forward = ComputeForward(m_yaw, m_pitch);
        const Vector3 upWorld(0.0f, 1.0f, 0.0f);
        Vector3 right = Cross(upWorld, forward);
        Normalize(right);

        const float speed = m_moveSpeed;
        float forwardDir = (m_moveForward ? 1.0f : 0.0f) + (m_moveBackward ? -1.0f : 0.0f);
        float rightDir = (m_moveRight ? 1.0f : 0.0f) + (m_moveLeft ? -1.0f : 0.0f);
        if (forwardDir == 0.0f && rightDir == 0.0f) return;

        Vector3 move(
            forward.x * forwardDir + right.x * rightDir,
            forward.y * forwardDir + right.y * rightDir,
            forward.z * forwardDir + right.z * rightDir);
        // Normalize so diagonal movement speed equals axis-aligned movement speed.
        Normalize(move);
        AddTarget(move.x * speed * deltaTime,
                  move.y * speed * deltaTime,
                  move.z * speed * deltaTime);
    }
}
