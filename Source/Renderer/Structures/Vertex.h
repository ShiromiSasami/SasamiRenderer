#pragma once

namespace SasamiEngine::Render
{
    struct Vertex
    {
        float position[3];
        float normal[3];
        float color[4];
        float uv[2];
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
