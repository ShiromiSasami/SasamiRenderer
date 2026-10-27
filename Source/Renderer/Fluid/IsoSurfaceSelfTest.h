#pragma once

#include <string>

namespace SasamiEngine::Render::Fluid
{
    // CPU checks of ParticleSplatGrid + ExtractIsoSurface: a single splat's level set is the analytic
    // sphere |x| = radius * sqrt(1 - iso^(1/3)) (volume and area within a few %), the mesh is closed
    // (CountOpenEdges 0) with outward normals and positive volume, 0 particles give an empty mesh,
    // two overlapping splats merge into one closed surface, SplatAll is bit-identical to serial Splat calls,
    // and the attribute mean is exact for equal attributes. Returns "OK checks=N ..." or "ERR ...".
    std::string RunIsoSurfaceSelfTest();
}

namespace SasamiEngine { using namespace Render; }
