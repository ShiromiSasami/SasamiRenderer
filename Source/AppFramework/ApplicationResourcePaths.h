#pragma once

#include <array>
#include <string>

namespace SasamiRenderer
{
    class ApplicationResourcePaths final
    {
    public:
        ApplicationResourcePaths() = delete;

        static std::string ResolveAssetPathString(const std::string& relativeOrAbsolutePath);
        static std::wstring ResolveAssetPathWide(const std::string& relativeOrAbsolutePath);
        static std::wstring ResolveWindowIconPath();
        static bool ResolveCubemapFacePaths(const std::string& directoryPath,
                                            std::array<std::wstring, 6>& outPaths);
        static bool ResolveEquirectSkyboxFile(const std::string& resourcePath,
                                              std::wstring& outPath,
                                              bool& outIsHdr);
    };
}
