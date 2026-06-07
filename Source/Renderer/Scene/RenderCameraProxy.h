#pragma once

namespace SasamiRenderer
{
    enum class RenderCameraMode
    {
        Pbr = 0,
        RayMarch = 1,
    };

    struct RenderCameraProxy
    {
        float viewProjection[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
        float projection[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
        float cameraPosition[3] = { 0.0f, 0.0f, 0.0f };
        float nearClip = 0.0005f;
        float farClip = 500.0f;
        RenderCameraMode cameraMode = RenderCameraMode::Pbr;
        float cameraRight[3] = { 1.0f, 0.0f, 0.0f };
        float cameraUp[3] = { 0.0f, 1.0f, 0.0f };
        float cameraForward[3] = { 0.0f, 0.0f, 1.0f };
        float tanHalfFovY = 0.577350269f;
        float aspectRatio = 1.0f;
    };
}
