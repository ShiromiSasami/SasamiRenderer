#pragma once

// RHI-to-D3D11 conversions shared by the device and its command encoder. They lived in the
// anonymous namespace of Dx11GraphicsDevice.cpp, which is what kept the encoder from moving out.

#include "Renderer/Backends/DirectX11/Dx11GraphicsDevice.h"
#include "Renderer/Backends/DirectX11/Dx11ShaderInclude.h"

#if RHI_DIRECTX11
#include "Foundation/Tools/DebugOutput.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <d3dcompiler.h>
#endif

#if RHI_DIRECTX11

namespace SasamiEngine::Render
{
    inline DXGI_FORMAT ToDx11Format(RhiFormat format)
    {
        switch (format) {
        case RhiFormat::R8UNorm: return DXGI_FORMAT_R8_UNORM;
        case RhiFormat::R8G8B8A8UNorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case RhiFormat::R8G8B8A8UNormSrgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case RhiFormat::B8G8R8A8UNorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case RhiFormat::R16G16B16A16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case RhiFormat::R32G32B32Float: return DXGI_FORMAT_R32G32B32_FLOAT;
        case RhiFormat::R32G32Float: return DXGI_FORMAT_R32G32_FLOAT;
        case RhiFormat::R16Float: return DXGI_FORMAT_R16_FLOAT;
        case RhiFormat::R16Typeless: return DXGI_FORMAT_R16_TYPELESS;
        case RhiFormat::R16UNorm: return DXGI_FORMAT_R16_UNORM;
        case RhiFormat::R32Float: return DXGI_FORMAT_R32_FLOAT;
        case RhiFormat::R32UInt: return DXGI_FORMAT_R32_UINT;
        case RhiFormat::R32Typeless: return DXGI_FORMAT_R32_TYPELESS;
        case RhiFormat::D16UNorm: return DXGI_FORMAT_D16_UNORM;
        case RhiFormat::D32Float: return DXGI_FORMAT_D32_FLOAT;
        case RhiFormat::D24UNormS8UInt: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }
    inline D3D11_PRIMITIVE_TOPOLOGY ToDx11Topology(RhiPrimitiveTopology topology)
    {
        switch (topology) {
        case RhiPrimitiveTopology::TriangleStrip: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case RhiPrimitiveTopology::LineList: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        case RhiPrimitiveTopology::LineStrip: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case RhiPrimitiveTopology::PointList: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        case RhiPrimitiveTopology::PatchList: return D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
        case RhiPrimitiveTopology::TriangleList:
        default: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        }
    }
    inline D3D11_VIEWPORT ToDx11Viewport(const RhiViewport& viewport)
    {
        D3D11_VIEWPORT out{};
        out.TopLeftX = viewport.x;
        out.TopLeftY = viewport.y;
        out.Width = viewport.width;
        out.Height = viewport.height;
        out.MinDepth = viewport.minDepth;
        out.MaxDepth = viewport.maxDepth;
        return out;
    }
    inline D3D11_RECT ToDx11Rect(const RhiRect& rect)
    {
        return D3D11_RECT{ rect.left, rect.top, rect.right, rect.bottom };
    }
    inline std::filesystem::path GetExecutableDir()
    {
        wchar_t exePath[MAX_PATH] = {};
        const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len == 0 || len == MAX_PATH) {
            return std::filesystem::current_path();
        }
        return std::filesystem::path(exePath).parent_path();
    }

