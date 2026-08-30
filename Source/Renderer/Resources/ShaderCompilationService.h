#pragma once
#include <cstdint>
#include <d3dcompiler.h>
#include <filesystem>
#include <string>
#include <vector>
#include <wrl/client.h>

struct ID3D12Device;

namespace SasamiRenderer::ShaderCompilationService
{
    std::string GetConfiguredShaderModel();
    std::string ResolveEffectiveShaderModel(ID3D12Device* device, const std::string& requestedModel);
    std::wstring ResolveEffectiveComputeShaderTarget(ID3D12Device* device, const std::string& requestedModel);
    void ResolveEffectiveVsPsTargets(ID3D12Device* device, const std::string& requestedModel,
                                     std::string& outVsTarget, std::string& outPsTarget);

    std::filesystem::path GetShaderSourceRoot();
    std::filesystem::path ResolveShaderPath(const wchar_t* relativeFileName);
    std::filesystem::path ResolveCompiledShaderPath(const std::filesystem::path& sourcePath,
                                                    const char* entry,
                                                    const char* target);

    bool IsCompiledShaderUpToDate(const std::filesystem::path& compiledPath,
                                  const std::filesystem::path& sourcePath);

    bool CompileShader(const std::filesystem::path& sourcePath,
                       const char* entry,
                       const char* target,
                       Microsoft::WRL::ComPtr<ID3DBlob>& outShader,
                       bool spirv = false);

    void LogShaderResolveMessage(const std::filesystem::path& sourcePath,
                                 const char* entry,
                                 const char* target,
                                 const char* action,
                                 const std::filesystem::path& compiledPath);

    // DXC-bytecode variant of the disk cache above, for the ad-hoc compile paths
    // (SWRT/GI/Fluid/Particles/DXR library) that bypass RenderPipelineStateCache.
    // entry may be empty for library targets (lib_6_x), which take no entry point.
    bool GetOrCompileShaderBytecodeDxc(const char* logTag,
                                       const wchar_t* relPath,
                                       const char* entry,
                                       const char* profile,
                                       std::vector<uint8_t>& outBytecode);
}
