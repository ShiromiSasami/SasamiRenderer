#pragma once
#include "Renderer/RHI/RhiDevice.h"
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
    };
}
