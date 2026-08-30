#include "ModelLoader.h"
#include "ModelLoaderFbx.h"
#include "StaticMeshCache.h"
#include "ModelLoaderGlb.h"
#include "ModelLoaderGltfUtility.h"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <array>
#include <unordered_map>
#include "Foundation/Math/MathUtil.h"
#include <rapidjson/document.h>

namespace SasamiRenderer
{
    // LoadOBJ → ModelLoaderOBJ.cpp

    using Math::Mul4x4;
    using Math::Clamp01;
    using Math::DefaultReflectionStrength;
    using namespace ModelLoaderGltfUtility;

    namespace {
        using rapidjson::Value;

        struct Node {
            int mesh = -1;
            std::vector<int> children;
            float local[16] = {
                1,0,0,0,
                0,1,0,0,
                0,0,1,0,
                0,0,0,1,
            };
        };

        static void ExtractNodeTransform(const Value& node, float out[16])
        {
            Identity(out);
            if (node.HasMember("matrix")) {
                const auto& m = node["matrix"];
                if (m.IsArray() && m.Size() == 16) {
                    // glTF matrix is column-major; transpose to row-major
                    for (int r = 0; r < 4; ++r) {
                        for (int c = 0; c < 4; ++c) {
                            out[r * 4 + c] = m[c * 4 + r].GetFloat();
                        }
                    }
                }
                return;
            }

            float T[16], R[16], S[16];
            Identity(T); Identity(R); Identity(S);
            if (node.HasMember("translation")) {
                float t[3] = { 0,0,0 };
                if (ReadVec(node["translation"], t, 3)) {
                    BuildTranslation(t, T);
                }
            }
            if (node.HasMember("rotation")) {
                float q[4] = { 0,0,0,1 };
                if (ReadVec(node["rotation"], q, 4)) {
                    BuildRotationFromQuat(q, R);
                }
            }
            if (node.HasMember("scale")) {
                float s[3] = { 1,1,1 };
                if (ReadVec(node["scale"], s, 3)) {
                    BuildScale(s, S);
                }
            }
            // TRS composition in row-vector convention (matches glTF column-major T*R*S):
            // local = S * R * T
            float SR[16]; Mul4x4(S, R, SR);
            Mul4x4(SR, T, out);
        }

        static void GatherNodeTransforms(const std::vector<Node>& nodes, int nodeIndex, const float parent[16],
                                         std::vector<std::array<float, 16>>& worldMatrices)
        {
            if (nodeIndex < 0 || nodeIndex >= (int)nodes.size()) return;
            float world[16];
            // Hierarchical transform accumulation:
            // world(node) = local(node) * world(parent)
            Mul4x4(nodes[nodeIndex].local, parent, world);
            std::array<float, 16> wm{};
            for (int i = 0; i < 16; ++i) wm[i] = world[i];
            worldMatrices[nodeIndex] = wm;

            for (int child : nodes[nodeIndex].children) {
                GatherNodeTransforms(nodes, child, world, worldMatrices);
            }
        }
    }

