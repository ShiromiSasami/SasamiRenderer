#pragma once

#include <cstdint>
#include <string>

namespace SasamiEngine::Render
{
    // Writes 8-bit pixels to a PNG file through WIC.
    //
    // Split out of TextureCapture so the encoding step has no D3D12 in it: the non-D3D12 backends
    // read their own back buffer and need the same encoder, and duplicating the WIC dance per
    // backend is how the copies drift apart.
    //
    // `bgra` is tightly packed, `width * height * 4` bytes, top row first. Creates parent
    // directories as needed. Initializes COM on the calling thread if it is not already.
    //
    // The byte order is **BGRA, not RGBA**: the encoder is GUID_WICPixelFormat32bppBGRA and the
    // bytes go to WIC untouched. This was called SavePngRgba8 until 2026-09-12, and the wrong name
    // cost a real bug -- the OpenGL capture handed it true RGBA and every screenshot came out with
    // red and blue swapped. A caller whose source is RGBA has to swizzle first: DirectX 11 does
    // that when its back buffer is R8G8B8A8, and TextureCapture writes B,G,R,A directly.
    bool SavePngBgra8(const std::wstring& path, const uint8_t* bgra, uint32_t width, uint32_t height);
}

namespace SasamiEngine
{
    using namespace Render;
}
