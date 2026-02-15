#include "ModelLoader.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <array>
#include "Foundation/Math/MathUtil.h"
#include <rapidjson/document.h>

namespace SasamiRenderer
{
    using Math::Mul4x4;

	static inline std::string Trim(const std::string& s)
	{
		size_t a = s.find_first_not_of(" \t\r\n");
		if (a == std::string::npos) { return ""; }
		size_t b = s.find_last_not_of(" \t\r\n");
		return s.substr(a, b - a + 1);
	}

	bool LoadOBJ(const std::string& path, std::vector<Vertex>& outVertices)
	{
		outVertices.clear();

		std::filesystem::path p(path);
		std::ifstream ifs(p);
		if (!ifs.is_open()) { return false; }

		std::vector<std::array<float, 3>> positions;
		std::vector<std::array<float, 2>> texcoords;
		struct Face { std::vector<std::string> tokens; };
		std::vector<Face> faces;

		std::string line;
		while (std::getline(ifs, line)) {
			line = Trim(line);
			if (line.empty() || line[0] == '#') { continue; }

			std::stringstream ss(line);
			std::string head;
			ss >> head;

			if (head == "v") {
				float x = 0, y = 0, z = 0;
				ss >> x >> y >> z;
				positions.push_back({ x, y, z });
			}
			else if (head == "vt") {
				float u = 0, v = 0;
				ss >> u >> v;
				texcoords.push_back({ u, v });
			}
			else if (head == "f") {
				Face f;
				std::string tok;
				while (ss >> tok) { f.tokens.push_back(tok); }
				if (f.tokens.size() >= 3) { faces.push_back(std::move(f)); }
			}
		}

		ifs.close();

		if (positions.empty() || faces.empty()) { return false; }

		auto parseTok = [&](const std::string& vtkn, int& vi, int& ti){
			vi = 0; ti = 0; std::string a,b,c; size_t slash = vtkn.find('/');
			if (slash == std::string::npos) { a = vtkn; }
			else {
				a = vtkn.substr(0, slash);
				size_t slash2 = vtkn.find('/', slash + 1);
				if (slash2 == std::string::npos) { b = vtkn.substr(slash + 1); }
				else { b = vtkn.substr(slash + 1, slash2 - slash - 1); c = vtkn.substr(slash2 + 1); }
			}
			if (!a.empty()) { vi = std::stoi(a); }
			if (!b.empty()) { ti = std::stoi(b); }
		};

		auto getPos = [&](int idx, float out[3]){
			if (idx < 0) idx = static_cast<int>(positions.size()) + idx + 1;
			if (idx > 0 && idx <= (int)positions.size()) {
				auto& p = positions[idx - 1];
				out[0] = p[0];
				out[1] = p[1];
				out[2] = p[2];
			}
		};

		for (auto& f : faces) {
				auto emitTri = [&](const std::string& a, const std::string& b, const std::string& c){
					int ia, ib, ic, ita, itb, itc; parseTok(a, ia, ita); parseTok(b, ib, itb); parseTok(c, ic, itc);
					float p0[3]{}, p1[3]{}, p2[3]{}; getPos(ia, p0); getPos(ib, p1); getPos(ic, p2);
					// Face normal from cross product:
					// n = normalize((p1 - p0) x (p2 - p0))
					float e1[3]{ p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
					float e2[3]{ p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] };
					float n[3]{ e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
				float len = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]); if (len>0){ n[0]/=len; n[1]/=len; n[2]/=len; }
				auto pushV = [&](int vi, int ti, const float pos[3]){
					Vertex v{}; v.position[0]=pos[0]; v.position[1]=pos[1]; v.position[2]=pos[2]; v.normal[0]=n[0]; v.normal[1]=n[1]; v.normal[2]=n[2];
					v.color[0]=1; v.color[1]=1; v.color[2]=1; v.color[3]=1;
					if (ti < 0) ti = static_cast<int>(texcoords.size()) + ti + 1;
					if (ti > 0 && ti <= (int)texcoords.size()) { v.uv[0]=texcoords[ti-1][0]; v.uv[1]=texcoords[ti-1][1]; } else { v.uv[0]=0; v.uv[1]=0; }
					outVertices.push_back(v);
				};
				pushV(ia, ita, p0); pushV(ib, itb, p1); pushV(ic, itc, p2);
			};

			if (f.tokens.size() == 3) {
				emitTri(f.tokens[0], f.tokens[1], f.tokens[2]);
			}
			else {
				for (size_t i = 2; i < f.tokens.size(); ++i) {
					emitTri(f.tokens[0], f.tokens[i - 1], f.tokens[i]);
				}
			}
		}

