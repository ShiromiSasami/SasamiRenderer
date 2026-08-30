#include "ModelLoaderGlb.h"
#include <cstring>

namespace SasamiRenderer
{
    namespace {
        constexpr uint32_t kGlbMagic     = 0x46546C67u; // "glTF"
        constexpr uint32_t kChunkTypeJson= 0x4E4F534Au; // "JSON"
        constexpr uint32_t kChunkTypeBin = 0x004E4942u; // "BIN\0"

        // glTF binary header/chunk fields are little-endian; x86/x64 hosts match natively.
        uint32_t ReadU32(const uint8_t* p)
        {
            uint32_t v;
            std::memcpy(&v, p, sizeof(uint32_t));
            return v;
        }
    }

    bool ParseGlbContainer(const std::vector<uint8_t>& fileBytes, std::string& outJson, std::vector<uint8_t>& outBin)
    {
        outJson.clear();
        outBin.clear();

        if (fileBytes.size() < 12) return false;
        const uint8_t* data = fileBytes.data();

        const uint32_t magic   = ReadU32(data + 0);
        const uint32_t version = ReadU32(data + 4);
        const uint32_t length  = ReadU32(data + 8);
        if (magic != kGlbMagic || version != 2) return false;
        if (length > fileBytes.size()) return false;

        size_t offset = 12;
        bool first = true;

        while (offset + 8 <= length)
        {
            const uint32_t chunkLength     = ReadU32(data + offset);
            const uint32_t chunkType       = ReadU32(data + offset + 4);
            const size_t   chunkDataOffset = offset + 8;
            if (chunkDataOffset + chunkLength > fileBytes.size()) return false;

            if (first)
            {
                // glTF 2.0 spec: the JSON chunk must be the first chunk in the container.
                if (chunkType != kChunkTypeJson) return false;
                outJson.assign(reinterpret_cast<const char*>(data + chunkDataOffset), chunkLength);
                first = false;
            }
            else if (chunkType == kChunkTypeBin)
            {
                outBin.assign(data + chunkDataOffset, data + chunkDataOffset + chunkLength);
            }
            // Other chunk types are reserved for future extensions and are ignored.

            offset = chunkDataOffset + chunkLength;
        }

        return !first; // true only if the JSON chunk was found and consumed
    }
}
