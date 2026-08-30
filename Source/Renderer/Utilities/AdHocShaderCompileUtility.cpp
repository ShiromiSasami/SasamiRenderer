#include "Renderer/Utilities/AdHocShaderCompileUtility.h"

#include "Foundation/Tools/DebugOutput.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <windows.h>
#include <dxcapi.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace SasamiRenderer
{
    namespace AdHocShaderCompileUtility
    {
        HRESULT CreateDxcInstance(REFCLSID clsid, REFIID iid, LPVOID* out)
        {
            static HMODULE mod = []() -> HMODULE {
                const std::filesystem::path bundledDllPath =
                    GetExeDir() / L"Tools" / L"DXC" / L"bin" / L"x64" / L"dxcompiler.dll";
                if (HMODULE module = LoadLibraryW(bundledDllPath.c_str())) {
                    return module;
                }
                return LoadLibraryW(L"dxcompiler.dll");
            }();
            static auto fn = mod
                ? reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>(
                      GetProcAddress(mod, "DxcCreateInstance"))
                : nullptr;
            return fn ? fn(clsid, iid, out) : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        }

        std::filesystem::path GetExeDir()
        {
            wchar_t buf[MAX_PATH]{};
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            return std::filesystem::path(buf).parent_path();
        }

        std::filesystem::path FindProjectRoot(std::filesystem::path dir)
        {
            for (int depth = 0; depth < 16; ++depth) {
                if ((std::filesystem::exists(dir / "Shaders") && std::filesystem::exists(dir / "Source")) ||
                    std::filesystem::exists(dir / "Source" / "Renderer" / "Shaders")) return dir;
                auto p = dir.parent_path();
                if (p.empty() || p == dir) break;
                dir = p;
            }
            return {};
        }

        std::filesystem::path GetShaderRoot()
        {
            static const std::filesystem::path root = []() {
                auto pr = FindProjectRoot(GetExeDir());
                if (pr.empty()) {
                    return std::filesystem::path(L"Shaders");
                }
                const std::filesystem::path shaderRoot = pr / L"Shaders";
                return std::filesystem::exists(shaderRoot)
                    ? shaderRoot
                    : pr / L"Source" / L"Renderer" / L"Shaders";
            }();
            return root;
        }

        bool CompileShader(const char* logTag,
                           const wchar_t* relPath,
                           const char* entry,
                           const char* profile,
                           std::vector<uint8_t>& outBytecode)
        {
            const std::filesystem::path srcPath = GetShaderRoot() / relPath;
            const std::filesystem::path incPath = GetShaderRoot();

            // IDxcCompiler3/IDxcUtils instances are not documented thread-safe, so each
            // thread keeps its own; this also avoids one DxcCreateInstance call per shader.
            thread_local ComPtr<IDxcUtils>     utils;
            thread_local ComPtr<IDxcCompiler3> compiler;
            if (!utils && FAILED(CreateDxcInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))))       return false;
            if (!compiler && FAILED(CreateDxcInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) return false;

            ComPtr<IDxcBlobEncoding> src;
            if (FAILED(utils->LoadFile(srcPath.c_str(), nullptr, &src))) {
                OutputDebugStringA((std::string(logTag) + ": failed to load shader: " + srcPath.string() + "\n").c_str());
                return false;
            }

            ComPtr<IDxcIncludeHandler> incHandler;
            utils->CreateDefaultIncludeHandler(&incHandler);

            const std::wstring srcW = srcPath.native();
            const std::wstring incW = incPath.native();
            auto toW = [](const char* s) {
                std::wstring w; if (!s) return w; for (const char* p = s; *p; ++p) w += (wchar_t)*p; return w;
            };
            const std::wstring entW  = toW(entry);
            const std::wstring profW = toW(profile);

            std::vector<LPCWSTR> args{ srcW.c_str() };
            if (entry && entry[0] != '\0') {
                args.push_back(L"-E");
                args.push_back(entW.c_str());
            }
            args.insert(args.end(), {
                L"-T", profW.c_str(),
                L"-I", incW.c_str(),
                L"-HV", L"2021",
                L"-WX",
            });
#if defined(_DEBUG)
            args.push_back(L"-Zi"); args.push_back(L"-Od");
#else
            args.push_back(L"-O3");
#endif

            DxcBuffer buf{ src->GetBufferPointer(), src->GetBufferSize(), DXC_CP_ACP };
            ComPtr<IDxcResult> result;
            if (FAILED(compiler->Compile(&buf, args.data(), (UINT32)args.size(), incHandler.Get(), IID_PPV_ARGS(&result))))
                return false;

            ComPtr<IDxcBlobUtf8> errors;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
                errors && errors->GetStringLength() > 0) {
                OutputDebugStringA(errors->GetStringPointer());
            }

            HRESULT hr = S_OK;
            result->GetStatus(&hr);
            if (FAILED(hr)) {
                OutputDebugStringA((std::string(logTag) + ": shader compilation failed: " + srcPath.string() + "\n").c_str());
                return false;
            }

            ComPtr<IDxcBlob> code;
            result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&code), nullptr);
            if (!code) return false;

            outBytecode.resize(code->GetBufferSize());
            std::memcpy(outBytecode.data(), code->GetBufferPointer(), code->GetBufferSize());
            return true;
        }
    }
}
