#pragma once

namespace SasamiRenderer
{
    struct SurfaceMaterial
    {
        float baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float emissive[3] = { 0.0f, 0.0f, 0.0f };
        float roughness = 0.5f;
        float metallic = 0.0f;
        float occlusionStrength = 1.0f;
        float transmission = 0.0f;
        float ior = 1.5f;
    };
}
