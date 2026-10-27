# meshoptimizer (vendored subset)

Upstream: https://github.com/zeux/meshoptimizer — MIT License (see LICENSE.md).
Vendored version: 1.2 (`MESHOPTIMIZER_VERSION 1020`), files copied from upstream
`master` at the commit matching this header (2026-09-04; `meshoptimizer.h` and
`clusterizer.cpp` were byte-identical to the already vendored copies).

Only the translation units needed for meshlet generation, the standard
pre-meshletization optimization pipeline and the cluster-LOD (virtual geometry)
DAG build are vendored, not all of `src/`:

| File | Why it is here |
| --- | --- |
| `meshoptimizer.h` | Public API header, required by every TU below. |
| `clusterizer.cpp` | `meshopt_buildMeshlets`, `meshopt_computeMeshletBounds` — the actual meshlet builder and the bounding sphere + normal cone used for amplification-shader culling. |
| `allocator.cpp` | Allocation hooks the other TUs rely on. |
| `vcacheoptimizer.cpp` | `meshopt_optimizeVertexCache` — run before meshletization so meshlets get good vertex reuse. Also improves the classic vertex-shader path. |
| `vfetchoptimizer.cpp` | `meshopt_optimizeVertexFetch` — vertex buffer reorder after cache optimization. |
| `indexgenerator.cpp` | `meshopt_generateVertexRemap` and friends, used to deduplicate vertices before the above. |
| `simplifier.cpp` | `meshopt_simplifyWithAttributes`, `meshopt_simplifySloppy`, `meshopt_simplifyScale` — group simplification for the cluster-LOD DAG (`ClusterDagBuilder`). |
| `meshletutils.cpp` | `meshopt_computeClusterBounds`, `meshopt_computeSphereBounds`, `meshopt_optimizeMeshletLevel`, `meshopt_extractMeshletIndices` — cluster bounds/ordering helpers `clusterlod.h` calls. |
| `partition.cpp` | `meshopt_partitionClusters` — groups adjacent clusters before each simplification level. |
| `spatialorder.cpp` | `meshopt_spatialSortRemap`, `meshopt_spatialClusterPoints` — spatial ordering used by `clusterlod.h`. |
| `vertexcodec.cpp` | `meshopt_encodeVertexBuffer`/`decodeVertexBuffer` — PerfLabApp mesh-compression case. |
| `indexcodec.cpp` | `meshopt_encodeIndexBuffer`/`decodeIndexBuffer` — PerfLabApp mesh-compression case. |
| `quantization.cpp` | `meshopt_quantizeHalf`/`quantizeFloat` — PerfLabApp quantization case. |
| `indexanalyzer.cpp` | `meshopt_analyzeVertexCache`, `meshopt_analyzeVertexFetch` — PerfLabApp ACMR/ATVR case. |
| `overdrawoptimizer.cpp` | `meshopt_optimizeOverdraw` — PerfLabApp ACMR/overdraw case. |
| `rasterizer.cpp` | `meshopt_analyzeOverdraw`, `meshopt_analyzeCoverage` — PerfLabApp overdraw measurement. |
| `clusterlod.h` | Upstream `demo/clusterlod.h`: the Nanite-style cluster → group → simplify → re-cluster DAG builder. `CLUSTERLOD_IMPLEMENTATION` is defined only in `Source/Renderer/VirtualGeometry/ClusterDagBuilder.cpp`. |

The six PerfLabApp files (codecs, quantization, analyzers, overdraw optimizer, rasterizer) were added on
2026-10-25 from upstream commit `8f0c87b` (2026-09-01), the newest commit whose `meshoptimizer.h` and
other vendored `.cpp` files are byte-identical to the copies here (checked with `git hash-object`).

Deliberately NOT vendored: the remesher, tangent space, opacity maps and stripifier. Add a file here only when something actually calls into it; the
subset exists so the build does not carry code the renderer never invokes.

Do not edit these files. To update, re-copy from upstream at the new tag and
re-check this table.
