#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

namespace SasamiRenderer
{
    // DDS decoding for texture sets that ship as block-compressed data (e.g. Bistro:
    // BC1 base color, BC3 specular, BC5 normals). Decodes the top mip to RGBA8 so the
    // result feeds the same CpuTextureRgba8 path as WIC-loaded PNG/JPG textures.
    //
    // Pure CPU + file IO, thread-safe: callable from JobSystem workers.
    namespace DdsTextureLoader
    {
        // Returns false (and logs) for unsupported formats such as BC6H (HDR) or
        // cubemap/volume DDS files.
        bool LoadRgba8FromDds(const std::wstring& path,
                              std::vector<uint8_t>& outPixels,
                              UINT& outWidth,
                              UINT& outHeight);
    }
}
