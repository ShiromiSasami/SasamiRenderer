#pragma once

// Reorders a loaded mesh for GPU cache locality. Both passes are pure reorderings: the same
// triangles are drawn from the same vertex data, only the order changes, so anything built from
// these buffers afterwards (BVH, BLAS, bounds) is unaffected.
//
// Motivation: the shadow pass measures 241 Mtri/s on an RTX 4070 Ti, which is more than an order
// of magnitude below what the part can do, and the usual suspects were eliminated by measurement
// (fill rate, CPU submission, per-draw overhead, bandwidth, and a depth-only VS that turned out to
// change nothing). Vertex cache locality is one of the two candidates left. See TODO.md.
//
// Opt-in via SASAMI_VERTEX_CACHE_OPT=1 rather than always-on, because the point right now is to
// A/B it: the two runs have to differ only in this, and the shadow pass timing has to be compared
// against the GBuffer pass in the same run to rule out external GPU load.

#include <cstring>
#include <vector>

// NOMINMAX is required here specifically: this header is included first in ModelLoader.cpp, so
// without it windows.h's min/max macros reach the std::max calls further down that file.
// WIN32_LEAN_AND_MEAN just keeps the pull-in small -- only GetEnvironmentVariableA is needed.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Renderer/Structures/Mesh.h"
#include "meshoptimizer/meshoptimizer.h"

namespace SasamiEngine::App
{
    inline bool IsVertexCacheOptimizationEnabled()
    {
        // Read once: this is consulted per mesh and the environment cannot change mid-run.
        // GetEnvironmentVariableA rather than std::getenv, matching SASAMI_GPU_VALIDATION and
        // SASAMI_DEBUG_REMOTE (getenv is rejected as C4996 under this project's warning level).
        static const bool enabled = [] {
            char value[8] = {};
            const DWORD length = GetEnvironmentVariableA("SASAMI_VERTEX_CACHE_OPT", value, sizeof(value));
            return length > 0 && length < sizeof(value) && value[0] == '1';
        }();
        return enabled;
    }

    // Reorders indices for the post-transform vertex cache, then reorders vertices so they are
    // fetched roughly in the order the index buffer asks for them.
    inline void OptimizeMeshForVertexCache(Mesh& mesh)
    {
        const size_t indexCount = mesh.indices.size();
        const size_t vertexCount = mesh.vertices.size();
        // meshopt requires a triangle list; a mesh with no indices is drawn non-indexed and has
        // nothing to reorder.
        if (indexCount < 3 || indexCount % 3 != 0 || vertexCount == 0) {
            return;
        }

        std::vector<uint32_t> reordered(indexCount);
        meshopt_optimizeVertexCache(reordered.data(), mesh.indices.data(), indexCount, vertexCount);
        mesh.indices = std::move(reordered);

        // Rewrites mesh.indices in place to match the new vertex order. Writing into a separate
        // buffer rather than in place keeps this independent of whether meshopt allows aliasing.
        std::vector<Vertex> fetched(vertexCount);
        const size_t uniqueVertices = meshopt_optimizeVertexFetch(fetched.data(),
                                                                  mesh.indices.data(),
                                                                  indexCount,
                                                                  mesh.vertices.data(),
                                                                  vertexCount,
                                                                  sizeof(Vertex));
        // Unused vertices are dropped by the fetch optimizer, so the buffer can legitimately
        // shrink; the indices it rewrote already point at the compacted range.
        fetched.resize(uniqueVertices);
        mesh.vertices = std::move(fetched);
    }
}
namespace SasamiEngine { using namespace App; }
