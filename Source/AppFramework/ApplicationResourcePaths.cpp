#include "ApplicationResourcePaths.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <system_error>
#include <vector>

namespace SasamiRenderer
{
    namespace
    {
        std::filesystem::path GetExecutableDir()
        {
            wchar_t exePath[MAX_PATH] = {};
            const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (len == 0 || len == MAX_PATH) {
                return std::filesystem::current_path();
            }
            return std::filesystem::path(exePath).parent_path();
        }

        std::filesystem::path FindProjectRootWithAssets(const std::filesystem::path& startDir)
        {
            std::error_code ec;
            std::filesystem::path dir = std::filesystem::absolute(startDir, ec);
            if (ec) {
                dir = startDir;
            }

            for (;;) {
                const std::filesystem::path assetsDir = dir / L"Assets";
                if (std::filesystem::exists(assetsDir, ec) &&
                    std::filesystem::is_directory(assetsDir, ec)) {
                    return dir;
                }

                const std::filesystem::path parent = dir.parent_path();
                if (parent.empty() || parent == dir) {
                    break;
                }
                dir = parent;
            }

            return {};
        }

        bool IsHdrExtension(const std::filesystem::path& path)
        {
            std::wstring ext = path.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) {
                return static_cast<wchar_t>(std::towlower(c));
            });
            return ext == L".hdr";
        }

        std::filesystem::path ResolveAssetPath(const std::string& relativeOrAbsolutePath)
        {
            const std::filesystem::path requestedPath(relativeOrAbsolutePath);
            if (requestedPath.is_absolute()) {
                return requestedPath;
            }

            const std::filesystem::path projectRoot = FindProjectRootWithAssets(GetExecutableDir());
            if (!projectRoot.empty()) {
                return projectRoot / L"Assets" / requestedPath;
            }

            return std::filesystem::current_path() / L"Assets" / requestedPath;
        }
    }

    std::string ApplicationResourcePaths::ResolveAssetPathString(const std::string& relativeOrAbsolutePath)
    {
        return ResolveAssetPath(relativeOrAbsolutePath).string();
    }

    std::wstring ApplicationResourcePaths::ResolveAssetPathWide(const std::string& relativeOrAbsolutePath)
    {
        return ResolveAssetPath(relativeOrAbsolutePath).wstring();
    }

    std::wstring ApplicationResourcePaths::ResolveWindowIconPath()
    {
        std::error_code ec;
        const std::filesystem::path projectRoot = FindProjectRootWithAssets(GetExecutableDir());
        if (!projectRoot.empty()) {
            const std::filesystem::path projectIcon = projectRoot / L"Assets" / L"SasamiIcon.ico";
            if (std::filesystem::exists(projectIcon, ec)) {
                return projectIcon.wstring();
            }
        }

        const std::filesystem::path cwdIcon = std::filesystem::current_path() / L"Assets" / L"SasamiIcon.ico";
        if (std::filesystem::exists(cwdIcon, ec)) {
            return cwdIcon.wstring();
        }

        return {};
    }

    bool ApplicationResourcePaths::ResolveCubemapFacePaths(const std::string& directoryPath,
                                                           std::array<std::wstring, 6>& outPaths)
    {
        static const std::array<std::vector<std::wstring>, 6> kFaceNameCandidates = {{
            { L"px", L"posx", L"positive_x", L"right", L"xpos", L"+x" },
            { L"nx", L"negx", L"negative_x", L"left",  L"xneg", L"-x" },
            { L"py", L"posy", L"positive_y", L"up",    L"ypos", L"+y" },
            { L"ny", L"negy", L"negative_y", L"down",  L"yneg", L"-y" },
            { L"pz", L"posz", L"positive_z", L"front", L"zpos", L"+z" },
            { L"nz", L"negz", L"negative_z", L"back",  L"zneg", L"-z" },
        }};
        static const std::array<std::wstring, 8> kExtensions = {
            L".png", L".jpg", L".jpeg", L".bmp", L".tif", L".tiff", L".wdp", L".hdp"
        };

        const std::filesystem::path directory(directoryPath);
        std::error_code ec;
        if (!std::filesystem::is_directory(directory, ec)) {
            return false;
        }

        for (size_t face = 0; face < kFaceNameCandidates.size(); ++face) {
            bool resolved = false;
            for (const std::wstring& name : kFaceNameCandidates[face]) {
                const std::filesystem::path exact = directory / name;
                if (std::filesystem::is_regular_file(exact, ec)) {
                    outPaths[face] = exact.wstring();
                    resolved = true;
                    break;
                }

                for (const std::wstring& ext : kExtensions) {
                    const std::filesystem::path candidate = directory / (name + ext);
                    if (std::filesystem::is_regular_file(candidate, ec)) {
                        outPaths[face] = candidate.wstring();
                        resolved = true;
                        break;
                    }
                }

                if (resolved) {
                    break;
                }
            }

            if (!resolved) {
                return false;
            }
        }

        return true;
    }

    bool ApplicationResourcePaths::ResolveEquirectSkyboxFile(const std::string& resourcePath,
                                                             std::wstring& outPath,
                                                             bool& outIsHdr)
    {
        const std::filesystem::path configuredPath(resourcePath);
        std::error_code ec;
        if (!std::filesystem::exists(configuredPath, ec) ||
            !std::filesystem::is_regular_file(configuredPath, ec)) {
            return false;
        }

        outPath = configuredPath.wstring();
        outIsHdr = IsHdrExtension(configuredPath);
        return true;
    }
}
