#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>
#include <windows.h>

namespace SasamiRenderer
{
    namespace AdHocShaderCompileUtility
    {
        // Minimal DXC-based shader compile helper shared by render passes that compile
        // their own shaders ad-hoc (outside ShaderCompilationService/DxrRayTracerUtility).
        // logTag identifies the calling pass in OutputDebugStringA diagnostics.
        // entry may be null or empty for library targets (lib_6_x profiles), which take
        // no entry point; in that case the "-E" argument is omitted from the DXC invocation.
        bool CompileShader(const char* logTag,
                           const wchar_t* relPath,
                           const char* entry,
                           const char* profile,
                           std::vector<uint8_t>& outBytecode);

        // Lower-level building blocks, exposed for callers that need their own
        // DXC compile call with a different profile/target-string derivation
        // than CompileShader (e.g. compute-only paths using ShaderCompilationService
        // for target-string resolution).
        HRESULT CreateDxcInstance(REFCLSID clsid, REFIID iid, LPVOID* out);
        std::filesystem::path GetExeDir();
        std::filesystem::path FindProjectRoot(std::filesystem::path dir);
        std::filesystem::path GetShaderRoot();
    }
}
