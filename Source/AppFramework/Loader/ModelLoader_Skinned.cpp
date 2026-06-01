// ModelLoader_Skinned.cpp
// glTF2 skinned mesh loader.
#include "ModelLoader.h"
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

    static float Clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
    static float DefaultReflectionStrength(float roughness, float metallic)
    {
        return Clamp01(Clamp01(metallic) * (1.0f - Clamp01(roughness)));
    }

    namespace {
        using rapidjson::Value;

        struct BufferView {
            int buffer = -1;
            size_t byteOffset = 0;
            size_t byteLength = 0;
            size_t byteStride = 0;
        };

        struct Accessor {
            int bufferView = -1;
            size_t byteOffset = 0;
            int componentType = 0;
            size_t count = 0;
            std::string type;
            bool normalized = false;
        };

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

        static bool ReadFileText(const std::filesystem::path& path, std::string& out)
        {
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs.is_open()) return false;
            std::ostringstream ss;
            ss << ifs.rdbuf();
            out = ss.str();
            return true;
        }

        static bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out)
        {
            std::ifstream ifs(path, std::ios::binary | std::ios::ate);
            if (!ifs.is_open()) return false;
            std::streamsize size = ifs.tellg();
            if (size <= 0) return false;
            ifs.seekg(0, std::ios::beg);
            out.resize(static_cast<size_t>(size));
            return static_cast<bool>(ifs.read(reinterpret_cast<char*>(out.data()), size));
        }

        static int NumComponents(const std::string& type)
        {
            if (type == "SCALAR") return 1;
            if (type == "VEC2") return 2;
            if (type == "VEC3") return 3;
            if (type == "VEC4") return 4;
            if (type == "MAT4") return 16;
            return 0;
        }

        static size_t ComponentSize(int componentType)
        {
            switch (componentType) {
            case 5120: return 1; // BYTE
            case 5121: return 1; // UNSIGNED_BYTE
            case 5122: return 2; // SHORT
            case 5123: return 2; // UNSIGNED_SHORT
            case 5125: return 4; // UNSIGNED_INT
            case 5126: return 4; // FLOAT
            default: return 0;
            }
        }

        static void Identity(float out[16])
        {
            for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        }

        static void BuildTranslation(const float t[3], float out[16])
        {
            Identity(out);
            // Row-major affine translation in last row (row-vector convention).
            out[12] = t[0];
            out[13] = t[1];
            out[14] = t[2];
        }

        static void BuildScale(const float s[3], float out[16])
        {
            Identity(out);
            // Non-uniform scale on matrix diagonal.
            out[0] = s[0];
            out[5] = s[1];
            out[10] = s[2];
        }

        static void BuildRotationFromQuat(const float q[4], float out[16])
        {
            // Quaternion q=(x,y,z,w) to rotation matrix.
            // Standard formula derived from q * p * q^{-1}.
            const float x = q[0], y = q[1], z = q[2], w = q[3];
            const float xx = x * x;
            const float yy = y * y;
            const float zz = z * z;
            const float xy = x * y;
            const float xz = x * z;
            const float yz = y * z;
            const float wx = w * x;
            const float wy = w * y;
            const float wz = w * z;

            // Compute canonical 3x3 rotation entries.
            // glTF stores transforms in column-major; engine stores row-major.
            // So values are placed transposed into the row-major matrix.
            float m00 = 1.0f - 2.0f * (yy + zz);
            float m01 = 2.0f * (xy - wz);
            float m02 = 2.0f * (xz + wy);
            float m10 = 2.0f * (xy + wz);
            float m11 = 1.0f - 2.0f * (xx + zz);
            float m12 = 2.0f * (yz - wx);
            float m20 = 2.0f * (xz - wy);
            float m21 = 2.0f * (yz + wx);
            float m22 = 1.0f - 2.0f * (xx + yy);

            Identity(out);
            out[0] = m00; out[1] = m10; out[2] = m20;
            out[4] = m01; out[5] = m11; out[6] = m21;
            out[8] = m02; out[9] = m12; out[10] = m22;
        }

        static bool ReadVec(const Value& arr, float* out, int n)
        {
            if (!arr.IsArray() || (int)arr.Size() < n) return false;
            for (int i = 0; i < n; ++i) out[i] = arr[i].GetFloat();
            return true;
        }

        static const uint8_t* GetAccessorPtr(const std::vector<uint8_t>& buffer,
                                             const BufferView& view,
                                             const Accessor& acc,
                                             size_t index)
        {
            const size_t compSize = ComponentSize(acc.componentType);
            const int comps = NumComponents(acc.type);
            if (compSize == 0 || comps == 0) return nullptr;
            size_t stride = view.byteStride;
            if (stride == 0) stride = compSize * static_cast<size_t>(comps);
            size_t offset = view.byteOffset + acc.byteOffset + index * stride;
            if (offset + compSize * static_cast<size_t>(comps) > buffer.size()) return nullptr;
            return buffer.data() + offset;
        }

        static bool LoadFloat3(const std::vector<uint8_t>& buffer, const BufferView& view, const Accessor& acc,
                               size_t index, float out[3])
        {
            const uint8_t* ptr = GetAccessorPtr(buffer, view, acc, index);
            if (!ptr) return false;
            if (acc.componentType == 5126) { // float
                const float* f = reinterpret_cast<const float*>(ptr);
                out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
                return true;
            }
            return false;
        }

        static bool LoadFloat2(const std::vector<uint8_t>& buffer, const BufferView& view, const Accessor& acc,
                               size_t index, float out[2])
        {
            const uint8_t* ptr = GetAccessorPtr(buffer, view, acc, index);
            if (!ptr) return false;
            if (acc.componentType == 5126) { // float
                const float* f = reinterpret_cast<const float*>(ptr);
                out[0] = f[0]; out[1] = f[1];
                return true;
            }
            return false;
        }

        static bool LoadIndices(const std::vector<uint8_t>& buffer, const BufferView& view, const Accessor& acc,
                                std::vector<uint32_t>& out)
        {
            const int comps = NumComponents(acc.type);
            if (comps != 1) return false;
            const size_t compSize = ComponentSize(acc.componentType);
            if (compSize == 0) return false;
            const size_t stride = view.byteStride == 0 ? compSize : view.byteStride;
            out.resize(acc.count);
            for (size_t i = 0; i < acc.count; ++i) {
                const uint8_t* ptr = buffer.data() + view.byteOffset + acc.byteOffset + i * stride;
                if (ptr + compSize > buffer.data() + buffer.size()) return false;
                uint32_t idx = 0;
                switch (acc.componentType) {
                case 5121: idx = *reinterpret_cast<const uint8_t*>(ptr); break;
                case 5123: idx = *reinterpret_cast<const uint16_t*>(ptr); break;
                case 5125: idx = *reinterpret_cast<const uint32_t*>(ptr); break;
                default: return false;
                }
                out[i] = idx;
            }
            return true;
        }

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

    bool LoadGLTFSkinned(const std::string& path, SkinnedModelData& outData)
    {
        outData = {};
        std::filesystem::path basePath = std::filesystem::path(path).parent_path();

        std::string jsonText;
        if (!ReadFileText(std::filesystem::path(path), jsonText)) return false;

        rapidjson::Document doc;
        doc.Parse(jsonText.c_str());
        if (!doc.IsObject()) return false;

        // Buffers
        std::vector<std::vector<uint8_t>> buffers;
        if (doc.HasMember("buffers") && doc["buffers"].IsArray()) {
            const auto& bufs = doc["buffers"];
            buffers.resize(bufs.Size());
            for (rapidjson::SizeType i = 0; i < bufs.Size(); ++i) {
                const auto& b = bufs[i];
                if (!b.IsObject() || !b.HasMember("uri")) continue;
                std::string uri = b["uri"].GetString();
                ReadFileBytes(basePath / uri, buffers[i]);
            }
        }

        // Buffer views
        std::vector<BufferView> views;
        if (doc.HasMember("bufferViews") && doc["bufferViews"].IsArray()) {
            const auto& v = doc["bufferViews"];
            views.resize(v.Size());
            for (rapidjson::SizeType i = 0; i < v.Size(); ++i) {
                const auto& bv = v[i];
                if (!bv.IsObject()) continue;
                views[i].buffer     = bv.HasMember("buffer")     ? bv["buffer"].GetInt()    : -1;
                views[i].byteOffset = bv.HasMember("byteOffset") ? bv["byteOffset"].GetUint(): 0;
                views[i].byteLength = bv.HasMember("byteLength") ? bv["byteLength"].GetUint(): 0;
                views[i].byteStride = bv.HasMember("byteStride") ? bv["byteStride"].GetUint(): 0;
            }
        }

        // Accessors
        std::vector<Accessor> accessors;
        if (doc.HasMember("accessors") && doc["accessors"].IsArray()) {
            const auto& a = doc["accessors"];
            accessors.resize(a.Size());
            for (rapidjson::SizeType i = 0; i < a.Size(); ++i) {
                const auto& acc = a[i];
                if (!acc.IsObject()) continue;
                accessors[i].bufferView   = acc.HasMember("bufferView")   ? acc["bufferView"].GetInt()   : -1;
                accessors[i].byteOffset   = acc.HasMember("byteOffset")   ? acc["byteOffset"].GetUint()  : 0;
                accessors[i].componentType= acc.HasMember("componentType") ? acc["componentType"].GetInt(): 0;
                accessors[i].count        = acc.HasMember("count")         ? acc["count"].GetUint()       : 0;
                accessors[i].type         = acc.HasMember("type")          ? acc["type"].GetString()      : "";
                accessors[i].normalized   = acc.HasMember("normalized")    ? acc["normalized"].GetBool()  : false;
            }
        }

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
                ExtractNodeTransform(n, nodes[i].local);
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

                if (posAccIdx < 0) continue;
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

                SkinnedMesh smesh;
                smesh.vertices = std::move(verts);
                smesh.indices  = std::move(indices);
                outData.meshes.push_back(std::move(smesh));

                // Texture paths
                std::string albedoPath, occlusionPath;
                SurfaceMaterial mat{};
                if (matIdx >= 0 && matIdx < (int)materials.size()) {
                    const GltfMaterial& gm = materials[matIdx];
                    mat.baseColor[0]=gm.baseColorFactor[0]; mat.baseColor[1]=gm.baseColorFactor[1];
                    mat.baseColor[2]=gm.baseColorFactor[2]; mat.baseColor[3]=gm.baseColorFactor[3];
                    mat.metallic  = gm.metallicFactor;
                    mat.roughness = gm.roughnessFactor;
                    mat.reflectionStrength = DefaultReflectionStrength(mat.roughness, mat.metallic);
                    mat.occlusionStrength  = gm.occlusionStrength;
                    if (gm.baseColorTexture >= 0 && gm.baseColorTexture < (int)textureToImage.size()) {
                        const int ii = textureToImage[gm.baseColorTexture];
                        if (ii >= 0 && ii < (int)imagePaths.size()) albedoPath = imagePaths[ii];
                    }
                    if (gm.occlusionTexture >= 0 && gm.occlusionTexture < (int)textureToImage.size()) {
                        const int ii = textureToImage[gm.occlusionTexture];
                        if (ii >= 0 && ii < (int)imagePaths.size()) occlusionPath = imagePaths[ii];
                    }
                }
                outData.albedoTexturePaths.push_back(std::move(albedoPath));
                outData.occlusionTexturePaths.push_back(std::move(occlusionPath));
                outData.materials.push_back(mat);
            }
        }

        return !outData.meshes.empty();
    }


} // namespace SasamiRenderer
