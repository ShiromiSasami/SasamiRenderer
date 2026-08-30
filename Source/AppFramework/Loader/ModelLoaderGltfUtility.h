#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <rapidjson/document.h>

namespace SasamiRenderer
{
    namespace ModelLoaderGltfUtility
    {
        struct BufferView
        {
            int buffer = -1;
            size_t byteOffset = 0;
            size_t byteLength = 0;
            size_t byteStride = 0;
        };

        struct Accessor
        {
            int bufferView = -1;
            size_t byteOffset = 0;
            int componentType = 0;
            size_t count = 0;
            std::string type;
            bool normalized = false;
        };

        bool ReadFileBytes(const std::filesystem::path& path, std::vector<uint8_t>& out);

        // Parses the glTF "buffers", "bufferViews", and "accessors" arrays from `doc`.
        // `glbBinChunk` supplies the data for buffers with no "uri" when `isGlb` is true.
        void ParseBuffersViewsAccessors(const rapidjson::Document& doc,
                                         const std::filesystem::path& basePath,
                                         bool isGlb,
                                         const std::vector<uint8_t>& glbBinChunk,
                                         std::vector<std::vector<uint8_t>>& outBuffers,
                                         std::vector<BufferView>& outViews,
                                         std::vector<Accessor>& outAccessors);

        int NumComponents(const std::string& type);
        size_t ComponentSize(int componentType);

        void Identity(float out[16]);
        void BuildTranslation(const float t[3], float out[16]);
        void BuildScale(const float s[3], float out[16]);
        void BuildRotationFromQuat(const float q[4], float out[16]);

        bool ReadVec(const rapidjson::Value& arr, float* out, int n);

        const uint8_t* GetAccessorPtr(const std::vector<uint8_t>& buffer,
                                       const BufferView& view,
                                       const Accessor& acc,
                                       size_t index);

        bool LoadFloat3(const std::vector<uint8_t>& buffer, const BufferView& view, const Accessor& acc,
                         size_t index, float out[3]);
        bool LoadFloat2(const std::vector<uint8_t>& buffer, const BufferView& view, const Accessor& acc,
                         size_t index, float out[2]);
        bool LoadIndices(const std::vector<uint8_t>& buffer, const BufferView& view, const Accessor& acc,
                          std::vector<uint32_t>& out);
    }
}
