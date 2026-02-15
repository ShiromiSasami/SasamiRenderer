#pragma once

namespace SasamiRenderer
{
    struct RenderDirectionalLight
    {
        float yaw = 0.7f;
        float pitch = 1.0f;
        float distance = 4.0f;
        float orthoHalf = 1.8f;
        float nearZ = 0.1f;
        float farZ = 10.0f;
        float color[3] = { 1.0f, 1.0f, 1.0f };
        float intensity = 1.0f;
    };

    struct RenderPointLight
    {
        float pos[3] = { 0.0f, 1.5f, 0.0f };
        float range = 5.0f;
        float color[3] = { 1.0f, 1.0f, 1.0f };
        float intensity = 1.0f;
    };

    struct RenderSpotLight
    {
        float pos[3] = { 0.0f, 2.0f, -2.0f };
        float range = 6.0f;
        float yaw = 0.0f;
        float pitch = -0.6f;
        float color[3] = { 1.0f, 1.0f, 1.0f };
        float intensity = 1.0f;
        float innerAngle = 0.261799f; // 15 deg
        float outerAngle = 0.436332f; // 25 deg
    };
}
