#pragma once

#include "Renderer/Structures/Mesh.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Render::ParticleCharacter
{
    // One point on the surface of a skinned model, stored as a triangle of the concatenated vertex list
    // (vertices of every mesh in mesh order, the same order BakeVat uses) plus barycentric weights.
    // The point follows the mesh when the three vertices are animated: p = b0*p0 + b1*p1 + b2*p2.
    struct SurfaceSample
    {
        uint32_t vertex[3] = { 0, 0, 0 }; // indices into the concatenated vertex list
        float    b1 = 0.0f;               // weight of vertex[1]
        float    b2 = 0.0f;               // weight of vertex[2]; b0 = 1 - b1 - b2
        float    uv[2] = { 0.0f, 0.0f };  // interpolated vertex uv (for colour lookups)
        uint32_t mesh = 0;                // source mesh index (for material lookups)
    };

    struct SurfaceSampleSet
    {
        std::vector<SurfaceSample> samples;
        uint32_t vertexCount = 0;    // size of the concatenated vertex list the indices refer to
        double   totalArea = 0.0;    // bind-pose surface area, model units^2
        uint32_t triangleCount = 0;  // triangles with non-zero area that took part in the sampling
    };

    // Area-weighted uniform sampling of the bind-pose surface (the point-cache bake of Unity VFX Graph
    // and Niagara's skeletal mesh location sampling): a triangle is drawn from the area CDF, then a point
    // inside it with u = sqrt(r1), (b0, b1, b2) = (1 - u, u (1 - r2), u r2) (Osada et al. 2002, "Shape
    // Distributions", eq. 1). The random stream is PCG32 (O'Neill 2014) seeded with `seed`, so the result
    // is the same on every platform. Meshes without indices use 0..N-1. Fails on count == 0, no
    // vertices, or zero total area.
    bool SampleSkinnedSurface(const std::vector<SkinnedMesh>& meshes, uint32_t count, uint32_t seed,
                              SurfaceSampleSet& out, std::string& error);

    // Interpolates the samples over per-vertex data. `positions` / `normals` hold vertexCount entries of
    // `strideFloats` floats each (3 for packed xyz, 4 for the VAT float4 rows). Writes count * 3 floats to
    // outPositions and, when normals and outNormals are both non-null, count * 3 normalized floats to
    // outNormals (a zero normal stays zero).
    void InterpolateSamples(const SurfaceSampleSet& set, const float* positions, const float* normals,
                            uint32_t strideFloats, float* outPositions, float* outNormals);
}

namespace SasamiEngine { using namespace Render; }
