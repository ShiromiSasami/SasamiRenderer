// ModelLoaderFbx.cpp
// FBX scene loading (Amazon Lumberyard Bistro et al.) via the vendored ufbx library.
// Produces the same LoadedStaticMesh list as the glTF/OBJ paths.
#include "ModelLoaderFbx.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Foundation/Math/MathUtil.h"
#include "Foundation/Tools/DebugOutput.h"

#include "ufbx/ufbx.h"

namespace SasamiRenderer
{
    using Math::Mul4x4;
    using Math::Clamp01;
    using Math::DefaultReflectionStrength;

    namespace {
        // Frees the ufbx_scene on scope exit regardless of the function's exit path.
        struct UfbxSceneGuard {
            ufbx_scene* scene = nullptr;
            ~UfbxSceneGuard() { if (scene) { ufbx_free_scene(scene); } }
        };

        // Exact-value vertex identity for index dedup. Vertex is a POD of floats built
        // from the same source values, so byte equality is the right test: corners that
        // came from identical attribute data compare equal without epsilon tuning.
        struct VertexKey {
            Vertex v;
            bool operator==(const VertexKey& other) const
            {
                return std::memcmp(&v, &other.v, sizeof(Vertex)) == 0;
            }
        };

        struct VertexKeyHash {
            static_assert(sizeof(Vertex) % sizeof(uint64_t) == 0,
                          "VertexKeyHash reads Vertex in whole uint64_t words");
            size_t operator()(const VertexKey& key) const
            {
                // FNV-1a over the vertex bytes, consumed a 64-bit word at a time
                // (Vertex is 48 bytes = 6 uint64_t words) instead of byte-by-byte.
                const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&key.v);
                size_t hash = 1469598103934665603ull;
                for (size_t i = 0; i < sizeof(Vertex); i += sizeof(uint64_t)) {
                    uint64_t word;
                    std::memcpy(&word, bytes + i, sizeof(uint64_t));
                    hash ^= word;
                    hash *= 1099511628211ull;
                }
                return hash;
            }
        };

        std::string ToStdString(const ufbx_string& s)
        {
            if (!s.data || s.length == 0) {
                return std::string();
            }
            return std::string(s.data, s.length);
        }

        std::string ToLower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        // Converts a 3x4 column-major, column-vector ufbx_matrix into the engine's
        // row-major, row-vector 4x4 layout (see ModelLoaderFbx task brief for derivation).
        void ConvertMatrix(const ufbx_matrix& m, float out[16])
        {
            out[0] = static_cast<float>(m.m00); out[1] = static_cast<float>(m.m10); out[2]  = static_cast<float>(m.m20); out[3]  = 0.0f;
            out[4] = static_cast<float>(m.m01); out[5] = static_cast<float>(m.m11); out[6]  = static_cast<float>(m.m21); out[7]  = 0.0f;
            out[8] = static_cast<float>(m.m02); out[9] = static_cast<float>(m.m12); out[10] = static_cast<float>(m.m22); out[11] = 0.0f;
            out[12] = static_cast<float>(m.m03); out[13] = static_cast<float>(m.m13); out[14] = static_cast<float>(m.m23); out[15] = 1.0f;
        }

        // Resolves a texture's on-disk path relative to the FBX file's own directory.
        // Bistro's FBX references sometimes point at files that only exist under a
        // sibling "Textures" directory, so that is tried as a fallback.
        std::string ResolveTexturePath(const std::filesystem::path& fbxDir,
                                       const ufbx_texture* texture,
                                       std::unordered_set<std::string>& warnedMissingTextures)
        {
            if (!texture) {
                return std::string();
            }

            std::string rel = ToStdString(texture->relative_filename);
            if (rel.empty()) {
                rel = ToStdString(texture->filename);
            }
            if (rel.empty()) {
                return std::string();
            }

            const std::filesystem::path relPath(rel);
            const std::filesystem::path resolved = relPath.is_absolute() ? relPath : (fbxDir / relPath);

            std::error_code ec;
            if (std::filesystem::exists(resolved, ec)) {
                return resolved.string();
            }

            const std::filesystem::path fallback = fbxDir / "Textures" / relPath.filename();
            if (std::filesystem::exists(fallback, ec)) {
                return fallback.string();
            }

            const std::string resolvedStr = resolved.string();
            if (warnedMissingTextures.insert(resolvedStr).second) {
                DebugLog(("ModelLoaderFbx: missing texture \"" + resolvedStr + "\"\n").c_str());
            }
            return resolvedStr;
        }

