#pragma once
#include "GraphicsDevice.h"
#include "MeshBuffer.h"
#include "Texture.h"

namespace SasamiRenderer
{
    // Records draw calls for all mesh items in MeshBuffer
    class DrawCommandBuilder
    {
    public:
        void RecordAll(CommandList* cmdList, MeshBuffer& buffer) const;

        // Draw a single mesh buffer item with a bound texture SRV (t0 by default).
        // Note: Caller must have set the descriptor heap that contains tex.srv.
        void RecordTextured(CommandList* cmdList,
                            MeshBuffer& buffer,
                            size_t itemIndex,
                            const Texture& tex,
                            UINT rootParamIndex = 0) const;

        // Draw all mesh buffer items with the same texture SRV (t0 by default).
        // Note: Caller must have set the descriptor heap that contains tex.srv.
        void RecordAllTextured(CommandList* cmdList,
                               MeshBuffer& buffer,
                               const Texture& tex,
                               UINT rootParamIndex = 0) const;
    };
}
