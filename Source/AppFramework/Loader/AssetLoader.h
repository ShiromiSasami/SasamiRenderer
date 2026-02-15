#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <wtypes.h>

namespace SasamiRenderer
{
    class AssetLoader
    {
    public:
        static bool LoadRgba8ViaWIC(const std::wstring& path,
                                    std::vector<uint8_t>& pixels,
                                    UINT& width,
                                    UINT& height);

        static bool LoadCubemapFacesViaWIC(const std::array<std::wstring, 6>& paths,
                                           std::vector<std::vector<uint8_t>>& facePixels,
                                           UINT& width,
                                           UINT& height);

        static bool LoadRadianceHdr(const std::wstring& path,
                                    std::vector<float>& outRgb,
                                    UINT& outWidth,
                                    UINT& outHeight);
    };
}