        struct ResolvedMaterial {
            SurfaceMaterial material;
            std::string texturePath;
            std::string normalTexturePath;
            std::string metallicRoughnessTexturePath;
        };

        // Fills a SurfaceMaterial + texture paths from a ufbx_material, mirroring the
        // field names/defaults the glTF loader uses in ModelLoader.cpp so materials
        // look consistent across formats.
        ResolvedMaterial ResolveMaterial(const ufbx_material* mat,
                                         const std::filesystem::path& fbxDir,
                                         std::unordered_set<std::string>& warnedMissingTextures,
                                         bool& warnedNormalMap)
        {
            ResolvedMaterial result;
            if (!mat) {
                return result;
            }

            const ufbx_material_map& baseColor = mat->pbr.base_color;
            if (baseColor.has_value) {
                result.material.baseColor[0] = static_cast<float>(baseColor.value_vec4.x);
                result.material.baseColor[1] = static_cast<float>(baseColor.value_vec4.y);
                result.material.baseColor[2] = static_cast<float>(baseColor.value_vec4.z);
                result.material.baseColor[3] = (baseColor.value_components >= 4)
                    ? static_cast<float>(baseColor.value_vec4.w)
                    : 1.0f;
            }

            if (mat->pbr.roughness.has_value) {
                result.material.roughness = Clamp01(static_cast<float>(mat->pbr.roughness.value_real));
            }
            if (mat->pbr.metalness.has_value) {
                result.material.metallic = Clamp01(static_cast<float>(mat->pbr.metalness.value_real));
            }
            result.material.reflectionStrength = DefaultReflectionStrength(result.material.roughness, result.material.metallic);
            result.material.doubleSided = mat->features.double_sided.enabled;

            const ufbx_texture* baseColorTexture = mat->pbr.base_color.texture;
            if (!baseColorTexture) {
                baseColorTexture = mat->fbx.diffuse_color.texture;
            }
            if (baseColorTexture) {
                result.texturePath = ResolveTexturePath(fbxDir, baseColorTexture, warnedMissingTextures);
            }

            // Lumberyard convention: "*_Specular" textures pack roughness/metallic.
            const ufbx_texture* specularCandidates[] = {
                mat->pbr.roughness.texture,
                mat->pbr.metalness.texture,
                mat->pbr.specular_color.texture,
                mat->fbx.specular_color.texture,
                mat->fbx.specular_factor.texture,
                mat->pbr.glossiness.texture,
            };
            for (const ufbx_texture* candidate : specularCandidates) {
                if (!candidate) {
                    continue;
                }
                std::string name = ToStdString(candidate->relative_filename);
                if (name.empty()) {
                    name = ToStdString(candidate->filename);
                }
                if (ToLower(name).find("specular") != std::string::npos) {
                    result.metallicRoughnessTexturePath = ResolveTexturePath(fbxDir, candidate, warnedMissingTextures);
                    break;
                }
            }

            const ufbx_texture* normalMapTexture = mat->pbr.normal_map.texture;
            if (!normalMapTexture) {
                normalMapTexture = mat->fbx.normal_map.texture;
            }
            if (normalMapTexture) {
                result.normalTexturePath = ResolveTexturePath(fbxDir, normalMapTexture, warnedMissingTextures);
            }

            if (!warnedNormalMap) {
                const bool hasUnresolvedBumpMap = !normalMapTexture && mat->fbx.bump.texture != nullptr;
                if (hasUnresolvedBumpMap) {
                    DebugLog("ModelLoaderFbx: bump map textures are present but not supported as tangent-space normal maps; skipping.\n");
                    warnedNormalMap = true;
                }
            }

            return result;
        }
    }

    bool LoadStaticModelFbx(const std::string& path,
                            float uniformScale,
                            std::vector<LoadedStaticMesh>& outMeshes)
    {
        outMeshes.clear();

        ufbx_load_opts opts = {};
        opts.target_axes = ufbx_axes_right_handed_y_up;
        opts.target_unit_meters = 1.0f;
        opts.generate_missing_normals = true;
        opts.load_external_files = false;
        opts.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;

        const auto loadStart = std::chrono::steady_clock::now();
        ufbx_error error;
        UfbxSceneGuard guard;
        guard.scene = ufbx_load_file(path.c_str(), &opts, &error);
        const auto parseEnd = std::chrono::steady_clock::now();
        if (!guard.scene) {
            DebugLog(("ModelLoaderFbx: failed to load \"" + path + "\": " + ToStdString(error.description) + "\n").c_str());
            return false;
        }
        const ufbx_scene* scene = guard.scene;

        const std::filesystem::path fbxDir = std::filesystem::path(path).parent_path();

        const float scale = uniformScale;
        // Uniform scaling matrix applied to the imported node transform (mirrors ModelLoader.cpp's glTF path).
        const float scaleM[16] = {
            scale, 0, 0, 0,
            0, scale, 0, 0,
            0, 0, scale, 0,
            0, 0, 0, 1
        };

        std::unordered_set<std::string> warnedMissingTextures;
        bool warnedNormalMap = false;
        std::unordered_map<const ufbx_material*, ResolvedMaterial> materialCache;

        std::vector<uint32_t> triScratch;

        for (size_t mi = 0; mi < scene->meshes.count; ++mi) {
            const ufbx_mesh* mesh = scene->meshes.data[mi];
            if (!mesh || mesh->instances.count == 0) {
                continue;
            }

            triScratch.resize(mesh->max_face_triangles * 3);

            for (size_t pi = 0; pi < mesh->material_parts.count; ++pi) {
                const ufbx_mesh_part& part = mesh->material_parts.data[pi];
                if (part.face_indices.count == 0) {
                    continue;
                }

                Mesh partMesh;
                partMesh.vertices.clear();
                partMesh.indices.clear();
                partMesh.indices.reserve(part.num_triangles * 3);
                // Only the index buffer is known up front; the vertex count after dedup is
                // typically a fraction of the corner count, so let the vector grow.

                // Vertex dedup keyed on the assembled vertex VALUE, not on ufbx attribute
                // indices: FBX stores normals/UVs per polygon-corner, so corners that share
                // a position still carry distinct attribute indices and an index-based key
                // never matches. Comparing the built Vertex bytes is what actually collapses
                // them -- without it Bistro expands to one vertex per triangle corner and
                // runs the process out of memory before it can render.
                std::unordered_map<VertexKey, uint32_t, VertexKeyHash> vertexLookup;
                vertexLookup.reserve(part.num_triangles * 2);
                for (size_t fi = 0; fi < part.face_indices.count; ++fi) {
                    const uint32_t faceIndex = part.face_indices.data[fi];
                    if (faceIndex >= mesh->faces.count) {
                        continue;
                    }
                    const ufbx_face face = mesh->faces.data[faceIndex];
                    if (face.num_indices < 3) {
                        continue;
                    }

                    const uint32_t numTris = ufbx_triangulate_face(triScratch.data(), triScratch.size(), mesh, face);
                    for (uint32_t t = 0; t < numTris; ++t) {
                        for (int c = 0; c < 3; ++c) {
                            const uint32_t ix = triScratch[static_cast<size_t>(t) * 3 + c];

                            Vertex v{};
                            if (mesh->vertex_position.exists && ix < mesh->vertex_position.indices.count) {
                                const ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, ix);
                                v.position[0] = static_cast<float>(p.x);
                                v.position[1] = static_cast<float>(p.y);
                                v.position[2] = static_cast<float>(p.z);
                            }
                            if (mesh->vertex_normal.exists && ix < mesh->vertex_normal.indices.count) {
                                const ufbx_vec3 n = ufbx_get_vertex_vec3(&mesh->vertex_normal, ix);
                                v.normal[0] = static_cast<float>(n.x);
                                v.normal[1] = static_cast<float>(n.y);
                                v.normal[2] = static_cast<float>(n.z);
                            } else {
                                v.normal[0] = 0.0f; v.normal[1] = 1.0f; v.normal[2] = 0.0f;
                            }
                            if (mesh->vertex_uv.exists && ix < mesh->vertex_uv.indices.count) {
                                const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, ix);
                                // FBX UVs use a bottom-left origin (V=0 at bottom); the engine's glTF
                                // path passes UVs through unflipped (top-left origin), so flip here to
                                // match that convention and avoid upside-down textures.
                                v.uv[0] = static_cast<float>(uv.x);
                                v.uv[1] = 1.0f - static_cast<float>(uv.y);
                            } else {
                                v.uv[0] = 0.0f; v.uv[1] = 0.0f;
                            }
                            v.color[0] = 1.0f; v.color[1] = 1.0f; v.color[2] = 1.0f; v.color[3] = 1.0f;

                            const VertexKey key{ v };
                            if (const auto it = vertexLookup.find(key); it != vertexLookup.end()) {
                                partMesh.indices.push_back(it->second);
                                continue;
                            }

                            const uint32_t newIndex = static_cast<uint32_t>(partMesh.vertices.size());
                            partMesh.vertices.push_back(v);
                            partMesh.indices.push_back(newIndex);
                            vertexLookup.emplace(key, newIndex);
                        }
                    }
                }

                if (partMesh.vertices.empty()) {
                    continue;
                }

                for (size_t ni = 0; ni < mesh->instances.count; ++ni) {
                    const ufbx_node* node = mesh->instances.data[ni];
                    if (!node) {
                        continue;
                    }

                    const ufbx_material* mat = nullptr;
                    if (part.index < node->materials.count) {
                        mat = node->materials.data[part.index];
                    } else if (part.index < mesh->materials.count) {
                        mat = mesh->materials.data[part.index];
                    }

                    auto cacheIt = materialCache.find(mat);
                    if (cacheIt == materialCache.end()) {
                        cacheIt = materialCache.emplace(mat, ResolveMaterial(mat, fbxDir, warnedMissingTextures, warnedNormalMap)).first;
                    }
                    const ResolvedMaterial& resolved = cacheIt->second;

                    LoadedStaticMesh out;
                    if (ni + 1 == mesh->instances.count) {
                        // Last instance owns partMesh's buffers outright; move instead of
                        // deep-copying the vertex/index data one more time.
                        out.mesh = std::move(partMesh);
                    } else {
                        out.mesh = partMesh;
                    }
                    out.material = resolved.material;
                    out.texturePath = resolved.texturePath;
                    out.normalTexturePath = resolved.normalTexturePath;
                    out.metallicRoughnessTexturePath = resolved.metallicRoughnessTexturePath;

                    float nodeMatrix[16];
                    ConvertMatrix(node->geometry_to_world, nodeMatrix);
                    // Final local transform = nodeTransform * uniformScale, matching the operand
                    // order used by the glTF path in ModelLoader.cpp.
                    Mul4x4(nodeMatrix, scaleM, out.localTransform);

                    outMeshes.push_back(std::move(out));
                }
            }
        }

        size_t totalVertices = 0;
        size_t totalTriangles = 0;
        for (const LoadedStaticMesh& m : outMeshes) {
            totalVertices += m.mesh.vertices.size();
            totalTriangles += m.mesh.indices.size() / 3;
        }
        const auto convertEnd = std::chrono::steady_clock::now();
        const double parseMs = std::chrono::duration<double, std::milli>(parseEnd - loadStart).count();
        const double convertMs = std::chrono::duration<double, std::milli>(convertEnd - parseEnd).count();
        char timing[192];
        snprintf(timing, sizeof(timing),
                 "[Perf] ModelLoaderFbx: ufbx parse %.0f ms, mesh conversion %.0f ms\n", parseMs, convertMs);
        DebugLog(timing);

        char stats[256];
        snprintf(stats, sizeof(stats),
                 "ModelLoaderFbx: %s -> %zu submeshes, %zu vertices, %zu triangles (~%zu MB CPU)\n",
                 std::filesystem::path(path).filename().string().c_str(),
                 outMeshes.size(), totalVertices, totalTriangles,
                 (totalVertices * sizeof(Vertex) + totalTriangles * 3 * sizeof(uint32_t)) / (1024 * 1024));
        DebugLog(stats);

        return !outMeshes.empty();
    }
}
