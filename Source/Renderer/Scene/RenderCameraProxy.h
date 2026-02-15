#pragma once

namespace SasamiRenderer
{
    struct RenderCameraProxy
    {
        float viewProjection[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
        float cameraPosition[3] = { 0.0f, 0.0f, 0.0f };
    };
}
