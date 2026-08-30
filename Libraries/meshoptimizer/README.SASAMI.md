# meshoptimizer (vendored subset)

Upstream: https://github.com/zeux/meshoptimizer — MIT License (see LICENSE.md).
Vendored version: 1.2 (`MESHOPTIMIZER_VERSION 1020`).

Only the translation units needed for meshlet generation and the standard
pre-meshletization optimization pipeline are vendored, not all of `src/`:

| File | Why it is here |
| --- | --- |
| `meshoptimizer.h` | Public API header, required by every TU below. |
| `clusterizer.cpp` | `meshopt_buildMeshlets`, `meshopt_computeMeshletBounds` — the actual meshlet builder and the bounding sphere + normal cone used for amplification-shader culling. |
| `allocator.cpp` | Allocation hooks the other TUs rely on. |
| `vcacheoptimizer.cpp` | `meshopt_optimizeVertexCache` — run before meshletization so meshlets get good vertex reuse. Also improves the classic vertex-shader path. |
| `vfetchoptimizer.cpp` | `meshopt_optimizeVertexFetch` — vertex buffer reorder after cache optimization. |
| `indexgenerator.cpp` | `meshopt_generateVertexRemap` and friends, used to deduplicate vertices before the above. |

Deliberately NOT vendored: the simplifier, remesher, codecs, tangent space,
partitioning, opacity maps, stripifier, spatial order, overdraw optimizer and
rasterizer. Add a file here only when something actually calls into it; the
subset exists so the build does not carry code the renderer never invokes.

Do not edit these files. To update, re-copy from upstream at the new tag and
re-check this table.
