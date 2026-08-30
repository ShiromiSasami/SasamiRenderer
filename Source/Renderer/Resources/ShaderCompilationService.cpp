#include "Renderer/Resources/ShaderCompilationService.h"

#include <array>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <debugapi.h>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>
#include <windows.h>

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Resources/RenderPipelineStateCacheLog.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace
{
    using Microsoft::WRL::ComPtr;
    using FileTime = std::filesystem::file_time_type;

    struct ShaderModelInfo
    {
        const char* text = nullptr;
        int major = 0;
        int minor = 0;
        D3D_SHADER_MODEL model = D3D_SHADER_MODEL_6_0;
    };

    // D3D_SHADER_MODEL_6_x are ENUMERATORS (d3d12.h: D3D_SHADER_MODEL_6_9 = 0x69), not
    // preprocessor macros, so the `#ifdef D3D_SHADER_MODEL_6_9` guards this table used to
    // carry were ALWAYS false. The table collapsed to the single unguarded 6_0 entry, and
    // ResolveEffectiveShaderModel could therefore only ever return "6_0" -- on every
    // machine, no matter the hardware. Every shader in the renderer was being compiled at
    // vs_6_0/ps_6_0/cs_6_0, and the mesh shader PSO (which needs 6_5) failed to create.
    //
    // The values are part of the stable D3D ABI, so spell them out and cast rather than
    // depending on which Windows SDK this translation unit happens to see. Listing a model
    // the *runtime* does not know is safe and is exactly what the descending loop in
    // ResolveEffectiveShaderModel exists for: CheckFeatureSupport returns E_INVALIDARG for
    // an unrecognised HighestShaderModel, and the loop then tries the next one down.
    static constexpr ShaderModelInfo kKnownShaderModels[] = {
        { "6_9", 6, 9, static_cast<D3D_SHADER_MODEL>(0x69) },
        { "6_8", 6, 8, static_cast<D3D_SHADER_MODEL>(0x68) },
        { "6_7", 6, 7, static_cast<D3D_SHADER_MODEL>(0x67) },
        { "6_6", 6, 6, static_cast<D3D_SHADER_MODEL>(0x66) },
        { "6_5", 6, 5, static_cast<D3D_SHADER_MODEL>(0x65) },
        { "6_4", 6, 4, static_cast<D3D_SHADER_MODEL>(0x64) },
        { "6_3", 6, 3, static_cast<D3D_SHADER_MODEL>(0x63) },
        { "6_2", 6, 2, static_cast<D3D_SHADER_MODEL>(0x62) },
        { "6_1", 6, 1, static_cast<D3D_SHADER_MODEL>(0x61) },
        { "6_0", 6, 0, D3D_SHADER_MODEL_6_0 },
    };
    static_assert(std::size(kKnownShaderModels) > 1,
                  "kKnownShaderModels must list more than 6_0; if this fires the entries "
                  "have been made conditional again and shader model detection is dead.");
    static_assert(kKnownShaderModels[std::size(kKnownShaderModels) - 1].model == D3D_SHADER_MODEL_6_0,
                  "The descending search relies on 6_0 being the last entry.");

    static std::filesystem::path GetExecutableDir()
    {
        wchar_t exePath[MAX_PATH] = {};
        const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len == 0 || len == MAX_PATH) {
            return std::filesystem::current_path();
        }
        return std::filesystem::path(exePath).parent_path();
    }

    static bool DirectoryExists(const std::filesystem::path& path)
    {
        std::error_code ec;
        return std::filesystem::exists(path, ec) &&
            std::filesystem::is_directory(path, ec);
    }

    static std::filesystem::path ResolveShaderSourceRootFromProjectRoot(const std::filesystem::path& projectRoot)
    {
        const std::filesystem::path shaderRoot = projectRoot / L"Shaders";
        if (DirectoryExists(shaderRoot)) {
            return shaderRoot;
        }

        const std::filesystem::path legacyShaderRoot = projectRoot / L"Source" / L"Renderer" / L"Shaders";
        if (DirectoryExists(legacyShaderRoot)) {
            return legacyShaderRoot;
        }

        return shaderRoot;
    }

    static std::filesystem::path FindProjectRootWithShaders(const std::filesystem::path& startDir)
    {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::absolute(startDir, ec);
        if (ec) {
            dir = startDir;
        }

        for (;;) {
            if ((DirectoryExists(dir / L"Shaders") && DirectoryExists(dir / L"Source")) ||
                DirectoryExists(dir / L"Source" / L"Renderer" / L"Shaders")) {
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

    static std::filesystem::path GetBundledDxcRoot()
    {
        const std::filesystem::path projectRoot = FindProjectRootWithShaders(GetExecutableDir());
        if (!projectRoot.empty()) {
            return projectRoot / L"Tools" / L"DXC" / L"bin" / L"x64";
        }
        return GetExecutableDir() / L"Tools" / L"DXC" / L"bin" / L"x64";
    }

    static std::optional<std::wstring> TryGetEnvironmentValue(const wchar_t* name)
    {
        const DWORD requiredLength = GetEnvironmentVariableW(name, nullptr, 0);
        if (requiredLength == 0) {
            return std::nullopt;
        }

        std::wstring value(requiredLength, L'\0');
        const DWORD copiedLength = GetEnvironmentVariableW(name, value.data(), requiredLength);
        if (copiedLength == 0) {
            return std::nullopt;
        }

        value.resize(copiedLength);
        return value;
    }

    static std::string NarrowAscii(const std::wstring& text)
    {
        std::string result;
        result.reserve(text.size());
        for (const wchar_t ch : text) {
            result.push_back(static_cast<char>(ch));
        }
        return result;
    }

    static std::wstring Widen(const char* text)
    {
        std::wstring result;
        if (!text) {
            return result;
        }

        while (*text != '\0') {
            result.push_back(static_cast<wchar_t>(*text));
            ++text;
        }
        return result;
    }

    static bool TryParseShaderModelVersion(const std::string& text, int& major, int& minor)
    {
        const std::size_t separator = text.find('_');
        if (separator == std::string::npos || separator == 0 || separator + 1 >= text.size()) {
            return false;
        }

        try {
            major = std::stoi(text.substr(0, separator));
            minor = std::stoi(text.substr(separator + 1));
            return true;
        } catch (...) {
            return false;
        }
    }

    static const ShaderModelInfo* FindShaderModelInfo(D3D_SHADER_MODEL model)
    {
        for (const ShaderModelInfo& info : kKnownShaderModels) {
            if (info.model == model) {
                return &info;
            }
        }

        return nullptr;
    }

    static std::size_t FindRequestedShaderModelIndex(const std::string& requestedShaderModel)
    {
        int requestedMajor = 0;
        int requestedMinor = 0;
        if (!TryParseShaderModelVersion(requestedShaderModel, requestedMajor, requestedMinor)) {
            return 0;
        }

        for (std::size_t index = 0; index < std::size(kKnownShaderModels); ++index) {
            const ShaderModelInfo& info = kKnownShaderModels[index];
            if (info.major < requestedMajor ||
                (info.major == requestedMajor && info.minor <= requestedMinor)) {
                return index;
            }
        }

        return std::size(kKnownShaderModels) - 1;
    }

    static std::optional<std::filesystem::path> TryParseQuotedInclude(const std::string& line)
    {
        static const std::regex kIncludePattern("^\\s*#\\s*include\\s*\"([^\"]+)\"");
        std::smatch match;
        if (!std::regex_search(line, match, kIncludePattern)) {
            return std::nullopt;
        }
        if (match.size() < 2) {
            return std::nullopt;
        }
        return std::filesystem::path(match[1].str());
    }

    static std::filesystem::path NormalizePath(const std::filesystem::path& path)
    {
        std::error_code ec;
        std::filesystem::path absolutePath = std::filesystem::absolute(path, ec);
        if (ec) {
            absolutePath = path;
        }
        return absolutePath.lexically_normal();
    }

    static std::optional<std::filesystem::path> TryResolveIncludeFile(const std::filesystem::path& includingFile,
                                                                      const std::filesystem::path& includePath)
    {
        std::error_code ec;
        if (includePath.is_absolute()) {
            if (std::filesystem::exists(includePath, ec) && !ec) {
                return includePath;
            }
            return std::nullopt;
        }

        const std::filesystem::path parentRelative = includingFile.parent_path() / includePath;
        if (std::filesystem::exists(parentRelative, ec) && !ec) {
            return parentRelative;
        }

        const std::filesystem::path projectRoot = FindProjectRootWithShaders(GetExecutableDir());
        if (!projectRoot.empty()) {
            const std::filesystem::path shaderRootRelative =
                ResolveShaderSourceRootFromProjectRoot(projectRoot) / includePath;
            if (std::filesystem::exists(shaderRootRelative, ec) && !ec) {
                return shaderRootRelative;
            }
        }

        return std::nullopt;
    }

    static bool TryCollectLatestDependencyWriteTime(const std::filesystem::path& filePath,
                                                    FileTime& latestWriteTime,
                                                    std::unordered_set<std::wstring>& visitedFiles)
    {
        const std::filesystem::path normalizedPath = NormalizePath(filePath);
        const std::wstring visitKey = normalizedPath.native();
        if (!visitedFiles.insert(visitKey).second) {
            return true;
        }

        std::error_code ec;
        const FileTime fileWriteTime = std::filesystem::last_write_time(normalizedPath, ec);
        if (ec) {
            return false;
        }
        if (fileWriteTime > latestWriteTime) {
            latestWriteTime = fileWriteTime;
        }

        std::ifstream input(normalizedPath);
        if (!input.is_open()) {
            return false;
        }

        std::string line;
        while (std::getline(input, line)) {
            const auto includePath = TryParseQuotedInclude(line);
            if (!includePath.has_value()) {
                continue;
            }

            const auto includedFile = TryResolveIncludeFile(normalizedPath, *includePath);
            if (!includedFile.has_value()) {
                return false;
            }
            if (!TryCollectLatestDependencyWriteTime(*includedFile, latestWriteTime, visitedFiles)) {
                return false;
            }
        }

        return true;
    }

    static bool TryGetLatestDependencyWriteTime(const std::filesystem::path& sourcePath, FileTime& latestWriteTime)
    {
        latestWriteTime = (FileTime::min)();
        std::unordered_set<std::wstring> visitedFiles;
        return TryCollectLatestDependencyWriteTime(sourcePath, latestWriteTime, visitedFiles);
    }

    static HRESULT CreateDxcInstance(REFCLSID clsid, REFIID iid, void** outObject)
    {
        static HMODULE dxcompilerModule = []() -> HMODULE {
            if (const auto explicitDllPath = TryGetEnvironmentValue(L"DXC_DLL"); explicitDllPath.has_value()) {
                if (HMODULE module = LoadLibraryW(explicitDllPath->c_str())) {
                    return module;
                }
            }

            if (const auto dxcExecutablePath = TryGetEnvironmentValue(L"DXC_EXECUTABLE"); dxcExecutablePath.has_value()) {
                const std::filesystem::path siblingDllPath =
                    std::filesystem::path(*dxcExecutablePath).parent_path() / L"dxcompiler.dll";
                if (HMODULE module = LoadLibraryW(siblingDllPath.c_str())) {
                    return module;
                }
            }

            const std::filesystem::path bundledDllPath = GetBundledDxcRoot() / L"dxcompiler.dll";
            if (HMODULE module = LoadLibraryW(bundledDllPath.c_str())) {
                return module;
            }

            const std::filesystem::path localDllPath = GetExecutableDir() / L"dxcompiler.dll";
            if (HMODULE module = LoadLibraryW(localDllPath.c_str())) {
                return module;
            }

            return LoadLibraryW(L"dxcompiler.dll");
        }();
        static auto dxcCreateInstance = dxcompilerModule
            ? reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>(
                GetProcAddress(dxcompilerModule, "DxcCreateInstance"))
            : nullptr;

        if (!dxcCreateInstance) {
            return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        }

        return dxcCreateInstance(clsid, iid, outObject);
    }

} // anonymous namespace

namespace SasamiRenderer::ShaderCompilationService
{
    std::string GetConfiguredShaderModel()
    {
        const auto configuredShaderModel = TryGetEnvironmentValue(L"RENDERER_SHADER_MODEL");
        if (configuredShaderModel.has_value() && !configuredShaderModel->empty()) {
            return NarrowAscii(*configuredShaderModel);
        }
        // MUST stay in sync with <RendererShaderModel> in SasamiRenderer.vcxproj. The build
        // precompiles every shader as <name>.<entry>.<profile>_<model>.cso and the runtime looks
        // the file up by that exact name, so if the two disagree the precompiled shaders are
        // never found and each machine recompiles all of them on first launch.
        //
        // Deliberately the highest non-beta model (6_9), per the project's target: the app should
        // use the best shader model the device offers rather than pin itself to a lower one.
        //
        // The cost is known and accepted: ResolveEffectiveShaderModel clamps this request down to
        // whatever the device reports, so on any device below 6_9 the resolved profile (e.g. 6_8)
        // will not match the *_6_9.cso the build precompiled, and every shader is compiled once at
        // runtime and cached under the resolved name. Subsequent launches hit that cache. Lowering
        // this to the minimum supported device model would make precompilation effective, at the
        // cost of never using newer shader model features.
        return "6_9";
    }

    std::string ResolveEffectiveShaderModel(ID3D12Device* device, const std::string& requestedShaderModel)
    {
        if (!device) {
            return requestedShaderModel;
        }

        HRESULT lastHr = S_OK;
        const std::size_t startIndex = FindRequestedShaderModelIndex(requestedShaderModel);
        for (std::size_t index = startIndex; index < std::size(kKnownShaderModels); ++index) {
            D3D12_FEATURE_DATA_SHADER_MODEL featureData{};
            featureData.HighestShaderModel = kKnownShaderModels[index].model;

            const HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
                                                           &featureData,
                                                           sizeof(featureData));
            if (SUCCEEDED(hr)) {
                if (const ShaderModelInfo* supported = FindShaderModelInfo(featureData.HighestShaderModel)) {
                    if (requestedShaderModel != supported->text) {
                        std::string message =
                            "RenderPipelineStateCache::Initialize: requested shader model ";
                        message += requestedShaderModel;
                        message += ", runtime/device supports up to ";
                        message += supported->text;
                        message += ". Falling back.\n";
                        DebugLog(message.c_str());
                    }

                    return supported->text;
                }

                return kKnownShaderModels[index].text;
            }

            lastHr = hr;
        }

        std::string message =
            "RenderPipelineStateCache::Initialize: D3D12_FEATURE_SHADER_MODEL query failed for all known models. ";
        message += "Falling back to ";
        message += kKnownShaderModels[std::size(kKnownShaderModels) - 1].text;
        message += ". lastHr=";
        message += FmtHr(lastHr);
        message += "\n";
        DebugLog(message.c_str());

        return kKnownShaderModels[std::size(kKnownShaderModels) - 1].text;
    }

    std::wstring ResolveEffectiveComputeShaderTarget(ID3D12Device* device, const std::string& requestedModel)
    {
        const std::string smStr = ResolveEffectiveShaderModel(device, requestedModel);
        return L"cs_" + std::wstring(smStr.begin(), smStr.end());
    }

    void ResolveEffectiveVsPsTargets(ID3D12Device* device, const std::string& requestedModel,
                                     std::string& outVsTarget, std::string& outPsTarget)
    {
        const std::string smStr = ResolveEffectiveShaderModel(device, requestedModel);
        outVsTarget = "vs_" + smStr;
        outPsTarget = "ps_" + smStr;
    }

    std::filesystem::path GetShaderSourceRoot()
    {
        static const std::filesystem::path shaderRoot = []() {
            const std::filesystem::path projectRoot = FindProjectRootWithShaders(GetExecutableDir());
            if (!projectRoot.empty()) {
                return ResolveShaderSourceRootFromProjectRoot(projectRoot);
            }
            return std::filesystem::path(L"Shaders");
        }();

        return shaderRoot;
    }

    std::filesystem::path ResolveShaderPath(const wchar_t* fileName)
    {
        return GetShaderSourceRoot() / fileName;
    }

    std::filesystem::path ResolveCompiledShaderPath(const std::filesystem::path& sourcePath,
                                                    const char* entry,
                                                    const char* target)
    {
        const std::filesystem::path shaderRoot = NormalizePath(GetShaderSourceRoot());
        const std::filesystem::path normalizedSource = NormalizePath(sourcePath);

        std::error_code ec;
        std::filesystem::path relativeSource = std::filesystem::relative(normalizedSource, shaderRoot, ec);
        if (ec || relativeSource.empty()) {
            relativeSource = normalizedSource.filename();
        }

        std::filesystem::path compiledFileName = relativeSource.filename().stem();
        compiledFileName += L".";
        compiledFileName += Widen(entry);
        compiledFileName += L".";
        compiledFileName += Widen(target);
        compiledFileName += L".cso";

        return GetExecutableDir() / L"Shaders" / relativeSource.parent_path() / compiledFileName;
    }

    bool IsCompiledShaderUpToDate(const std::filesystem::path& compiledPath,
                                  const std::filesystem::path& sourcePath)
    {
        std::error_code ec;
        if (!std::filesystem::exists(compiledPath, ec) || ec) {
            return false;
        }

        FileTime latestDependencyWriteTime{};
        if (!TryGetLatestDependencyWriteTime(sourcePath, latestDependencyWriteTime)) {
            return false;
        }

        const FileTime compiledWriteTime = std::filesystem::last_write_time(compiledPath, ec);
        if (ec) {
            return false;
        }
        return compiledWriteTime >= latestDependencyWriteTime;
    }

    void LogShaderResolveMessage(const std::filesystem::path& sourcePath,
                                 const char* entry,
                                 const char* target,
                                 const char* action,
                                 const std::filesystem::path& compiledPath)
    {
        std::string message = "RenderPipelineStateCache::ResolveShader ";
        message += sourcePath.filename().string();
        message += " (";
        message += entry;
        message += "/";
        message += target;
        message += "): ";
        message += action;
        message += " -> ";
        message += compiledPath.string();
        message += "\n";
        DebugLog(message.c_str());
    }

    bool CompileShader(const std::filesystem::path& sourcePath,
                       const char* entry,
                       const char* target,
                       Microsoft::WRL::ComPtr<ID3DBlob>& outShader,
                       bool spirv)
    {
        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        HRESULT hr = CreateDxcInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
        if (FAILED(hr)) {
            DebugLog("RenderPipelineStateCache::CompileShader: failed to create DXC utils.\n");
            return false;
        }

        hr = CreateDxcInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
        if (FAILED(hr)) {
            DebugLog("RenderPipelineStateCache::CompileShader: failed to create DXC compiler.\n");
            return false;
        }

        ComPtr<IDxcBlobEncoding> sourceBlob;
        hr = utils->LoadFile(sourcePath.c_str(), nullptr, &sourceBlob);
        if (FAILED(hr)) {
            std::string msg = "RenderPipelineStateCache::CompileShader: failed to load source file: ";
            msg += sourcePath.string();
            msg += "\n";
            DebugLog(msg.c_str());
            return false;
        }

        ComPtr<IDxcIncludeHandler> includeHandler;
        hr = utils->CreateDefaultIncludeHandler(&includeHandler);
        if (FAILED(hr)) {
            DebugLog("RenderPipelineStateCache::CompileShader: failed to create DXC include handler.\n");
            return false;
        }

        const std::wstring sourceArg = sourcePath.native();
        const std::wstring entryArg = Widen(entry);
        const std::wstring targetArg = Widen(target);
        const std::wstring includeArg = GetShaderSourceRoot().native();

        std::vector<LPCWSTR> arguments{
            sourceArg.c_str(),
            L"-E", entryArg.c_str(),
            L"-T", targetArg.c_str(),
            L"-I", includeArg.c_str(),
            L"-WX"
        };

        if (spirv) {
            arguments.push_back(L"-spirv");
            arguments.push_back(L"-fvk-use-dx-layout");
            arguments.push_back(L"-fvk-use-dx-position-w");
            arguments.push_back(L"-fvk-b-shift");
            arguments.push_back(L"0");
            arguments.push_back(L"0");
            arguments.push_back(L"-fvk-t-shift");
            arguments.push_back(L"100");
            arguments.push_back(L"0");
            arguments.push_back(L"-fvk-s-shift");
            arguments.push_back(L"200");
            arguments.push_back(L"0");
            arguments.push_back(L"-fvk-u-shift");
            arguments.push_back(L"300");
            arguments.push_back(L"0");
        }

#if defined(_DEBUG)
        arguments.push_back(L"-Zi");
        arguments.push_back(L"-Od");
#else
        arguments.push_back(L"-O3");
#endif

        DxcBuffer sourceBuffer{};
        sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
        sourceBuffer.Size = sourceBlob->GetBufferSize();
        sourceBuffer.Encoding = DXC_CP_ACP;

        ComPtr<IDxcResult> result;
        hr = compiler->Compile(&sourceBuffer,
                               arguments.data(),
                               static_cast<UINT32>(arguments.size()),
                               includeHandler.Get(),
                               IID_PPV_ARGS(&result));
        if (FAILED(hr) || !result) {
            DebugLog("RenderPipelineStateCache::CompileShader: DXC compile invocation failed.\n");
            return false;
        }

        ComPtr<IDxcBlobUtf8> errorText;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errorText), nullptr)) &&
            errorText &&
            errorText->GetStringLength() > 0) {
            DebugLog(errorText->GetStringPointer());
            DebugLog("\n");
        }

        HRESULT compileStatus = S_OK;
        hr = result->GetStatus(&compileStatus);
        if (FAILED(hr) || FAILED(compileStatus)) {
            return false;
        }

        ComPtr<IDxcBlob> objectBlob;
        hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&objectBlob), nullptr);
        if (FAILED(hr) || !objectBlob) {
            DebugLog("RenderPipelineStateCache::CompileShader: DXC object output is missing.\n");
            return false;
        }

        hr = D3DCreateBlob(objectBlob->GetBufferSize(), outShader.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !outShader) {
            DebugLog("RenderPipelineStateCache::CompileShader: failed to allocate shader blob.\n");
            return false;
        }

        std::memcpy(outShader->GetBufferPointer(), objectBlob->GetBufferPointer(), objectBlob->GetBufferSize());
        return true;
    }

} // namespace SasamiRenderer::ShaderCompilationService