    inline std::filesystem::path FindShaderFile(const wchar_t* relativePath)
    {
        std::error_code ec;
        std::array<std::filesystem::path, 2> starts = {
            std::filesystem::current_path(ec),
            GetExecutableDir(),
        };

        for (const auto& start : starts) {
            std::filesystem::path dir = std::filesystem::absolute(start, ec);
            if (ec) {
                dir = start;
                ec.clear();
            }

            for (;;) {
                // The canonical shader root is <repo>/Shaders -- the same one
                // ShaderCompilationService resolves to. This used to search only the legacy
                // <repo>/Source/Renderer/Shaders location, which does not exist in this repo,
                // so every file-based DX11 shader load silently failed to find its source.
                const std::filesystem::path candidate = dir / L"Shaders" / relativePath;
                if (std::filesystem::exists(candidate, ec)) {
                    return candidate;
                }

                const std::filesystem::path legacyCandidate =
                    dir / L"Source" / L"Renderer" / L"Shaders" / relativePath;
                if (std::filesystem::exists(legacyCandidate, ec)) {
                    return legacyCandidate;
                }

                const std::filesystem::path parent = dir.parent_path();
                if (parent.empty() || parent == dir) {
                    break;
                }
                dir = parent;
            }
        }

        return {};
    }

    // The shader root (<repo>/Shaders) that FindShaderFile resolved `relativePath` against,
    // recovered by peeling that many components back off the result. Derived rather than searched
    // a second time so the two can never disagree about which root was used.
    inline std::filesystem::path ShaderRootOf(const std::filesystem::path& resolvedPath,
                                              const wchar_t* relativePath)
    {
        std::filesystem::path root = resolvedPath;
        const std::filesystem::path relative(relativePath);
        for (auto it = relative.begin(); it != relative.end() && !root.empty(); ++it) {
            root = root.parent_path();
        }
        return root;
    }

    // The constant-buffer layouts and the shader compile helper, shared by the device and by
    // every path split out of it (ray march, sky, skinned mesh).
    struct Dx11RayMarchConstants
    {
        float invViewProjection[16];
        float cameraPos[3];
        float sceneTimeSec;
        float sunDir[3];
        float sunIntensity;
        float sunColor[3];
        float cloudCover;
        float renderWidth;
        float renderHeight;
        float fluidMode;
        float cloudDensity;
        float extra0[4];
        float extra1[4];
        float extra2[4];
    };

    struct Dx11MeshConstants
    {
        float viewProjection[16];
        float model[16];
        float baseColor[4];
        float emissiveRoughness[4];
        float lightDirIntensity[4];
        float lightColor[4];
        float cameraPosMetallic[4];
        float featureFlags[4];
        float featureFlags2[4];
        float lightViewProjection[16];
        // x = depth bias added to the comparison depth, y = normal-bias factor,
        // z = world size of one shadow texel, w = shadow map resolution (0 = no shadow map).
        float shadowParams[4];
        // x = exposure (RenderSettings::exposure).
        float exposureParams[4];
    };

    inline bool CompileDx11Shader(const wchar_t* relativePath,
                           const char* entryPoint,
                           const char* profile,
                           Microsoft::WRL::ComPtr<ID3DBlob>& outBlob)
    {
        const std::filesystem::path shaderPath = FindShaderFile(relativePath);
        if (shaderPath.empty()) {
            DebugLog("Dx11GraphicsDevice: shader file was not found.\n");
            return false;
        }

        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        // Not D3D_COMPILE_STANDARD_FILE_INCLUDE: that one resolves a quoted include only against
        // the including file's own directory, which cannot reach the shader-root-relative includes
        // the shared headers under Shaders/ use. See Dx11ShaderInclude.h.
        Dx11ShaderIncludeHandler includeHandler(shaderPath.parent_path(),
                                                ShaderRootOf(shaderPath, relativePath));

        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompileFromFile(shaderPath.c_str(),
                                              nullptr,
                                              &includeHandler,
                                              entryPoint,
                                              profile,
                                              flags,
                                              0,
                                              outBlob.GetAddressOf(),
                                              errors.GetAddressOf());
        if (FAILED(hr)) {
            DebugLog("Dx11GraphicsDevice: shader compilation failed.\n");
            if (errors && errors->GetBufferPointer()) {
                DebugLog(static_cast<const char*>(errors->GetBufferPointer()));
                DebugLog("\n");
            }
            return false;
        }
        return true;
    }

}

#endif // RHI_DIRECTX11
