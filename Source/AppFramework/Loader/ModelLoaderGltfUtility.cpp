#include "ModelLoaderGltfUtility.h"

#include <fstream>

namespace SasamiRenderer
{
    namespace ModelLoaderGltfUtility
    {
        bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out)
        {
            std::ifstream ifs(path, std::ios::binary | std::ios::ate);
            if (!ifs.is_open()) return false;
            std::streamsize size = ifs.tellg();
            if (size <= 0) return false;
            ifs.seekg(0, std::ios::beg);
            out.resize(static_cast<size_t>(size));
            return static_cast<bool>(ifs.read(reinterpret_cast<char*>(out.data()), size));
        }

        void ParseBuffersViewsAccessors(const rapidjson::Document& doc,
                                         const std::filesystem::path& basePath,
                                         bool isGlb,
                                         const std::vector<uint8_t>& glbBinChunk,
                                         std::vector<std::vector<uint8_t>>& outBuffers,
                                         std::vector<BufferView>& outViews,
                                         std::vector<Accessor>& outAccessors)
        {
            outBuffers.clear();
            outViews.clear();
            outAccessors.clear();

            if (doc.HasMember("buffers") && doc["buffers"].IsArray()) {
                const auto& bufs = doc["buffers"];
                outBuffers.resize(bufs.Size());
                for (rapidjson::SizeType i = 0; i < bufs.Size(); ++i) {
                    const auto& b = bufs[i];
                    if (!b.IsObject()) continue;
                    if (b.HasMember("uri")) {
                        std::string uri = b["uri"].GetString();
                        ReadFileBytes(basePath / uri, outBuffers[i]);
                    } else if (isGlb) {
                        // A buffer with no "uri" refers to the embedded GLB binary chunk.
                        outBuffers[i] = glbBinChunk;
                    }
                }
            }

            if (doc.HasMember("bufferViews") && doc["bufferViews"].IsArray()) {
                const auto& v = doc["bufferViews"];
                outViews.resize(v.Size());
                for (rapidjson::SizeType i = 0; i < v.Size(); ++i) {
                    const auto& bv = v[i];
                    if (!bv.IsObject()) continue;
                    BufferView view;
                    view.buffer = bv.HasMember("buffer") ? bv["buffer"].GetInt() : -1;
                    view.byteOffset = bv.HasMember("byteOffset") ? bv["byteOffset"].GetUint() : 0;
                    view.byteLength = bv.HasMember("byteLength") ? bv["byteLength"].GetUint() : 0;
                    view.byteStride = bv.HasMember("byteStride") ? bv["byteStride"].GetUint() : 0;
                    outViews[i] = view;
                }
            }

            if (doc.HasMember("accessors") && doc["accessors"].IsArray()) {
                const auto& a = doc["accessors"];
                outAccessors.resize(a.Size());
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
                    outAccessors[i] = out;
                }
            }
        }

        int NumComponents(const std::string& type)
        {
            if (type == "SCALAR") return 1;
            if (type == "VEC2") return 2;
            if (type == "VEC3") return 3;
            if (type == "VEC4") return 4;
            if (type == "MAT4") return 16;
            return 0;
        }

        size_t ComponentSize(int componentType)
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

        void Identity(float out[16])
        {
            for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        }

        void BuildTranslation(const float t[3], float out[16])
        {
            Identity(out);
            // Row-major affine translation in last row (row-vector convention).
            out[12] = t[0];
            out[13] = t[1];
            out[14] = t[2];
        }

        void BuildScale(const float s[3], float out[16])
        {
            Identity(out);
            // Non-uniform scale on matrix diagonal.
            out[0] = s[0];
            out[5] = s[1];
            out[10] = s[2];
        }

        void BuildRotationFromQuat(const float q[4], float out[16])
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

        bool ReadVec(const rapidjson::Value& arr, float* out, int n)
        {
            if (!arr.IsArray() || (int)arr.Size() < n) return false;
            for (int i = 0; i < n; ++i) out[i] = arr[i].GetFloat();
            return true;
        }

        const uint8_t* GetAccessorPtr(const std::vector<uint8_t>& buffer,
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

        bool LoadFloat3(const std::vector<uint8_t>& buffer, const BufferView& view, const Accessor& acc,
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

        bool LoadFloat2(const std::vector<uint8_t>& buffer, const BufferView& view, const Accessor& acc,
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

        bool LoadIndices(const std::vector<uint8_t>& buffer, const BufferView& view, const Accessor& acc,
                          std::vector<uint32_t>& out)
        {
            const int comps = NumComponents(acc.type);
            if (comps != 1) return false;
            const size_t compSize = ComponentSize(acc.componentType);
            if (compSize == 0) return false;
            const size_t stride = view.byteStride == 0 ? compSize : view.byteStride;
            out.resize(acc.count);
            for (size_t i = 0; i < acc.count; ++i) {
                const size_t offset = view.byteOffset + acc.byteOffset + i * stride;
                if (offset + compSize > buffer.size()) return false;
                const uint8_t* ptr = buffer.data() + offset;
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
    }
}
