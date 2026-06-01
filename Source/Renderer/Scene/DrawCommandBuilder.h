#pragma once
#include "Renderer/Core/RhiDevice.h"
#include "Renderer/Scene/MeshBuffer.h"
#include "Renderer/Scene/SkinnedMeshBuffer.h"
#include "Renderer/Structures/Texture.h"

namespace SasamiRenderer
{
    // Records draw calls for all mesh items in MeshBuffer
    class DrawCommandBuilder
    {
    public:
        void RecordAll(IRhiCommandEncoder* enc, MeshBuffer& buffer) const;

        // Draw a single mesh buffer item with a bound texture SRV (t0 by default).
        // Note: Caller must have set the descriptor heap that contains tex.srv.
        void RecordTextured(IRhiCommandEncoder* enc,
                            MeshBuffer& buffer,
                            size_t itemIndex,
                            const Texture& tex,
                            UINT rootParamIndex = 0) const;

        // Draw all mesh buffer items with the same texture SRV (t0 by default).
        // Note: Caller must have set the descriptor heap that contains tex.srv.
        void RecordAllTextured(IRhiCommandEncoder* enc,
                               MeshBuffer& buffer,
                               const Texture& tex,
                               UINT rootParamIndex = 0) const;

        // Draw a single skinned mesh item with bone matrix CB bound at root param [14].
        // Caller must bind the skinned root signature and PSO before calling.
        void RecordSkinnedTextured(IRhiCommandEncoder* enc,
                                   SkinnedMeshBuffer& buffer,
                                   size_t itemIndex,
                                   const Texture& tex,
                                   RhiGpuAddress boneMatricesCbGpu,
                                   UINT texRootParamIndex    = 0,
                                   UINT boneCbRootParamIndex = 14) const;
    };
}
