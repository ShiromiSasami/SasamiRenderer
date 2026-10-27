#pragma once
#include "Renderer/RHI/RhiDevice.h"
#include "Renderer/Scene/MeshBuffer.h"
#include "Renderer/Scene/SkinnedMeshBuffer.h"
#include "Renderer/Structures/Texture.h"

namespace SasamiEngine::Render
{
    // Records draw calls for all mesh items in MeshBuffer
    class DrawCommandBuilder
    {
    public:
        void RecordAll(IRhiCommandEncoder* enc, MeshBuffer& buffer) const;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
