#pragma once
#include <d3dcompiler.h>
#include <filesystem>
#include <string>
#include <wrl/client.h>

struct ID3D12Device;

namespace SasamiRenderer::ShaderCompilationService
{
    std::string GetConfiguredShaderModel();
    std::string ResolveEffectiveShaderModel(ID3D12Device* device, const std::string& requestedModel);

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
                       Microsoft::WRL::ComPtr<ID3DBlob>& outShader);

    void LogShaderResolveMessage(const std::filesystem::path& sourcePath,
                                 const char* entry,
                                 const char* target,
                                 const char* action,
                                 const std::filesystem::path& compiledPath);
}
