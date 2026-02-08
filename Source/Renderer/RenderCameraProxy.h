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
    };
}
