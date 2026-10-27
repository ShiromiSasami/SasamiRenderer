#pragma once

// A D3DCompile include handler for the DirectX 11 native shader path.
//
// D3D_COMPILE_STANDARD_FILE_INCLUDE resolves a quoted include only against the directory of the
// file that contains it. That covers the native path's own siblings, but not the shared shader
// headers under Shaders/, which include each other by shader-root-relative path:
// SSAO_Sampling.hlsli, for one, includes "Shared/Common/WorldPosReconstruction.hlsli". The D3D12
// path makes those resolvable by handing dxc `-I <shader root>` (see ShaderCompilationService);
// this handler gives the D3D11 compiler the same second chance, so the DX11 backend can compile
// the shared shader sources unmodified instead of carrying forked copies of them.

#include "Renderer/RHI/GraphicsDevice.h"

#if RHI_DIRECTX11

#include <d3dcommon.h>

#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SasamiEngine::Render
{
    class Dx11ShaderIncludeHandler final : public ID3DInclude
    {
    public:
        // `sourceDir` is the directory of the top-level shader being compiled, used as the base
        // for includes it contains itself. `shaderRoot` is <repo>/Shaders and may be empty, in
        // which case the handler behaves exactly like the standard one.
        Dx11ShaderIncludeHandler(std::filesystem::path sourceDir, std::filesystem::path shaderRoot)
            : m_sourceDir(std::move(sourceDir))
            , m_shaderRoot(std::move(shaderRoot))
        {
        }

        HRESULT __stdcall Open(D3D_INCLUDE_TYPE /*includeType*/,
                               LPCSTR pFileName,
                               LPCVOID pParentData,
                               LPCVOID* ppData,
                               UINT* pBytes) override
        {
            if (ppData == nullptr || pBytes == nullptr) {
                return E_POINTER;
            }
            *ppData = nullptr;
            *pBytes = 0;
            if (pFileName == nullptr) {
                return E_INVALIDARG;
            }

            const std::filesystem::path requested(pFileName);
            const std::filesystem::path resolved = Resolve(requested, pParentData);
            if (resolved.empty()) {
                return E_FAIL;
            }

            std::vector<char> contents;
            if (!ReadWholeFile(resolved, contents)) {
                return E_FAIL;
            }

            const UINT size = static_cast<UINT>(contents.size());
            // An empty header is legal; keep the buffer non-empty so data() stays a valid,
            // non-null key for Close() while the reported size stays truthful.
            if (contents.empty()) {
                contents.push_back('\0');
            }

            OpenFile entry;
            entry.directory = resolved.parent_path();
            entry.contents = std::move(contents);
            const void* key = entry.contents.data();
            m_openFiles.emplace(key, std::move(entry));

            *ppData = key;
            *pBytes = size;
            return S_OK;
        }

        HRESULT __stdcall Close(LPCVOID pData) override
        {
            m_openFiles.erase(pData);
            return S_OK;
        }

    private:
        struct OpenFile
        {
            std::filesystem::path directory;
            std::vector<char> contents;
        };

        std::filesystem::path Resolve(const std::filesystem::path& requested, LPCVOID parentData) const
        {
            std::error_code ec;
            if (requested.is_absolute()) {
                return (std::filesystem::exists(requested, ec) && !ec) ? requested : std::filesystem::path{};
            }

            // Standard semantics first: relative to whoever did the including.
            const std::filesystem::path base = BaseDirFor(parentData);
            const std::filesystem::path parentRelative = base / requested;
            if (std::filesystem::exists(parentRelative, ec) && !ec) {
                return parentRelative;
            }

            // Then the shader root, which is what the D3D12 compiler's -I resolves.
            if (!m_shaderRoot.empty()) {
                ec.clear();
                const std::filesystem::path rootRelative = m_shaderRoot / requested;
                if (std::filesystem::exists(rootRelative, ec) && !ec) {
                    return rootRelative;
                }
            }
            return {};
        }

        std::filesystem::path BaseDirFor(LPCVOID parentData) const
        {
            if (parentData != nullptr) {
                const auto it = m_openFiles.find(parentData);
                if (it != m_openFiles.end()) {
                    return it->second.directory;
                }
            }
            return m_sourceDir;
        }

        static bool ReadWholeFile(const std::filesystem::path& path, std::vector<char>& out)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                return false;
            }
            file.seekg(0, std::ios::end);
            const std::streamoff size = file.tellg();
            if (size < 0) {
                return false;
            }
            file.seekg(0, std::ios::beg);
            out.resize(static_cast<size_t>(size));
            if (size > 0 && !file.read(out.data(), size)) {
                return false;
            }
            return true;
        }

        std::filesystem::path m_sourceDir;
        std::filesystem::path m_shaderRoot;
        // Keyed by the pointer handed back from Open(); node-based so the stored buffers -- and
        // therefore those keys -- stay valid for as long as the compiler holds them.
        std::unordered_map<const void*, OpenFile> m_openFiles;
    };
}

namespace SasamiEngine { using namespace Render; }

#endif // RHI_DIRECTX11
