#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

namespace SasamiEngine::App
{
    // DDS decoding for texture sets that ship as block-compressed data (e.g. Bistro:
    // BC1 base color, BC3 specular, BC5 normals). Decodes the top mip to RGBA8 so the
    // result feeds the same CpuTextureRgba8 path as WIC-loaded PNG/JPG textures.
    //
    // Pure CPU + file IO, thread-safe: callable from JobSystem workers.
    namespace DdsTextureLoader
    {
        // Storage format of a parsed DDS file. BC2 and BC6H decode but have no RhiFormat.
        enum class DdsFormat
        {
            None,
            BC1,
            BC2,
            BC3,
            BC4,
            BC5,
            BC6HUnsigned,
            BC6HSigned,
            BC7,
            Rgba8,
            Bgra8,
        };

        // One stored mip level. offset is from the start of the file; byteSize is the stored
        // (compressed or raw) size, rowPitch the bytes of one row of 4x4 blocks (BC) or texels.
        struct DdsMipLevel
        {
            size_t offset = 0;
            size_t byteSize = 0;
            uint32_t rowPitch = 0;
            uint32_t width = 0;
            uint32_t height = 0;
        };

        struct DdsImageInfo
        {
            DdsFormat format = DdsFormat::None;
            bool srgb = false; // only DX10 headers carry it; legacy FourCC files report false
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t pitchOrLinearSize = 0;
            // Every level the header announces. A level whose bytes run past the end of the
            // file is still listed (offset + byteSize > file size); check before reading it.
            std::vector<DdsMipLevel> mips;
        };

        bool IsBlockCompressed(DdsFormat format);

        // Parses the header of a DDS file held in memory. Same restrictions and logging as
        // LoadRgba8FromDds (no cubemaps, volumes or unknown formats).
        bool ParseDds(const uint8_t* data, size_t size, DdsImageInfo& outInfo, const std::wstring& pathForLog);

        // Decodes one mip of a parsed file to tightly packed RGBA8.
        bool DecodeDdsMipToRgba8(const uint8_t* data, size_t size, const DdsImageInfo& info, uint32_t mip,
                                 std::vector<uint8_t>& outPixels, const std::wstring& pathForLog);

        // Returns false (and logs) for unsupported formats such as BC6H (HDR) or
        // cubemap/volume DDS files.
        bool LoadRgba8FromDds(const std::wstring& path,
                              std::vector<uint8_t>& outPixels,
                              UINT& outWidth,
                              UINT& outHeight);
    }
}
namespace SasamiEngine { using namespace App; }
