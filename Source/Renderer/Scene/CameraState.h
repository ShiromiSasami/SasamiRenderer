#pragma once
#include "Renderer/Scene/RenderCameraProxy.h"
#include "Foundation/Math/MathUtil.h"
#include <cstring>

namespace SasamiRenderer
{
    class CameraState
    {
    public:
        void Update(const RenderCameraProxy* camera)
        {
            if (camera) {
                for (int i = 0; i < 16; ++i) {
                    m_pv[i]   = camera->viewProjection[i];
                    m_proj[i] = camera->projection[i];
                }
                m_pos[0] = camera->cameraPosition[0];
                m_pos[1] = camera->cameraPosition[1];
                m_pos[2] = camera->cameraPosition[2];
                m_cameraMode = camera->cameraMode;
                m_right[0] = camera->cameraRight[0];
                m_right[1] = camera->cameraRight[1];
                m_right[2] = camera->cameraRight[2];
                m_up[0] = camera->cameraUp[0];
                m_up[1] = camera->cameraUp[1];
                m_up[2] = camera->cameraUp[2];
                m_forward[0] = camera->cameraForward[0];
                m_forward[1] = camera->cameraForward[1];
                m_forward[2] = camera->cameraForward[2];
                m_tanHalfFovY = camera->tanHalfFovY;
                m_aspectRatio = camera->aspectRatio;
                m_nearClip = camera->nearClip;
                m_farClip = camera->farClip;
                if (!Math::Invert4x4(camera->viewProjection, m_invPV)) {
                    for (int i = 0; i < 16; ++i) m_invPV[i] = (i % 5 == 0) ? 1.0f : 0.0f;
                }
            } else {
                for (int i = 0; i < 16; ++i) {
                    m_pv[i]    = (i % 5 == 0) ? 1.0f : 0.0f;
                    m_invPV[i] = (i % 5 == 0) ? 1.0f : 0.0f;
                    m_proj[i]  = (i % 5 == 0) ? 1.0f : 0.0f;
                }
                m_pos[0] = m_pos[1] = m_pos[2] = 0.0f;
                m_right[0] = 1.0f; m_right[1] = 0.0f; m_right[2] = 0.0f;
                m_up[0] = 0.0f; m_up[1] = 1.0f; m_up[2] = 0.0f;
                m_forward[0] = 0.0f; m_forward[1] = 0.0f; m_forward[2] = 1.0f;
                m_tanHalfFovY = 0.577350269f;
                m_aspectRatio = 1.0f;
                m_nearClip = 0.0005f;
                m_farClip = 500.0f;
                m_cameraMode = RenderCameraMode::Pbr;
            }
        }
        const float* GetPV()    const { return m_pv; }
        const float* GetProj()  const { return m_proj; }
        const float* GetPos()   const { return m_pos; }
        const float* GetRight() const { return m_right; }
        const float* GetUp()    const { return m_up; }
        const float* GetForward() const { return m_forward; }
        float GetTanHalfFovY() const { return m_tanHalfFovY; }
        float GetAspectRatio() const { return m_aspectRatio; }
        float GetNearClip() const { return m_nearClip; }
        float GetFarClip() const { return m_farClip; }
        RenderCameraMode GetCameraMode() const { return m_cameraMode; }
        const float* GetInvPV() const { return m_invPV; }
    private:
        float m_pv[16]    = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        float m_proj[16]  = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        float m_pos[3]    = {};
        float m_right[3]  = { 1.0f, 0.0f, 0.0f };
        float m_up[3]     = { 0.0f, 1.0f, 0.0f };
        float m_forward[3]= { 0.0f, 0.0f, 1.0f };
        float m_tanHalfFovY = 0.577350269f;
        float m_aspectRatio = 1.0f;
        float m_nearClip = 0.0005f;
        float m_farClip = 500.0f;
        RenderCameraMode m_cameraMode = RenderCameraMode::Pbr;
        float m_invPV[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    };
}