    bool LoadGLTFStatic(const std::string& path, GltfScene& outScene)
    {
        outScene = {};
        std::filesystem::path basePath = std::filesystem::path(path).parent_path();

        std::vector<uint8_t> fileBytes;
        if (!ReadFileBytes(std::filesystem::path(path), fileBytes)) return false;

        std::string jsonText;
        std::vector<uint8_t> glbBinChunk;
        bool isGlb = fileBytes.size() >= 4 &&
                     fileBytes[0] == 'g' && fileBytes[1] == 'l' && fileBytes[2] == 'T' && fileBytes[3] == 'F';
        if (isGlb) {
            if (!ParseGlbContainer(fileBytes, jsonText, glbBinChunk)) return false;
        } else {
            jsonText.assign(reinterpret_cast<const char*>(fileBytes.data()), fileBytes.size());
        }

        rapidjson::Document doc;
        doc.Parse(jsonText.c_str());
        if (!doc.IsObject()) return false;

        std::vector<std::vector<uint8_t>> buffers;
        std::vector<BufferView> views;
        std::vector<Accessor> accessors;
        ParseBuffersViewsAccessors(doc, basePath, isGlb, glbBinChunk, buffers, views, accessors);

        // Images
        if (doc.HasMember("images") && doc["images"].IsArray()) {
            const auto& imgs = doc["images"];
            outScene.images.resize(imgs.Size());
            for (rapidjson::SizeType i = 0; i < imgs.Size(); ++i) {
                const auto& im = imgs[i];
                if (im.HasMember("uri")) {
                    std::string uri = im["uri"].GetString();
                    outScene.images[i].uri = (basePath / uri).string();
                }
            }
        }

        // Textures
        if (doc.HasMember("textures") && doc["textures"].IsArray()) {
            const auto& texs = doc["textures"];
            outScene.textures.resize(texs.Size());
            for (rapidjson::SizeType i = 0; i < texs.Size(); ++i) {
                const auto& t = texs[i];
                if (t.HasMember("source")) {
                    outScene.textures[i].imageIndex = t["source"].GetInt();
                }
            }
        }

        // Materials
        if (doc.HasMember("materials") && doc["materials"].IsArray()) {
            const auto& mats = doc["materials"];
            outScene.materials.resize(mats.Size());
            for (rapidjson::SizeType i = 0; i < mats.Size(); ++i) {
                const auto& m = mats[i];
                if (!m.IsObject()) continue;
                GltfMaterial mat;
                if (m.HasMember("pbrMetallicRoughness")) {
                    const auto& pbr = m["pbrMetallicRoughness"];
                    if (pbr.HasMember("baseColorTexture")) {
                        const auto& bct = pbr["baseColorTexture"];
                        if (bct.HasMember("index")) mat.baseColorTexture = bct["index"].GetInt();
                    }
                    if (pbr.HasMember("metallicRoughnessTexture")) {
                        const auto& mrt = pbr["metallicRoughnessTexture"];
                        if (mrt.HasMember("index")) mat.metallicRoughnessTexture = mrt["index"].GetInt();
                    }
                    if (pbr.HasMember("baseColorFactor")) {
                        ReadVec(pbr["baseColorFactor"], mat.baseColorFactor, 4);
                    }
                    if (pbr.HasMember("metallicFactor")) {
                        mat.metallicFactor = pbr["metallicFactor"].GetFloat();
                    }
                    if (pbr.HasMember("roughnessFactor")) {
                        mat.roughnessFactor = pbr["roughnessFactor"].GetFloat();
                    }
                }
                if (m.HasMember("emissiveFactor")) {
                    ReadVec(m["emissiveFactor"], mat.emissiveFactor, 3);
                }
                if (m.HasMember("emissiveTexture") && m["emissiveTexture"].IsObject()) {
                    const auto& emissive = m["emissiveTexture"];
                    if (emissive.HasMember("index")) {
                        mat.emissiveTexture = emissive["index"].GetInt();
                    }
                }
                if (m.HasMember("occlusionTexture") && m["occlusionTexture"].IsObject()) {
                    const auto& occ = m["occlusionTexture"];
                    if (occ.HasMember("index")) {
                        mat.occlusionTexture = occ["index"].GetInt();
                    }
                    if (occ.HasMember("strength")) {
                        mat.occlusionStrength = occ["strength"].GetFloat();
                    }
                }
                if (m.HasMember("normalTexture") && m["normalTexture"].IsObject()) {
                    const auto& norm = m["normalTexture"];
                    if (norm.HasMember("index")) {
                        mat.normalTexture = norm["index"].GetInt();
                    }
                }
                if (m.HasMember("alphaMode") && m["alphaMode"].IsString()) {
                    mat.alphaBlend = (std::string(m["alphaMode"].GetString()) == "BLEND");
                }
                if (m.HasMember("doubleSided") && m["doubleSided"].IsBool()) {
                    mat.doubleSided = m["doubleSided"].GetBool();
                }
                if (m.HasMember("extensions") && m["extensions"].IsObject()) {
                    const auto& extensions = m["extensions"];
                    if (extensions.HasMember("KHR_materials_transmission") &&
                        extensions["KHR_materials_transmission"].IsObject()) {
                        const auto& transmission = extensions["KHR_materials_transmission"];
                        if (transmission.HasMember("transmissionFactor")) {
                            mat.transmissionFactor = transmission["transmissionFactor"].GetFloat();
                        }
                    }
                    if (extensions.HasMember("KHR_materials_ior") &&
                        extensions["KHR_materials_ior"].IsObject()) {
                        const auto& ior = extensions["KHR_materials_ior"];
                        if (ior.HasMember("ior")) {
                            mat.ior = ior["ior"].GetFloat();
                        }
                    }
                    if (extensions.HasMember("KHR_materials_volume") &&
                        extensions["KHR_materials_volume"].IsObject()) {
                        const auto& volume = extensions["KHR_materials_volume"];
                        if (volume.HasMember("thicknessFactor")) {
                            mat.thicknessFactor = volume["thicknessFactor"].GetFloat();
                        }
                        if (volume.HasMember("attenuationColor")) {
                            ReadVec(volume["attenuationColor"], mat.attenuationColor, 3);
                        }
                        if (volume.HasMember("attenuationDistance")) {
                            mat.attenuationDistance = volume["attenuationDistance"].GetFloat();
                        }
                    }
                }
                outScene.materials[i] = mat;
            }
        }

        // Nodes
        std::vector<Node> nodes;
        if (doc.HasMember("nodes") && doc["nodes"].IsArray()) {
            const auto& ns = doc["nodes"];
            nodes.resize(ns.Size());
            for (rapidjson::SizeType i = 0; i < ns.Size(); ++i) {
                const auto& n = ns[i];
                Node node;
                node.mesh = n.HasMember("mesh") ? n["mesh"].GetInt() : -1;
                if (n.HasMember("children") && n["children"].IsArray()) {
                    for (auto& c : n["children"].GetArray()) node.children.push_back(c.GetInt());
                }
                if (n.HasMember("matrix") || n.HasMember("translation") || n.HasMember("rotation") || n.HasMember("scale")) {
                    ExtractNodeTransform(n, node.local);
                } else {
                    Identity(node.local);
                }
                nodes[i] = node;
            }
        }

        // Scenes -> root nodes
        std::vector<int> rootNodes;
        if (doc.HasMember("scenes") && doc["scenes"].IsArray()) {
            const auto& scenes = doc["scenes"];
            int sceneIndex = doc.HasMember("scene") ? doc["scene"].GetInt() : 0;
            if (sceneIndex >= 0 && sceneIndex < (int)scenes.Size()) {
                const auto& scene = scenes[sceneIndex];
                if (scene.HasMember("nodes") && scene["nodes"].IsArray()) {
                    for (auto& n : scene["nodes"].GetArray()) rootNodes.push_back(n.GetInt());
                }
            }
        }
        if (rootNodes.empty()) {
            for (size_t i = 0; i < nodes.size(); ++i) {
                rootNodes.push_back(static_cast<int>(i));
            }
        }

        // Compute world transforms
        std::vector<std::array<float, 16>> worldMats(nodes.size());
        float identity[16]; Identity(identity);
        for (int root : rootNodes) {
            GatherNodeTransforms(nodes, root, identity, worldMats);
        }

        // Meshes
        if (!doc.HasMember("meshes") || !doc["meshes"].IsArray()) return false;
        const auto& meshes = doc["meshes"];
        for (rapidjson::SizeType mi = 0; mi < meshes.Size(); ++mi) {
            const auto& m = meshes[mi];
            if (!m.HasMember("primitives") || !m["primitives"].IsArray()) continue;
            for (const auto& prim : m["primitives"].GetArray()) {
                if (!prim.HasMember("attributes")) continue;
                const auto& attrs = prim["attributes"];

                int posAcc = attrs.HasMember("POSITION") ? attrs["POSITION"].GetInt() : -1;
                int norAcc = attrs.HasMember("NORMAL") ? attrs["NORMAL"].GetInt() : -1;
                int uvAcc = attrs.HasMember("TEXCOORD_0") ? attrs["TEXCOORD_0"].GetInt() : -1;
                int idxAcc = prim.HasMember("indices") ? prim["indices"].GetInt() : -1;
                int matIdx = prim.HasMember("material") ? prim["material"].GetInt() : -1;

                if (posAcc < 0 || posAcc >= (int)accessors.size()) continue;
                const Accessor& posA = accessors[posAcc];
                if (posA.bufferView < 0 || posA.bufferView >= (int)views.size()) continue;
                const BufferView& posView = views[posA.bufferView];
                if (posView.buffer < 0 || posView.buffer >= (int)buffers.size()) continue;
                const std::vector<uint8_t>& posBuf = buffers[posView.buffer];

                std::vector<uint32_t> indices;
                if (idxAcc >= 0 && idxAcc < (int)accessors.size()) {
                    const Accessor& idxA = accessors[idxAcc];
                    if (idxA.bufferView >= 0 && idxA.bufferView < (int)views.size()) {
                        const BufferView& idxView = views[idxA.bufferView];
                        if (idxView.buffer >= 0 && idxView.buffer < (int)buffers.size()) {
                            LoadIndices(buffers[idxView.buffer], idxView, idxA, indices);
                        }
                    }
                }

                std::vector<Vertex> verts;
                size_t vertCount = posA.count;
                verts.resize(vertCount);
                for (size_t i = 0; i < vertCount; ++i) {
                    float p[3] = {0,0,0};
                    float n[3] = {0,1,0};
                    float uv[2] = {0,0};
                    LoadFloat3(posBuf, posView, posA, i, p);
                    if (norAcc >= 0 && norAcc < (int)accessors.size()) {
                        const Accessor& norA = accessors[norAcc];
                        if (norA.bufferView >= 0 && norA.bufferView < (int)views.size()) {
                            const BufferView& norView = views[norA.bufferView];
                            if (norView.buffer >= 0 && norView.buffer < (int)buffers.size()) {
                                LoadFloat3(buffers[norView.buffer], norView, norA, i, n);
                            }
                        }
                    }
                    if (uvAcc >= 0 && uvAcc < (int)accessors.size()) {
                        const Accessor& uvA = accessors[uvAcc];
                        if (uvA.bufferView >= 0 && uvA.bufferView < (int)views.size()) {
                            const BufferView& uvView = views[uvA.bufferView];
                            if (uvView.buffer >= 0 && uvView.buffer < (int)buffers.size()) {
                                LoadFloat2(buffers[uvView.buffer], uvView, uvA, i, uv);
                            }
                        }
                    }
                    Vertex v{};
                    v.position[0] = p[0]; v.position[1] = p[1]; v.position[2] = p[2];
                    v.normal[0] = n[0]; v.normal[1] = n[1]; v.normal[2] = n[2];
                    v.color[0] = 1.0f; v.color[1] = 1.0f; v.color[2] = 1.0f; v.color[3] = 1.0f;
                    v.uv[0] = uv[0]; v.uv[1] = uv[1];
                    verts[i] = v;
                }

                Mesh mesh;
                mesh.vertices = std::move(verts);
                mesh.indices = std::move(indices);

                GltfPrimitiveInstance inst;
                inst.mesh = std::move(mesh);
                inst.materialIndex = matIdx;
                if (mi < nodes.size()) {
                    for (size_t ni = 0; ni < nodes.size(); ++ni) {
                        if (nodes[ni].mesh == (int)mi) {
                            for (int j = 0; j < 16; ++j) inst.transform[j] = worldMats[ni][j];
                            break;
                        }
                    }
                }

                outScene.primitives.push_back(std::move(inst));
            }
        }

        return !outScene.primitives.empty();
    }

