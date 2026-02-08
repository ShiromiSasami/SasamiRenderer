#include "Object/Camera.h"
#include <windows.h>
#include <cmath>
#include "Foundation/Math/MathUtil.h"

namespace SasamiRenderer
{
    using Math::Deg;
    using Math::Mul4x4;

    namespace {
        static void Cross(const float a[3], const float b[3], float out[3])
        {
            out[0] = a[1] * b[2] - a[2] * b[1];
            out[1] = a[2] * b[0] - a[0] * b[2];
            out[2] = a[0] * b[1] - a[1] * b[0];
        }

        static void Normalize(float v[3])
        {
            float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len > 0.0f) {
                v[0] /= len; v[1] /= len; v[2] /= len;
            }
        }

        static void ComputeForward(float yaw, float pitch, float out[3])
        {
            float cy = std::cos(yaw), sy = std::sin(yaw);
            float cp = std::cos(pitch), sp = std::sin(pitch);
            out[0] = sy * cp;
            out[1] = -sp;
            out[2] = cy * cp;
        }
    }

    std::array<float, 16> Camera::ComputeMVP(float viewportWidth, float viewportHeight) const {
        const float yaw = -m_yaw;
        const float pitch = -m_pitch;
        float cy = std::cos(yaw), sy = std::sin(yaw);
        float cp = std::cos(pitch), sp = std::sin(pitch);
        // View: rotate opposite to camera yaw/pitch
        float worldRot[16] = {
            cy,      sp * sy,   -cp * sy,  0,
            0,       cp,         sp,       0,
            sy,     -sp * cy,    cp * cy,  0,
            0,       0,          0,        1,
        };

        float worldTrans[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            -m_transform.position[0], -m_transform.position[1], -m_transform.position[2], 1,
        };

        // Projection: perspective (row-major, LH)
        float aspect = viewportWidth / viewportHeight;
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

        float view[16]; Mul4x4(worldTrans, worldRot, view);
        float mvp[16]; Mul4x4(view, proj, mvp);
        std::array<float, 16> out{};
        for (int i = 0; i < 16; ++i) out[i] = mvp[i];
        return out;
    }

    RenderCameraProxy Camera::BuildRenderCameraProxy(float viewportWidth, float viewportHeight) const
    {
        RenderCameraProxy proxy{};
        const auto vp = ComputeMVP(viewportWidth, viewportHeight);
        for (int i = 0; i < 16; ++i) {
            proxy.viewProjection[i] = vp[i];
        }
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
        case 'W': m_moveForward = true; break;
        case 'S': m_moveBackward = true; break;
        case 'A': m_moveLeft = true; break;
        case 'D': m_moveRight = true; break;
        default: break;
        }
    }

    void Camera::OnKeyUp(WPARAM key)
    {
        switch (key) {
        case 'W': m_moveForward = false; break;
        case 'S': m_moveBackward = false; break;
        case 'A': m_moveLeft = false; break;
        case 'D': m_moveRight = false; break;
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
            float forward[3];
            ComputeForward(m_yaw, m_pitch, forward);
            float upWorld[3] = { 0.0f, 1.0f, 0.0f };
            float right[3]; Cross(upWorld, forward, right); Normalize(right);
            float up[3]; Cross(right, forward, up); Normalize(up);
            float panSpeed = m_distance * 0.002f;
            AddTarget(-right[0] * dx * panSpeed - up[0] * dy * panSpeed,
                      -right[1] * dx * panSpeed - up[1] * dy * panSpeed,
                      -right[2] * dx * panSpeed - up[2] * dy * panSpeed);
        } else {
            const float sens = 0.005f;
            AddYawPitch(dx * sens, -dy * sens);
        }
    }

    void Camera::OnMouseWheel(int delta)
    {
        float forward[3];
        ComputeForward(m_yaw, m_pitch, forward);
        const float zoomStep = 0.2f;
        float dir = (delta > 0) ? 1.0f : -1.0f;
        AddTarget(forward[0] * zoomStep * dir,
                  forward[1] * zoomStep * dir,
                  forward[2] * zoomStep * dir);
    }

    void Camera::Update(float deltaTime)
    {
        if (!m_moveForward && !m_moveBackward && !m_moveLeft && !m_moveRight) return;

        float forward[3];
        ComputeForward(m_yaw, m_pitch, forward);
        float upWorld[3] = { 0.0f, 1.0f, 0.0f };
        float right[3];
        Cross(upWorld, forward, right);
        Normalize(right);

        const float speed = m_moveSpeed;
        float forwardDir = (m_moveForward ? 1.0f : 0.0f) + (m_moveBackward ? -1.0f : 0.0f);
        float rightDir = (m_moveRight ? 1.0f : 0.0f) + (m_moveLeft ? -1.0f : 0.0f);
        if (forwardDir == 0.0f && rightDir == 0.0f) return;

        float move[3] = {
            forward[0] * forwardDir + right[0] * rightDir,
            forward[1] * forwardDir + right[1] * rightDir,
            forward[2] * forwardDir + right[2] * rightDir
        };
        Normalize(move);
        AddTarget(move[0] * speed * deltaTime,
                  move[1] * speed * deltaTime,
                  move[2] * speed * deltaTime);
    }
}