		return !outVertices.empty();
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
            // TRS composition in row-vector convention:
            // local = T * R * S
            float TR[16]; Mul4x4(T, R, TR);
            Mul4x4(TR, S, out);
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
                std::filesystem::path binPath = basePath / uri;
                ReadFileBytes(binPath, buffers[i]);
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
                BufferView view;
                view.buffer = bv.HasMember("buffer") ? bv["buffer"].GetInt() : -1;
                view.byteOffset = bv.HasMember("byteOffset") ? bv["byteOffset"].GetUint() : 0;
                view.byteLength = bv.HasMember("byteLength") ? bv["byteLength"].GetUint() : 0;
                view.byteStride = bv.HasMember("byteStride") ? bv["byteStride"].GetUint() : 0;
                views[i] = view;
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
                Accessor out;
                out.bufferView = acc.HasMember("bufferView") ? acc["bufferView"].GetInt() : -1;
                out.byteOffset = acc.HasMember("byteOffset") ? acc["byteOffset"].GetUint() : 0;
                out.componentType = acc.HasMember("componentType") ? acc["componentType"].GetInt() : 0;
                out.count = acc.HasMember("count") ? acc["count"].GetUint() : 0;
                out.type = acc.HasMember("type") ? acc["type"].GetString() : "";
                out.normalized = acc.HasMember("normalized") ? acc["normalized"].GetBool() : false;
                accessors[i] = out;
            }
        }

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
                    if (pbr.HasMember("baseColorFactor")) {
                        ReadVec(pbr["baseColorFactor"], mat.baseColorFactor, 4);
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
                        const BufferView& norView = views[norA.bufferView];
                        const std::vector<uint8_t>& norBuf = buffers[norView.buffer];
                        LoadFloat3(norBuf, norView, norA, i, n);
                    }
                    if (uvAcc >= 0 && uvAcc < (int)accessors.size()) {
                        const Accessor& uvA = accessors[uvAcc];
                        const BufferView& uvView = views[uvA.bufferView];
                        const std::vector<uint8_t>& uvBuf = buffers[uvView.buffer];
                        LoadFloat2(uvBuf, uvView, uvA, i, uv);
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

    bool LoadStaticModel(const std::string& path,
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
            if (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size())) {
                const auto& mat = scene.materials[prim.materialIndex];
                for (auto& v : prim.mesh.vertices) {
                    v.color[0] *= mat.baseColorFactor[0];
                    v.color[1] *= mat.baseColorFactor[1];
                    v.color[2] *= mat.baseColorFactor[2];
                    v.color[3] *= mat.baseColorFactor[3];
                }
            }

            LoadedStaticMesh out;
            out.mesh = std::move(prim.mesh);
            // Final local transform = uniformScale * sourcePrimitiveTransform.
            Mul4x4(scaleM, prim.transform, out.localTransform);
            if (prim.materialIndex >= 0 && prim.materialIndex < static_cast<int>(scene.materials.size())) {
                const int texIndex = scene.materials[prim.materialIndex].baseColorTexture;
                if (texIndex >= 0 && texIndex < static_cast<int>(scene.textures.size())) {
                    const int imageIndex = scene.textures[texIndex].imageIndex;
                    if (imageIndex >= 0 && imageIndex < static_cast<int>(scene.images.size())) {
                        out.texturePath = scene.images[imageIndex].uri;
                    }
                }
            }
            outMeshes.push_back(std::move(out));
        }

        return !outMeshes.empty();
    }
}