    // Actual parsing for each supported format. LoadStaticModel below wraps this with
    // the on-disk cache; call that instead.
    static bool LoadStaticModelUncached(const std::string& path,
                                        StaticModelFormat format,
                                        float uniformScale,
                                        std::vector<LoadedStaticMesh>& outMeshes)
    {
        outMeshes.clear();

        if (format == StaticModelFormat::Obj) {
            std::vector<Vertex> vertices;
            if (!LoadOBJ(path, vertices)) {
                return false;
            }

            Mesh mesh;
            mesh.vertices = std::move(vertices);
            mesh.indices.resize(mesh.vertices.size());
            for (uint32_t i = 0; i < mesh.indices.size(); ++i) {
                mesh.indices[i] = i;
            }

            LoadedStaticMesh out;
            out.mesh = std::move(mesh);
            out.localTransform[0] = uniformScale;
            out.localTransform[5] = uniformScale;
            out.localTransform[10] = uniformScale;
            outMeshes.push_back(std::move(out));
            return true;
        }

        if (format == StaticModelFormat::Fbx) {
            // The FBX loader applies uniformScale itself, so no further scaling is done here.
            return LoadStaticModelFbx(path, uniformScale, outMeshes);
        }

        if (format != StaticModelFormat::Gltf) {
            return false;
        }

        GltfScene scene;
        if (!LoadGLTFStatic(path, scene)) {
            return false;
        }

        const float scale = uniformScale;
        // Uniform scaling matrix applied to imported primitive transform.
        const float scaleM[16] = {
            scale, 0, 0, 0,
            0, scale, 0, 0,
            0, 0, scale, 0,
            0, 0, 0, 1
        };

        outMeshes.reserve(scene.primitives.size());
        for (auto& prim : scene.primitives) {
            LoadedStaticMesh out;
            out.mesh = std::move(prim.mesh);
            // Final local transform = sourcePrimitiveTransform * uniformScale
            // (scales world-space result including translations, not just local vertices).
            Mul4x4(prim.transform, scaleM, out.localTransform);
            if (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size())) {
                const GltfMaterial& material = scene.materials[prim.materialIndex];
                out.material.baseColor[0] = material.baseColorFactor[0];
                out.material.baseColor[1] = material.baseColorFactor[1];
                out.material.baseColor[2] = material.baseColorFactor[2];
                out.material.baseColor[3] = material.baseColorFactor[3];
                out.material.emissive[0] = material.emissiveFactor[0];
                out.material.emissive[1] = material.emissiveFactor[1];
                out.material.emissive[2] = material.emissiveFactor[2];
                out.material.metallic = material.metallicFactor;
                out.material.roughness = material.roughnessFactor;
                out.material.reflectionStrength = DefaultReflectionStrength(out.material.roughness, out.material.metallic);
                out.material.occlusionStrength = material.occlusionStrength;
                out.material.ior = material.ior;
                out.material.thickness = material.thicknessFactor;
                out.material.attenuationColor[0] = material.attenuationColor[0];
                out.material.attenuationColor[1] = material.attenuationColor[1];
                out.material.attenuationColor[2] = material.attenuationColor[2];
                out.material.attenuationDistance = material.attenuationDistance;
                out.material.doubleSided = material.doubleSided;
                if (material.alphaBlend) {
                    out.material.transmission = 1.0f - material.baseColorFactor[3];
                }
                out.material.transmission = std::max(out.material.transmission, material.transmissionFactor);

                const int baseColorIndex = material.baseColorTexture;
                if (baseColorIndex >= 0 && baseColorIndex < static_cast<int>(scene.textures.size())) {
                    const int imageIndex = scene.textures[baseColorIndex].imageIndex;
                    if (imageIndex >= 0 && imageIndex < static_cast<int>(scene.images.size())) {
                        out.texturePath = scene.images[imageIndex].uri;
                    }
                }

                const int occlusionIndex = material.occlusionTexture;
                if (occlusionIndex >= 0 && occlusionIndex < static_cast<int>(scene.textures.size())) {
                    const int imageIndex = scene.textures[occlusionIndex].imageIndex;
                    if (imageIndex >= 0 && imageIndex < static_cast<int>(scene.images.size())) {
                        out.occlusionTexturePath = scene.images[imageIndex].uri;
                    }
                }

                const int normalIndex = material.normalTexture;
                if (normalIndex >= 0 && normalIndex < static_cast<int>(scene.textures.size())) {
                    const int imageIndex = scene.textures[normalIndex].imageIndex;
                    if (imageIndex >= 0 && imageIndex < static_cast<int>(scene.images.size())) {
                        out.normalTexturePath = scene.images[imageIndex].uri;
                    }
                }

                const int metallicRoughnessIndex = material.metallicRoughnessTexture;
                if (metallicRoughnessIndex >= 0 && metallicRoughnessIndex < static_cast<int>(scene.textures.size())) {
                    const int imageIndex = scene.textures[metallicRoughnessIndex].imageIndex;
                    if (imageIndex >= 0 && imageIndex < static_cast<int>(scene.images.size())) {
                        out.metallicRoughnessTexturePath = scene.images[imageIndex].uri;
                    }
                }
            }
            outMeshes.push_back(std::move(out));
        }

        return !outMeshes.empty();
    }

    bool LoadStaticModel(const std::string& path,
                         StaticModelFormat format,
                         float uniformScale,
                         std::vector<LoadedStaticMesh>& outMeshes)
    {
        outMeshes.clear();

        // Parsing and converting a large scene is the dominant startup cost, and the
        // result is format-independent, so cache it here rather than per loader: glTF,
        // OBJ and FBX all benefit. A miss (or any malformed cache) just re-parses.
        const std::filesystem::path cachePath = StaticMeshCache::ResolveCachePath(path, uniformScale);
        if (StaticMeshCache::IsUpToDate(cachePath, path, uniformScale) &&
            StaticMeshCache::Load(cachePath, outMeshes) &&
            !outMeshes.empty()) {
            return true;
        }

        outMeshes.clear(); // A partially-read cache must not leak into the parsed result.
        if (!LoadStaticModelUncached(path, format, uniformScale, outMeshes)) {
            return false;
        }

        StaticMeshCache::Save(cachePath, path, uniformScale, outMeshes); // best-effort
        return true;
    }
}
