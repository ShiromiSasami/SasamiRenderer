// ModelLoaderSkinned.cpp
// glTF2 skinned mesh loader.
#include "ModelLoader.h"
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
            // Bind-pose local TRS (identity when the node uses "matrix" instead of TRS).
            float bindT[3] = { 0,0,0 };
            float bindR[4] = { 0,0,0,1 };
            float bindS[3] = { 1,1,1 };
        };

        // outT/outR/outS receive the node's bind-pose local TRS (identity when the
        // node uses an explicit "matrix" property instead of TRS — decomposition
        // of that case is not implemented, see Node::bindT/bindR/bindS comment).
        static void ExtractNodeTransform(const Value& node, float out[16], float outT[3], float outR[4], float outS[3])
        {
            Identity(out);
            outT[0] = outT[1] = outT[2] = 0.0f;
            outR[0] = outR[1] = outR[2] = 0.0f; outR[3] = 1.0f;
            outS[0] = outS[1] = outS[2] = 1.0f;
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
                if (ReadVec(node["translation"], outT, 3)) {
                    BuildTranslation(outT, T);
                }
            }
            if (node.HasMember("rotation")) {
                if (ReadVec(node["rotation"], outR, 4)) {
                    BuildRotationFromQuat(outR, R);
                }
            }
            if (node.HasMember("scale")) {
                if (ReadVec(node["scale"], outS, 3)) {
                    BuildScale(outS, S);
                }
            }
            // TRS composition in row-vector convention (matches glTF column-major T*R*S):
            // local = S * R * T
            float SR[16]; Mul4x4(S, R, SR);
            Mul4x4(SR, T, out);
        }

    }

    bool LoadGLTFSkinned(const std::string& path, SkinnedModelData& outData)
    {
        outData = {};
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

        // Nodes (for transform hierarchy and parent resolution)
        std::vector<Node> nodes;
        if (doc.HasMember("nodes") && doc["nodes"].IsArray()) {
            const auto& ns = doc["nodes"];
            nodes.resize(ns.Size());
            for (rapidjson::SizeType i = 0; i < ns.Size(); ++i) {
                const auto& n = ns[i];
                nodes[i].mesh = n.HasMember("mesh") ? n["mesh"].GetInt() : -1;
                if (n.HasMember("children") && n["children"].IsArray()) {
                    for (auto& c : n["children"].GetArray()) nodes[i].children.push_back(c.GetInt());
                }
                ExtractNodeTransform(n, nodes[i].local, nodes[i].bindT, nodes[i].bindR, nodes[i].bindS);
            }
        }

        // Images and textures (for texture paths)
        std::vector<std::string> imagePaths;
        if (doc.HasMember("images") && doc["images"].IsArray()) {
            const auto& imgs = doc["images"];
            imagePaths.resize(imgs.Size());
            for (rapidjson::SizeType i = 0; i < imgs.Size(); ++i) {
                if (imgs[i].HasMember("uri"))
                    imagePaths[i] = (basePath / imgs[i]["uri"].GetString()).string();
            }
        }
        std::vector<int> textureToImage;
        if (doc.HasMember("textures") && doc["textures"].IsArray()) {
            const auto& texs = doc["textures"];
            textureToImage.resize(texs.Size(), -1);
            for (rapidjson::SizeType i = 0; i < texs.Size(); ++i) {
                if (texs[i].HasMember("source")) textureToImage[i] = texs[i]["source"].GetInt();
            }
        }

        // Materials (reuse GltfMaterial parsing)
        std::vector<GltfMaterial> materials;
        if (doc.HasMember("materials") && doc["materials"].IsArray()) {
            const auto& mats = doc["materials"];
            materials.resize(mats.Size());
            for (rapidjson::SizeType i = 0; i < mats.Size(); ++i) {
                const auto& m = mats[i];
                if (!m.IsObject()) continue;
                GltfMaterial& mat = materials[i];
                if (m.HasMember("pbrMetallicRoughness")) {
                    const auto& pbr = m["pbrMetallicRoughness"];
                    if (pbr.HasMember("baseColorTexture") && pbr["baseColorTexture"].HasMember("index"))
                        mat.baseColorTexture = pbr["baseColorTexture"]["index"].GetInt();
                    if (pbr.HasMember("occlusionTexture") && pbr["occlusionTexture"].HasMember("index"))
                        mat.occlusionTexture = pbr["occlusionTexture"]["index"].GetInt();
                    if (pbr.HasMember("baseColorFactor")) ReadVec(pbr["baseColorFactor"], mat.baseColorFactor, 4);
                    if (pbr.HasMember("metallicFactor"))  mat.metallicFactor  = pbr["metallicFactor"].GetFloat();
                    if (pbr.HasMember("roughnessFactor")) mat.roughnessFactor = pbr["roughnessFactor"].GetFloat();
                }
                if (m.HasMember("occlusionTexture") && m["occlusionTexture"].IsObject()) {
                    const auto& occ = m["occlusionTexture"];
                    if (occ.HasMember("index"))    mat.occlusionTexture  = occ["index"].GetInt();
                    if (occ.HasMember("strength")) mat.occlusionStrength = occ["strength"].GetFloat();
                }
                if (m.HasMember("normalTexture") && m["normalTexture"].IsObject()) {
                    const auto& nrm = m["normalTexture"];
                    if (nrm.HasMember("index")) mat.normalTexture = nrm["index"].GetInt();
                }
                if (m.HasMember("doubleSided") && m["doubleSided"].IsBool()) {
                    mat.doubleSided = m["doubleSided"].GetBool();
                }
            }
        }

        // Skin 0 → Skeleton
        if (!doc.HasMember("skins") || !doc["skins"].IsArray() || doc["skins"].Empty()) return false;
        const auto& skin = doc["skins"][0];

        std::vector<int> jointNodeIndices;
        if (skin.HasMember("joints") && skin["joints"].IsArray()) {
            for (auto& j : skin["joints"].GetArray()) jointNodeIndices.push_back(j.GetInt());
        }
        const uint32_t boneCount = static_cast<uint32_t>(jointNodeIndices.size());
        if (boneCount == 0 || boneCount > Skeleton::kMaxBones) return false;

        // node index → bone index reverse map
        std::unordered_map<int, int> nodeToBone;
        for (int bi = 0; bi < static_cast<int>(boneCount); ++bi)
            nodeToBone[jointNodeIndices[bi]] = bi;

        outData.skeleton.boneCount = boneCount;

        // Build parent index array by traversing node children
        // Initialize all to -1 (root)
        for (uint32_t i = 0; i < boneCount; ++i) outData.skeleton.parentIndex[i] = -1;
        for (uint32_t bi = 0; bi < boneCount; ++bi) {
            int nodeIdx = jointNodeIndices[bi];
            if (nodeIdx < 0 || nodeIdx >= (int)nodes.size()) continue;
            for (int child : nodes[nodeIdx].children) {
                auto it = nodeToBone.find(child);
                if (it != nodeToBone.end())
                    outData.skeleton.parentIndex[it->second] = static_cast<int32_t>(bi);
            }
        }

        // Bone names from node names
        for (uint32_t bi = 0; bi < boneCount; ++bi) {
            int nodeIdx = jointNodeIndices[bi];
            if (nodeIdx >= 0 && nodeIdx < (int)doc["nodes"].Size()) {
                const auto& n = doc["nodes"][nodeIdx];
                if (n.HasMember("name") && n["name"].IsString()) {
                    const char* nm = n["name"].GetString();
                    strncpy_s(outData.skeleton.boneName[bi], sizeof(outData.skeleton.boneName[bi]), nm, _TRUNCATE);
                }
            }
        }

        // Bind-pose local TRS per bone (fallback for animation channels the
        // playing clip doesn't drive — see AnimationController::SetSkeleton).
        for (uint32_t bi = 0; bi < boneCount; ++bi) {
            int nodeIdx = jointNodeIndices[bi];
            if (nodeIdx < 0 || nodeIdx >= (int)nodes.size()) continue;
            const Node& node = nodes[nodeIdx];
            std::memcpy(outData.skeleton.bindLocalT[bi], node.bindT, sizeof(float)*3);
            std::memcpy(outData.skeleton.bindLocalR[bi], node.bindR, sizeof(float)*4);
            std::memcpy(outData.skeleton.bindLocalS[bi], node.bindS, sizeof(float)*3);
        }

        // Inverse bind matrices (column-major float4x4 per bone)
        {
            // Default to identity
            for (uint32_t i = 0; i < boneCount; ++i) {
                float* ibp = outData.skeleton.inverseBindPose[i];
                std::memset(ibp, 0, sizeof(float)*16);
                ibp[0]=ibp[5]=ibp[10]=ibp[15]=1.0f;
            }

            if (skin.HasMember("inverseBindMatrices")) {
                const int ibmAccIdx = skin["inverseBindMatrices"].GetInt();
                if (ibmAccIdx >= 0 && ibmAccIdx < (int)accessors.size()) {
                    const Accessor& ibmAcc = accessors[ibmAccIdx];
                    if (ibmAcc.bufferView >= 0 && ibmAcc.bufferView < (int)views.size()) {
                        const BufferView& ibmView = views[ibmAcc.bufferView];
                        if (ibmView.buffer >= 0 && ibmView.buffer < (int)buffers.size()) {
                            const std::vector<uint8_t>& ibmBuf = buffers[ibmView.buffer];
                            for (uint32_t bi = 0; bi < boneCount && bi < ibmAcc.count; ++bi) {
                                const uint8_t* ptr = GetAccessorPtr(ibmBuf, ibmView, ibmAcc, bi);
                                if (ptr && ibmAcc.componentType == 5126 /*FLOAT*/) {
                                    // glTF MAT4 is column-major — store as-is
                                    std::memcpy(outData.skeleton.inverseBindPose[bi], ptr, sizeof(float)*16);
                                }
                            }
                        }
                    }
                }
            }
        }

        // Animations
        if (doc.HasMember("animations") && doc["animations"].IsArray()) {
            const auto& anims = doc["animations"];
            outData.animations.reserve(anims.Size());
            for (rapidjson::SizeType ai = 0; ai < anims.Size(); ++ai) {
                const auto& anim = anims[ai];

                SkeletonAnimation sa;
                sa.name = anim.HasMember("name") ? anim["name"].GetString() : "";
                sa.boneTracks.resize(boneCount);

                if (!anim.HasMember("samplers") || !anim.HasMember("channels")) continue;
                const auto& samplers  = anim["samplers"];
                const auto& channels  = anim["channels"];

                float maxTime = 0.0f;

                for (rapidjson::SizeType ci = 0; ci < channels.Size(); ++ci) {
                    const auto& ch = channels[ci];
                    if (!ch.HasMember("sampler") || !ch.HasMember("target")) continue;
                    const auto& target = ch["target"];
                    if (!target.HasMember("node") || !target.HasMember("path")) continue;

                    const int targetNode = target["node"].GetInt();
                    const std::string tpath = target["path"].GetString();
                    auto boneIt = nodeToBone.find(targetNode);
                    if (boneIt == nodeToBone.end()) continue;
                    const int boneIdx = boneIt->second;

                    const int samplerIdx = ch["sampler"].GetInt();
                    if (samplerIdx < 0 || samplerIdx >= (int)samplers.Size()) continue;
                    const auto& sampler = samplers[samplerIdx];
                    if (!sampler.HasMember("input") || !sampler.HasMember("output")) continue;

                    const int inputAccIdx  = sampler["input"].GetInt();
                    const int outputAccIdx = sampler["output"].GetInt();
                    if (inputAccIdx  < 0 || inputAccIdx  >= (int)accessors.size()) continue;
                    if (outputAccIdx < 0 || outputAccIdx >= (int)accessors.size()) continue;

                    const Accessor& inputAcc  = accessors[inputAccIdx];
                    const Accessor& outputAcc = accessors[outputAccIdx];
                    if (inputAcc.bufferView  < 0 || inputAcc.bufferView  >= (int)views.size()) continue;
                    if (outputAcc.bufferView < 0 || outputAcc.bufferView >= (int)views.size()) continue;
                    const BufferView& inputView  = views[inputAcc.bufferView];
                    const BufferView& outputView = views[outputAcc.bufferView];
                    if (inputView.buffer  < 0 || inputView.buffer  >= (int)buffers.size()) continue;
                    if (outputView.buffer < 0 || outputView.buffer >= (int)buffers.size()) continue;
                    const std::vector<uint8_t>& inputBuf  = buffers[inputView.buffer];
                    const std::vector<uint8_t>& outputBuf = buffers[outputView.buffer];

                    const size_t frameCount = inputAcc.count;
                    const int outComponents = (tpath == "rotation") ? 4 : 3;

                    std::vector<AnimKeyframe> frames;
                    frames.reserve(frameCount);
                    for (size_t fi = 0; fi < frameCount; ++fi) {
                        AnimKeyframe kf{};
                        // Time (FLOAT scalar)
                        const uint8_t* tptr = GetAccessorPtr(inputBuf, inputView, inputAcc, fi);
                        if (tptr && inputAcc.componentType == 5126)
                            kf.time = *reinterpret_cast<const float*>(tptr);
                        // Value
                        const uint8_t* vptr = GetAccessorPtr(outputBuf, outputView, outputAcc, fi);
                        if (vptr && outputAcc.componentType == 5126) {
                            const float* vf = reinterpret_cast<const float*>(vptr);
                            for (int c = 0; c < outComponents; ++c) kf.value[c] = vf[c];
                        }
                        maxTime = std::max(maxTime, kf.time);
                        frames.push_back(kf);
                    }

                    BoneTrack& bt = sa.boneTracks[boneIdx];
                    if      (tpath == "translation") bt.translation = std::move(frames);
                    else if (tpath == "rotation")    bt.rotation    = std::move(frames);
                    else if (tpath == "scale")       bt.scale       = std::move(frames);
                }

                sa.durationSec = maxTime;
                outData.animations.push_back(std::move(sa));
            }
        }

        // Meshes — look for primitives with JOINTS_0 + WEIGHTS_0
        if (!doc.HasMember("meshes") || !doc["meshes"].IsArray()) return false;
        const auto& meshes = doc["meshes"];

        for (rapidjson::SizeType mi = 0; mi < meshes.Size(); ++mi) {
            const auto& m = meshes[mi];
            if (!m.HasMember("primitives") || !m["primitives"].IsArray()) continue;
            for (const auto& prim : m["primitives"].GetArray()) {
                if (!prim.HasMember("attributes")) continue;
                const auto& attrs = prim["attributes"];

                const int posAccIdx    = attrs.HasMember("POSITION")   ? attrs["POSITION"].GetInt()   : -1;
                const int norAccIdx    = attrs.HasMember("NORMAL")     ? attrs["NORMAL"].GetInt()     : -1;
                const int uvAccIdx     = attrs.HasMember("TEXCOORD_0") ? attrs["TEXCOORD_0"].GetInt() : -1;
                const int jointsAccIdx = attrs.HasMember("JOINTS_0")   ? attrs["JOINTS_0"].GetInt()   : -1;
                const int weightsAccIdx= attrs.HasMember("WEIGHTS_0")  ? attrs["WEIGHTS_0"].GetInt()  : -1;
                const int idxAccIdx    = prim.HasMember("indices")     ? prim["indices"].GetInt()     : -1;
                const int matIdx       = prim.HasMember("material")    ? prim["material"].GetInt()    : -1;

                if (posAccIdx < 0 || posAccIdx >= (int)accessors.size()) continue;
                const Accessor& posA = accessors[posAccIdx];
                if (posA.bufferView < 0 || posA.bufferView >= (int)views.size()) continue;
                const BufferView& posView = views[posA.bufferView];
                if (posView.buffer < 0 || posView.buffer >= (int)buffers.size()) continue;
                const std::vector<uint8_t>& posBuf = buffers[posView.buffer];

                std::vector<uint32_t> indices;
                if (idxAccIdx >= 0 && idxAccIdx < (int)accessors.size()) {
                    const Accessor& idxA = accessors[idxAccIdx];
                    if (idxA.bufferView >= 0 && idxA.bufferView < (int)views.size()) {
                        const BufferView& idxView = views[idxA.bufferView];
                        if (idxView.buffer >= 0 && idxView.buffer < (int)buffers.size())
                            LoadIndices(buffers[idxView.buffer], idxView, idxA, indices);
                    }
                }

                const size_t vertCount = posA.count;
                std::vector<SkinnedVertex> verts(vertCount);

                for (size_t vi = 0; vi < vertCount; ++vi) {
                    SkinnedVertex& sv = verts[vi];

                    // Position
                    float p[3] = {0,0,0};
                    LoadFloat3(posBuf, posView, posA, vi, p);
                    sv.position[0]=p[0]; sv.position[1]=p[1]; sv.position[2]=p[2];

                    // Normal
                    float n[3] = {0,1,0};
                    if (norAccIdx >= 0 && norAccIdx < (int)accessors.size()) {
                        const Accessor& norA = accessors[norAccIdx];
                        if (norA.bufferView >= 0 && norA.bufferView < (int)views.size()) {
                            const BufferView& norView = views[norA.bufferView];
                            if (norView.buffer >= 0 && norView.buffer < (int)buffers.size())
                                LoadFloat3(buffers[norView.buffer], norView, norA, vi, n);
                        }
                    }
                    sv.normal[0]=n[0]; sv.normal[1]=n[1]; sv.normal[2]=n[2];
                    sv.color[0]=sv.color[1]=sv.color[2]=sv.color[3]=1.0f;

                    // UV
                    float uv[2] = {0,0};
                    if (uvAccIdx >= 0 && uvAccIdx < (int)accessors.size()) {
                        const Accessor& uvA = accessors[uvAccIdx];
                        if (uvA.bufferView >= 0 && uvA.bufferView < (int)views.size()) {
                            const BufferView& uvView = views[uvA.bufferView];
                            if (uvView.buffer >= 0 && uvView.buffer < (int)buffers.size())
                                LoadFloat2(buffers[uvView.buffer], uvView, uvA, vi, uv);
                        }
                    }
                    sv.uv[0]=uv[0]; sv.uv[1]=uv[1];

                    // Bone indices (UNSIGNED_BYTE or UNSIGNED_SHORT → uint8)
                    sv.boneIndices[0]=sv.boneIndices[1]=sv.boneIndices[2]=sv.boneIndices[3]=0;
                    if (jointsAccIdx >= 0 && jointsAccIdx < (int)accessors.size()) {
                        const Accessor& jA = accessors[jointsAccIdx];
                        if (jA.bufferView >= 0 && jA.bufferView < (int)views.size()) {
                            const BufferView& jView = views[jA.bufferView];
                            if (jView.buffer >= 0 && jView.buffer < (int)buffers.size()) {
                                const uint8_t* jptr = GetAccessorPtr(buffers[jView.buffer], jView, jA, vi);
                                if (jptr) {
                                    if (jA.componentType == 5121) { // UNSIGNED_BYTE
                                        sv.boneIndices[0] = jptr[0];
                                        sv.boneIndices[1] = jptr[1];
                                        sv.boneIndices[2] = jptr[2];
                                        sv.boneIndices[3] = jptr[3];
                                    } else if (jA.componentType == 5123) { // UNSIGNED_SHORT
                                        const uint16_t* js = reinterpret_cast<const uint16_t*>(jptr);
                                        sv.boneIndices[0] = static_cast<uint8_t>(js[0]);
                                        sv.boneIndices[1] = static_cast<uint8_t>(js[1]);
                                        sv.boneIndices[2] = static_cast<uint8_t>(js[2]);
                                        sv.boneIndices[3] = static_cast<uint8_t>(js[3]);
                                    }
                                }
                            }
                        }
                    }
                    for (int k = 0; k < 4; ++k) {
                        if (sv.boneIndices[k] >= boneCount) sv.boneIndices[k] = 0;
                    }

                    // Bone weights (FLOAT)
                    sv.boneWeights[0]=1.0f; sv.boneWeights[1]=sv.boneWeights[2]=sv.boneWeights[3]=0.0f;
                    if (weightsAccIdx >= 0 && weightsAccIdx < (int)accessors.size()) {
                        const Accessor& wA = accessors[weightsAccIdx];
                        if (wA.bufferView >= 0 && wA.bufferView < (int)views.size()) {
                            const BufferView& wView = views[wA.bufferView];
                            if (wView.buffer >= 0 && wView.buffer < (int)buffers.size()) {
                                const uint8_t* wptr = GetAccessorPtr(buffers[wView.buffer], wView, wA, vi);
                                if (wptr && wA.componentType == 5126) {
                                    const float* wf = reinterpret_cast<const float*>(wptr);
                                    sv.boneWeights[0]=wf[0]; sv.boneWeights[1]=wf[1];
                                    sv.boneWeights[2]=wf[2]; sv.boneWeights[3]=wf[3];
                                }
                            }
                        }
                    }
                }

                // Some skinned assets (e.g. glTF sample Fox) omit NORMAL; derive
                // smooth vertex normals from the triangle faces instead of the
                // (0,1,0) placeholder so lighting stays plausible.
                if (norAccIdx < 0 && indices.size() >= 3) {
                    std::vector<float> acc(vertCount * 3, 0.0f);
                    for (size_t ti = 0; ti + 2 < indices.size(); ti += 3) {
                        const uint32_t i0 = indices[ti], i1 = indices[ti + 1], i2 = indices[ti + 2];
                        if (i0 >= vertCount || i1 >= vertCount || i2 >= vertCount) continue;
                        const float* p0 = verts[i0].position;
                        const float* p1 = verts[i1].position;
                        const float* p2 = verts[i2].position;
                        const float e1[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
                        const float e2[3] = { p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] };
                        const float fn[3] = { e1[1]*e2[2] - e1[2]*e2[1],
                                              e1[2]*e2[0] - e1[0]*e2[2],
                                              e1[0]*e2[1] - e1[1]*e2[0] };
                        for (uint32_t vi3 : { i0, i1, i2 }) {
                            acc[vi3*3+0] += fn[0]; acc[vi3*3+1] += fn[1]; acc[vi3*3+2] += fn[2];
                        }
                    }
                    for (size_t vi2 = 0; vi2 < vertCount; ++vi2) {
                        const float len = std::sqrt(acc[vi2*3]*acc[vi2*3] +
                                                    acc[vi2*3+1]*acc[vi2*3+1] +
                                                    acc[vi2*3+2]*acc[vi2*3+2]);
                        if (len > 1e-8f) {
                            verts[vi2].normal[0] = acc[vi2*3]   / len;
                            verts[vi2].normal[1] = acc[vi2*3+1] / len;
                            verts[vi2].normal[2] = acc[vi2*3+2] / len;
                        }
                    }
                }

                SkinnedMesh smesh;
                smesh.vertices = std::move(verts);
                smesh.indices  = std::move(indices);
                outData.meshes.push_back(std::move(smesh));

                // Texture paths
                std::string albedoPath, occlusionPath, normalPath;
                SurfaceMaterial mat{};
                if (matIdx >= 0 && matIdx < (int)materials.size()) {
                    const GltfMaterial& gm = materials[matIdx];
                    mat.baseColor[0]=gm.baseColorFactor[0]; mat.baseColor[1]=gm.baseColorFactor[1];
                    mat.baseColor[2]=gm.baseColorFactor[2]; mat.baseColor[3]=gm.baseColorFactor[3];
                    mat.metallic  = gm.metallicFactor;
                    mat.roughness = gm.roughnessFactor;
                    mat.reflectionStrength = DefaultReflectionStrength(mat.roughness, mat.metallic);
                    mat.occlusionStrength  = gm.occlusionStrength;
                    mat.doubleSided        = gm.doubleSided;
                    if (gm.baseColorTexture >= 0 && gm.baseColorTexture < (int)textureToImage.size()) {
                        const int ii = textureToImage[gm.baseColorTexture];
                        if (ii >= 0 && ii < (int)imagePaths.size()) albedoPath = imagePaths[ii];
                    }
                    if (gm.occlusionTexture >= 0 && gm.occlusionTexture < (int)textureToImage.size()) {
                        const int ii = textureToImage[gm.occlusionTexture];
                        if (ii >= 0 && ii < (int)imagePaths.size()) occlusionPath = imagePaths[ii];
                    }
                    if (gm.normalTexture >= 0 && gm.normalTexture < (int)textureToImage.size()) {
                        const int ii = textureToImage[gm.normalTexture];
                        if (ii >= 0 && ii < (int)imagePaths.size()) normalPath = imagePaths[ii];
                    }
                }
                outData.albedoTexturePaths.push_back(std::move(albedoPath));
                outData.occlusionTexturePaths.push_back(std::move(occlusionPath));
                outData.normalTexturePaths.push_back(std::move(normalPath));
                outData.materials.push_back(mat);
            }
        }

        return !outData.meshes.empty();
    }


} // namespace SasamiRenderer
