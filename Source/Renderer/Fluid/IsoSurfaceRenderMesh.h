#pragma once

#include "Renderer/Fluid/IsoSurfaceMesh.h"
#include "Renderer/Structures/Mesh.h"

namespace SasamiEngine::Render::Fluid
{
    // Copies an iso-surface into a renderer Mesh (48 B Vertex): positions, normals, indices as they
    // are, every vertex coloured `rgba`, uv 0. The extractor's winding ((b-a)x(c-a) points out of the
    // surface) is the one the engine's other meshes use for front faces (OceanSurfaceMesh, AddBox), so
    // the indices are not flipped.
    Mesh BuildIsoSurfaceRenderMesh(const IsoSurfaceMesh& iso, const float rgba[4]);
}


namespace SasamiEngine { using namespace Render; }
