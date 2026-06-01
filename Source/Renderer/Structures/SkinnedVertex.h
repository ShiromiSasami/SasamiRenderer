#pragma once
#include <cstdint>

namespace SasamiRenderer
{
    // 68-byte skinned vertex: Vertex base (48) + bone indices (4) + bone weights (16)
    struct SkinnedVertex
    {
        float   position[3];     // offset  0, 12 bytes
        float   normal[3];       // offset 12, 12 bytes
        float   color[4];        // offset 24, 16 bytes
        float   uv[2];           // offset 40,  8 bytes
        uint8_t boneIndices[4];  // offset 48,  4 bytes — DXGI_FORMAT_R8G8B8A8_UINT, up to 255 bones
        float   boneWeights[4];  // offset 52, 16 bytes — must sum to 1.0
    };                           // total: 68 bytes
}
