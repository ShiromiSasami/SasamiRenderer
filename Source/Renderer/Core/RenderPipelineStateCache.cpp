#include "Renderer/Core/RenderPipelineStateCache.h"

#include <array>
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
#include "Foundation/Tools/ScopedPerfTimer.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace SasamiRenderer
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        using FileTime = std::filesystem::file_time_type;

        struct ShaderSpec
        {
            const wchar_t* sourceRelativePath = nullptr;
            const char* entry = nullptr;
            const char* target = nullptr;
            ComPtr<ID3DBlob>* output = nullptr;
        };

        struct ShaderModelInfo
        {
            const char* text = nullptr;
            int major = 0;
            int minor = 0;
            D3D_SHADER_MODEL model = D3D_SHADER_MODEL_6_0;
        };

        static constexpr ShaderModelInfo kKnownShaderModels[] = {
#ifdef D3D_SHADER_MODEL_6_9
            { "6_9", 6, 9, D3D_SHADER_MODEL_6_9 },
#endif
#ifdef D3D_SHADER_MODEL_6_8
            { "6_8", 6, 8, D3D_SHADER_MODEL_6_8 },
#endif
#ifdef D3D_SHADER_MODEL_6_7
            { "6_7", 6, 7, D3D_SHADER_MODEL_6_7 },
#endif
#ifdef D3D_SHADER_MODEL_6_6
            { "6_6", 6, 6, D3D_SHADER_MODEL_6_6 },
#endif
#ifdef D3D_SHADER_MODEL_6_5
            { "6_5", 6, 5, D3D_SHADER_MODEL_6_5 },
#endif
#ifdef D3D_SHADER_MODEL_6_4
            { "6_4", 6, 4, D3D_SHADER_MODEL_6_4 },
#endif
#ifdef D3D_SHADER_MODEL_6_3
            { "6_3", 6, 3, D3D_SHADER_MODEL_6_3 },
#endif
#ifdef D3D_SHADER_MODEL_6_2
            { "6_2", 6, 2, D3D_SHADER_MODEL_6_2 },
#endif
#ifdef D3D_SHADER_MODEL_6_1
            { "6_1", 6, 1, D3D_SHADER_MODEL_6_1 },
#endif
            { "6_0", 6, 0, D3D_SHADER_MODEL_6_0 },
        };

        static std::filesystem::path GetExecutableDir()
        {
            wchar_t exePath[MAX_PATH] = {};
            const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (len == 0 || len == MAX_PATH) {
                return std::filesystem::current_path();
            }
            return std::filesystem::path(exePath).parent_path();
        }

        static std::filesystem::path FindProjectRootWithShaders(const std::filesystem::path& startDir)
        {
            std::error_code ec;
            std::filesystem::path dir = std::filesystem::absolute(startDir, ec);
            if (ec) {
                dir = startDir;
            }

            for (;;) {
                const std::filesystem::path shaderDir = dir / L"Source" / L"Renderer" / L"Shaders";
                if (std::filesystem::exists(shaderDir, ec) &&
                    std::filesystem::is_directory(shaderDir, ec)) {
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

        static const std::filesystem::path& GetShaderSourceRoot()
        {
            static const std::filesystem::path shaderRoot = []() {
                const std::filesystem::path projectRoot = FindProjectRootWithShaders(GetExecutableDir());
                if (!projectRoot.empty()) {
                    return projectRoot / L"Source" / L"Renderer" / L"Shaders";
                }
                return std::filesystem::path(L"Source/Renderer/Shaders");
            }();

            return shaderRoot;
        }

        static std::filesystem::path ResolveShaderPath(const wchar_t* fileName)
        {
            return GetShaderSourceRoot() / fileName;
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

        static std::string GetConfiguredShaderModel()
        {
            const auto configuredShaderModel = TryGetEnvironmentValue(L"RENDERER_SHADER_MODEL");
            if (configuredShaderModel.has_value() && !configuredShaderModel->empty()) {
                return NarrowAscii(*configuredShaderModel);
            }
            return "6_9";
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

        static std::string FormatHResult(HRESULT hr)
        {
            char text[16] = {};
            std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned int>(hr));
            return text;
        }

        static void LogFailureMessage(const char* context, HRESULT hr)
        {
            std::string message = context;
            message += " failed. hr=";
            message += FormatHResult(hr);
            message += "\n";
            DebugLog(message.c_str());
        }

        static std::string ResolveEffectiveShaderModel(ID3D12Device* device, const std::string& requestedShaderModel)
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
            message += FormatHResult(lastHr);
            message += "\n";
            DebugLog(message.c_str());

            return kKnownShaderModels[std::size(kKnownShaderModels) - 1].text;
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

                const std::filesystem::path includedFile = normalizedPath.parent_path() / *includePath;
                if (!TryCollectLatestDependencyWriteTime(includedFile, latestWriteTime, visitedFiles)) {
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

        static std::filesystem::path ResolveCompiledShaderPath(const std::filesystem::path& sourcePath,
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

        static bool IsCompiledShaderUpToDate(const std::filesystem::path& compiledPath,
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

        static void LogShaderResolveMessage(const std::filesystem::path& sourcePath,
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

        static bool CompileShaderWithDxc(const std::filesystem::path& sourcePath,
                                         const char* entry,
                                         const char* target,
                                         ComPtr<ID3DBlob>& outShader)
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
    }

    bool RenderPipelineStateCache::Initialize(GraphicsDevice& device)
    {
        ScopedPerfTimer perfTimer("RenderPipelineStateCache::Initialize");
        const std::string configuredShaderModel = GetConfiguredShaderModel();
        const std::string shaderModel = ResolveEffectiveShaderModel(device.GetDevice(), configuredShaderModel);

        // Root signature:
        // [0]=material SRV (t0), [1]=shadow SRV (t1), [2]=CBV (b0), [3]=CBV (b1),
        // [4]=light SRVs (t2-t3), [5]=IBL SRVs (t4-t6), [6]=AO SRV (t7), [7]=reflection SRV (t8),
        // [8]=SSAO SRV (t9), [9]=CBV (b2 GI probe grid), [10]=inline SRV (t10 probe SH data)
        D3D12_DESCRIPTOR_RANGE descRangeMaterial{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 0,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_DESCRIPTOR_RANGE descRangeShadow{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 1,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_DESCRIPTOR_RANGE descRangeLights{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 2,
            .BaseShaderRegister = 2,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_DESCRIPTOR_RANGE descRangeIbl{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 3,
            .BaseShaderRegister = 4,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_DESCRIPTOR_RANGE descRangeOcclusion{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 7,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_DESCRIPTOR_RANGE descRangeReflection{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 8,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_DESCRIPTOR_RANGE descRangeSceneDepth{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 11,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_ROOT_PARAMETER rootParams[13] = {};
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[0].DescriptorTable.pDescriptorRanges = &descRangeMaterial;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[1].DescriptorTable.pDescriptorRanges = &descRangeShadow;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[2].Descriptor.ShaderRegister = 0; // b0
        rootParams[2].Descriptor.RegisterSpace = 0;
        // Make camera CB visible to all stages (VS/HS/DS/GS)
        rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[3].Descriptor.ShaderRegister = 1; // b1 (used in VS+PS)
        rootParams[3].Descriptor.RegisterSpace = 0;
        rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[4].DescriptorTable.pDescriptorRanges = &descRangeLights;
        rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[5].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[5].DescriptorTable.pDescriptorRanges = &descRangeIbl;
        rootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[6].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[6].DescriptorTable.pDescriptorRanges = &descRangeOcclusion;
        rootParams[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[7].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[7].DescriptorTable.pDescriptorRanges = &descRangeReflection;
        rootParams[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // [8] t9 - Screen-space AO (SSAO output), PS only
        D3D12_DESCRIPTOR_RANGE descRangeScreenSpaceAO{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 9,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        rootParams[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[8].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[8].DescriptorTable.pDescriptorRanges = &descRangeScreenSpaceAO;
        rootParams[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // [9] b2 - GI probe grid CBV (inline root descriptor, PS only)
        rootParams[9].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[9].Descriptor.ShaderRegister = 2; // b2
        rootParams[9].Descriptor.RegisterSpace  = 0;
        rootParams[9].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        // [10] t10 - GI probe SH data (inline root SRV, PS only)
        rootParams[10].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[10].Descriptor.ShaderRegister = 10; // t10
        rootParams[10].Descriptor.RegisterSpace  = 0;
        rootParams[10].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        // [11] t11 - Scene depth for screen-space contact shadows
        rootParams[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[11].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[11].DescriptorTable.pDescriptorRanges = &descRangeSceneDepth;
        rootParams[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // [12] t12 - Spot light shadow map SRV, PS only
        D3D12_DESCRIPTOR_RANGE descRangeSpotShadow{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 12,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        rootParams[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[12].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[12].DescriptorTable.pDescriptorRanges = &descRangeSpotShadow;
        rootParams[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.ShaderRegister = 0;
        samplerDesc.RegisterSpace = 0;
        samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.NumParameters = 13;
        rootSigDesc.pParameters = rootParams;
        rootSigDesc.NumStaticSamplers = 1;
        rootSigDesc.pStaticSamplers = &samplerDesc;
        rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
        if (FAILED(hr)) {
            if (error && error->GetBufferPointer() && error->GetBufferSize() > 0) {
                DebugLog(static_cast<const char*>(error->GetBufferPointer()));
                DebugLog("\n");
            }
            LogFailureMessage("RenderPipelineStateCache::Initialize: D3D12SerializeRootSignature", hr);
            return false;
        }

        hr = device.CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), m_rootSignature);
        if (FAILED(hr)) {
            LogFailureMessage("RenderPipelineStateCache::Initialize: CreateRootSignature", hr);
            return false;
        }

        // Resolve shader blobs from precompiled CSO first, then compile only stale/missing ones.
        ComPtr<ID3DBlob> vertexShader;
        ComPtr<ID3DBlob> pixelShader;
        ComPtr<ID3DBlob> basicVertexShader;
        ComPtr<ID3DBlob> basicPixelShader;
        ComPtr<ID3DBlob> skyboxVS;
        ComPtr<ID3DBlob> skyboxHdrPS;
        ComPtr<ID3DBlob> skyboxLdrPS;
        ComPtr<ID3DBlob> tessVS, tessHS, tessDS, tessGS, tessDebugDS;
        ComPtr<ID3DBlob> tessDebugPS;
        ComPtr<ID3DBlob> meshletDebugPS;
        ComPtr<ID3DBlob> ssaoVS;
        ComPtr<ID3DBlob> ssaoPS;
        ComPtr<ID3DBlob> ssaoBlurPS;
        ComPtr<ID3DBlob> proceduralSkyPS;
        ComPtr<ID3DBlob> sdfFluidVS;
        ComPtr<ID3DBlob> sdfFluidPS;
        ComPtr<ID3DBlob> rayMarchVS;
        ComPtr<ID3DBlob> rayMarchPS;
        ComPtr<ID3DBlob> volumetricCloudVS;
        ComPtr<ID3DBlob> volumetricCloudPS;
        ComPtr<ID3DBlob> swrtReflectionCompositePS;
        auto loadOrCompileShader = [&](const ShaderSpec& spec) -> bool
        {
            const std::filesystem::path sourcePath = ResolveShaderPath(spec.sourceRelativePath);
            const std::filesystem::path compiledPath = ResolveCompiledShaderPath(sourcePath, spec.entry, spec.target);
            spec.output->Reset();

            if (IsCompiledShaderUpToDate(compiledPath, sourcePath)) {
                hr = D3DReadFileToBlob(compiledPath.c_str(), spec.output->GetAddressOf());
                if (SUCCEEDED(hr)) {
                    LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "loaded precompiled shader", compiledPath);
                    return true;
                }

                LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "failed to read precompiled shader, falling back to runtime compile", compiledPath);
            } else {
                LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "precompiled shader missing or stale, runtime compiling", compiledPath);
            }

            ScopedPerfTimer::Timestamp compileStart = ScopedPerfTimer::Now();
            const bool compileSucceeded = CompileShaderWithDxc(sourcePath, spec.entry, spec.target, *spec.output);
            const ScopedPerfTimer::Timestamp compileEnd = ScopedPerfTimer::Now();

            std::string perfLabel = "RenderPipelineStateCache::CompileShader ";
            perfLabel += sourcePath.filename().string();
            perfLabel += " (";
            perfLabel += spec.entry;
            perfLabel += "/";
            perfLabel += spec.target;
            perfLabel += ")";
            ScopedPerfTimer::LogMilliseconds(perfLabel.c_str(),
                           ScopedPerfTimer::ElapsedMilliseconds(compileStart, compileEnd));

            if (!compileSucceeded) {
                std::string msg = "RenderPipelineStateCache::Initialize: shader compile failed: ";
                msg += sourcePath.string();
                msg += "\n";
                DebugLogDialog(msg.c_str(), L"Shader Initialize Error", MB_OK | MB_ICONERROR);
                return false;
            }

            std::error_code ec;
            std::filesystem::create_directories(compiledPath.parent_path(), ec);
            if (!ec) {
                const HRESULT writeHr = D3DWriteBlobToFile(spec.output->Get(), compiledPath.c_str(), TRUE);
                if (FAILED(writeHr)) {
                    LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "compiled shader could not be written", compiledPath);
                } else {
                    LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "runtime compiled shader and updated cache", compiledPath);
                }
            } else {
                LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "cache directory creation failed; keeping runtime blob only", compiledPath);
            }

            return true;
        };

        // Skybox PS split:
        // - Skybox_PS.hlsl      : LDR cubemap path (direct sample)
        // - Skybox_HDR_PS.hlsl  : HDR cubemap path (tone map + gamma)
        const std::string vertexProfile = "vs_" + shaderModel;
        const std::string pixelProfile = "ps_" + shaderModel;
        const std::string hullProfile = "hs_" + shaderModel;
        const std::string domainProfile = "ds_" + shaderModel;
        const std::string geometryProfile = "gs_" + shaderModel;
        const std::array<ShaderSpec, 25> shaderSpecs{ {
            { L"CookTorranceGGX_VS.hlsl", "VSMain", vertexProfile.c_str(), &vertexShader },
            { L"CookTorranceGGX_PS.hlsl", "PSMain", pixelProfile.c_str(), &pixelShader },
            { L"Opaque_VS.hlsl", "VSMain", vertexProfile.c_str(), &basicVertexShader },
            { L"Opaque_PS.hlsl", "PSMain", pixelProfile.c_str(), &basicPixelShader },
            { L"Skybox/Skybox_VS.hlsl", "VSMain", vertexProfile.c_str(), &skyboxVS },
            { L"Skybox/Skybox_HDR_PS.hlsl", "PSMain", pixelProfile.c_str(), &skyboxHdrPS },
            { L"Skybox/Skybox_PS.hlsl", "PSMain", pixelProfile.c_str(), &skyboxLdrPS },
            { L"Tessellation_VS.hlsl", "VSMain", vertexProfile.c_str(), &tessVS },
            { L"Tessellation_HS.hlsl", "HSMain", hullProfile.c_str(), &tessHS },
            { L"Tessellation_DS.hlsl", "DSMain", domainProfile.c_str(), &tessDS },
            { L"Tessellation_GS.hlsl", "GSMain", geometryProfile.c_str(), &tessGS },
            { L"Debug/Tessellation_Debug_DS.hlsl", "DSMain", domainProfile.c_str(), &tessDebugDS },
            { L"Debug/Tessellation_Debug_PS.hlsl", "PSMain", pixelProfile.c_str(), &tessDebugPS },
            { L"Debug/MeshletDebug_PS.hlsl",        "PSMain", pixelProfile.c_str(), &meshletDebugPS },
            { L"SSAO/SSAO_VS.hlsl", "VSMain", vertexProfile.c_str(), &ssaoVS },
            { L"SSAO/SSAO_PS.hlsl", "PSMain", pixelProfile.c_str(), &ssaoPS },
            { L"SSAO/SSAO_Blur_PS.hlsl", "PSMain", pixelProfile.c_str(), &ssaoBlurPS },
            { L"ProceduralSky/ProceduralSky_PS.hlsl", "PSMain", pixelProfile.c_str(), &proceduralSkyPS },
            { L"SdfFluid/SdfFluid_VS.hlsl", "VSMain", vertexProfile.c_str(), &sdfFluidVS },
            { L"SdfFluid/SdfFluid_PS.hlsl", "PSMain", pixelProfile.c_str(), &sdfFluidPS },
            { L"VolumetricCloud/VolumetricCloud_VS.hlsl", "VSMain", vertexProfile.c_str(), &volumetricCloudVS },
            { L"VolumetricCloud/VolumetricCloud_PS.hlsl", "PSMain", pixelProfile.c_str(), &volumetricCloudPS },
            { L"SWRT/SWRT_ReflectionComposite_PS.hlsl", "PSMain", pixelProfile.c_str(), &swrtReflectionCompositePS },
            { L"RayMarch/RayMarch_VS.hlsl", "VSMain", vertexProfile.c_str(), &rayMarchVS },
            { L"RayMarch/RayMarch_PS.hlsl", "PSMain", pixelProfile.c_str(), &rayMarchPS },
        } };

        for (const ShaderSpec& spec : shaderSpecs) {
            if (!loadOrCompileShader(spec)) {
                return false;
            }
        }

        D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        D3D12_INPUT_ELEMENT_DESC skyboxInputElementDescs[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        auto blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        // Opaque pass: disable blending to avoid translucency
        blendDesc.RenderTarget[0].BlendEnable = FALSE;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        auto rast = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        rast.CullMode = D3D12_CULL_MODE_NONE;

        auto shadowRast = rast;
        shadowRast.DepthBias = 1000;
        shadowRast.DepthBiasClamp = 0.0f;
        shadowRast.SlopeScaledDepthBias = 2.0f;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
        psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
        psoDesc.RasterizerState = rast;
        psoDesc.BlendState = blendDesc;
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 5;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;    // SceneColor
        psoDesc.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;    // GBufferAlbedo
        psoDesc.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT; // GBufferNormal
        psoDesc.RTVFormats[3] = DXGI_FORMAT_R8G8B8A8_UNORM;    // GBufferMaterial
        psoDesc.RTVFormats[4] = DXGI_FORMAT_R16G16B16A16_FLOAT; // GBufferEmissive
        psoDesc.SampleDesc.Count = 1;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        auto createPipelineState = [&](const char* label,
                                       const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                                       PipelineState& outState) -> bool
        {
            hr = device.CreateGraphicsPipelineState(desc, outState);
            if (FAILED(hr)) {
                std::string context = "RenderPipelineStateCache::Initialize: CreateGraphicsPipelineState(";
                context += label;
                context += ")";
                LogFailureMessage(context.c_str(), hr);
                return false;
            }

            return true;
        };

        if (!createPipelineState("CookTorranceGGX", psoDesc, m_pipelineState)) { return false; }

        D3D12_BLEND_DESC transparentBlend = blendDesc;
        for (UINT rtIndex = 0; rtIndex < 5; ++rtIndex) {
            auto& rt = transparentBlend.RenderTarget[rtIndex];
            rt.BlendEnable = (rtIndex == 0) ? TRUE : FALSE;
            rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTransparent = psoDesc;
        psoTransparent.BlendState = transparentBlend;
        psoTransparent.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoTransparent.NumRenderTargets = 1;
        psoTransparent.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
        psoTransparent.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
        psoTransparent.RTVFormats[3] = DXGI_FORMAT_UNKNOWN;
        psoTransparent.RTVFormats[4] = DXGI_FORMAT_UNKNOWN;
        if (!createPipelineState("CookTorranceGGXTransparent", psoTransparent, m_transparentPipelineState)) { return false; }

        // Opaque raster pipeline (unlit opaque draw path) — single RTV only
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoBasic = psoDesc;
        psoBasic.VS = { basicVertexShader->GetBufferPointer(), basicVertexShader->GetBufferSize() };
        psoBasic.PS = { basicPixelShader->GetBufferPointer(), basicPixelShader->GetBufferSize() };
        psoBasic.NumRenderTargets = 1;
        psoBasic.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
        psoBasic.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
        psoBasic.RTVFormats[3] = DXGI_FORMAT_UNKNOWN;
        psoBasic.RTVFormats[4] = DXGI_FORMAT_UNKNOWN;
        if (!createPipelineState("Opaque", psoBasic, m_basicPipelineState)) { return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTransparentBasic = psoBasic;
        psoTransparentBasic.BlendState = transparentBlend;
        psoTransparentBasic.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        if (!createPipelineState("OpaqueTransparent", psoTransparentBasic, m_transparentBasicPipelineState)) { return false; }

        D3D12_BLEND_DESC additiveBlend = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        additiveBlend.RenderTarget[0].BlendEnable = TRUE;
        additiveBlend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        additiveBlend.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        additiveBlend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        additiveBlend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
        additiveBlend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        additiveBlend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        additiveBlend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSwrtReflectionComposite = psoBasic;
        psoSwrtReflectionComposite.InputLayout = { nullptr, 0 };
        psoSwrtReflectionComposite.VS = { ssaoVS->GetBufferPointer(), ssaoVS->GetBufferSize() };
        psoSwrtReflectionComposite.PS = { swrtReflectionCompositePS->GetBufferPointer(), swrtReflectionCompositePS->GetBufferSize() };
        psoSwrtReflectionComposite.BlendState = additiveBlend;
        psoSwrtReflectionComposite.DepthStencilState.DepthEnable = FALSE;
        psoSwrtReflectionComposite.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoSwrtReflectionComposite.DepthStencilState.StencilEnable = FALSE;
        if (!createPipelineState("SWRTReflectionComposite", psoSwrtReflectionComposite, m_swrtReflectionCompositePipelineState)) { return false; }

        // Shadow pipeline (depth-only, reuse VS; no PS)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoShadow = {};
        psoShadow.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoShadow.pRootSignature = m_rootSignature.Get();
        psoShadow.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
        psoShadow.PS = { nullptr, 0 };
        psoShadow.RasterizerState = shadowRast;
        psoShadow.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoShadow.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoShadow.SampleMask = UINT_MAX;
        psoShadow.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoShadow.NumRenderTargets = 0;
        psoShadow.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoShadow.SampleDesc.Count = 1;

        if (!createPipelineState("Shadow", psoShadow, m_shadowPipelineState)) { return false; }

        // Tessellation pipeline (VS+HS+DS+GS+PS)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTess = {};
        psoTess.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoTess.pRootSignature = m_rootSignature.Get();
        psoTess.VS = { tessVS->GetBufferPointer(), tessVS->GetBufferSize() };
        psoTess.HS = { tessHS->GetBufferPointer(), tessHS->GetBufferSize() };
        psoTess.DS = { tessDS->GetBufferPointer(), tessDS->GetBufferSize() };
        psoTess.GS = { tessGS->GetBufferPointer(), tessGS->GetBufferSize() };
        psoTess.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() }; // Reuse PBR PS
        auto rastTess = rast;
        rastTess.CullMode = D3D12_CULL_MODE_NONE; // HS uses triangle_cw; keep NONE to match standard PSO
        psoTess.RasterizerState = rastTess;
        psoTess.BlendState = blendDesc;
        psoTess.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoTess.DepthStencilState.DepthEnable = TRUE;
        psoTess.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        psoTess.DepthStencilState.StencilEnable = FALSE;
        psoTess.SampleMask = UINT_MAX;
        psoTess.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        psoTess.NumRenderTargets = 5;
        psoTess.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;    // SceneColor
        psoTess.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;    // GBufferAlbedo
        psoTess.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT; // GBufferNormal
        psoTess.RTVFormats[3] = DXGI_FORMAT_R8G8B8A8_UNORM;    // GBufferMaterial
        psoTess.RTVFormats[4] = DXGI_FORMAT_R16G16B16A16_FLOAT; // GBufferEmissive
        psoTess.SampleDesc.Count = 1;
        psoTess.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        if (!createPipelineState("Tessellation", psoTess, m_tessPipelineState)) { return false; }

        // Tessellation wireframe pipeline — same as tessellation but FillMode = WIREFRAME.
        // Lets the user visualize the polygon mesh formed by the tessellation stage.
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTessWF = psoTess;
            auto rastTessWF = rastTess;
            rastTessWF.FillMode = D3D12_FILL_MODE_WIREFRAME;
            psoTessWF.RasterizerState = rastTessWF;
            if (!createPipelineState("TessellationWireframe", psoTessWF, m_tessWireframePipelineState)) { return false; }
        }

        // Tessellation debug pipeline — same stages as tessellation but uses
        // Tessellation_Debug_PS which flat-shades by per-patch color (input.color).
        // This gives a clean meshlet-style patch boundary visualization.
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTessDbg = psoTess;
            psoTessDbg.DS = { tessDebugDS->GetBufferPointer(), tessDebugDS->GetBufferSize() };
            psoTessDbg.PS = { tessDebugPS->GetBufferPointer(), tessDebugPS->GetBufferSize() };
            if (!createPipelineState("TessellationDebug", psoTessDbg, m_tessDebugPipelineState)) { return false; }
        }

        // Meshlet debug pipeline — VS + MeshletDebug_PS.
        // Uses SV_PrimitiveID / 16 to derive the meshlet index (exact for sequential
        // meshlet builds) so each meshlet group gets a unique color.  Works with the
        // standard DrawIndexedInstanced path; no mesh shader dispatch required.
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoMeshDbg = psoDesc;
            psoMeshDbg.PS = { meshletDebugPS->GetBufferPointer(), meshletDebugPS->GetBufferSize() };
            if (!createPipelineState("MeshletDebug", psoMeshDbg, m_meshletDebugPipelineState)) { return false; }
        }

        // Tessellation shadow pipeline (VS+HS+DS; depth-only)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTessShadow = {};
        psoTessShadow.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoTessShadow.pRootSignature = m_rootSignature.Get();
        psoTessShadow.VS = { tessVS->GetBufferPointer(), tessVS->GetBufferSize() };
        psoTessShadow.HS = { tessHS->GetBufferPointer(), tessHS->GetBufferSize() };
        psoTessShadow.DS = { tessDS->GetBufferPointer(), tessDS->GetBufferSize() };
        psoTessShadow.GS = { nullptr, 0 };
        psoTessShadow.PS = { nullptr, 0 };
        auto shadowRastTess = rastTess;
        shadowRastTess.DepthBias = shadowRast.DepthBias;
        shadowRastTess.DepthBiasClamp = shadowRast.DepthBiasClamp;
        shadowRastTess.SlopeScaledDepthBias = shadowRast.SlopeScaledDepthBias;
        psoTessShadow.RasterizerState = shadowRastTess;
        psoTessShadow.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoTessShadow.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoTessShadow.SampleMask = UINT_MAX;
        psoTessShadow.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        psoTessShadow.NumRenderTargets = 0;
        psoTessShadow.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoTessShadow.SampleDesc.Count = 1;

        if (!createPipelineState("TessellationShadow", psoTessShadow, m_tessShadowPipelineState)) { return false; }

        // Skybox pipeline (cube map)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkybox = {};
        psoSkybox.InputLayout = { skyboxInputElementDescs, _countof(skyboxInputElementDescs) };
        psoSkybox.pRootSignature = m_rootSignature.Get();
        psoSkybox.VS = { skyboxVS->GetBufferPointer(), skyboxVS->GetBufferSize() };
        psoSkybox.RasterizerState = rast;
        psoSkybox.BlendState = blendDesc;
        psoSkybox.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoSkybox.DepthStencilState.DepthEnable = TRUE;
        psoSkybox.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoSkybox.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        psoSkybox.DepthStencilState.StencilEnable = FALSE;
        psoSkybox.SampleMask = UINT_MAX;
        psoSkybox.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoSkybox.NumRenderTargets = 1;
        psoSkybox.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoSkybox.SampleDesc.Count = 1;
        psoSkybox.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkyboxHdr = psoSkybox;
        psoSkyboxHdr.PS = { skyboxHdrPS->GetBufferPointer(), skyboxHdrPS->GetBufferSize() };
        if (!createPipelineState("SkyboxHDR", psoSkyboxHdr, m_skyboxHdrPipelineState)) { return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkyboxLdr = psoSkybox;
        psoSkyboxLdr.PS = { skyboxLdrPS->GetBufferPointer(), skyboxLdrPS->GetBufferSize() };
        if (!createPipelineState("SkyboxLDR", psoSkyboxLdr, m_skyboxLdrPipelineState)) { return false; }

        // Procedural sky pipeline: same cube mesh + depth state as skybox, different PS (no texture)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoProceduralSky = psoSkybox;
        psoProceduralSky.PS = { proceduralSkyPS->GetBufferPointer(), proceduralSkyPS->GetBufferSize() };
        if (!createPipelineState("ProceduralSky", psoProceduralSky, m_proceduralSkyPipelineState)) { return false; }

        // --- SdfFluid root signature (single root CBV at b0) ---
        {
            D3D12_ROOT_PARAMETER sdfParam = {};
            sdfParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            sdfParam.Descriptor.ShaderRegister = 0;
            sdfParam.Descriptor.RegisterSpace  = 0;
            sdfParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_ROOT_SIGNATURE_DESC sdfRsDesc = {};
            sdfRsDesc.NumParameters = 1;
            sdfRsDesc.pParameters   = &sdfParam;
            sdfRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;

            ComPtr<ID3DBlob> sdfSig, sdfErr;
            hr = D3D12SerializeRootSignature(&sdfRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sdfSig, &sdfErr);
            if (FAILED(hr)) {
                LogFailureMessage("RenderPipelineStateCache: SdfFluid SerializeRootSignature", hr);
                return false;
            }
            hr = device.CreateRootSignature(0, sdfSig->GetBufferPointer(), sdfSig->GetBufferSize(), m_sdfFluidRootSignature);
            if (FAILED(hr)) {
                LogFailureMessage("RenderPipelineStateCache: SdfFluid CreateRootSignature", hr);
                return false;
            }
        }

        // SdfFluid PSO: fullscreen triangle, no input layout, no depth test
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSdf = {};
        psoSdf.InputLayout          = { nullptr, 0 };
        psoSdf.pRootSignature       = m_sdfFluidRootSignature.Get();
        psoSdf.VS                   = { sdfFluidVS->GetBufferPointer(), sdfFluidVS->GetBufferSize() };
        psoSdf.PS                   = { sdfFluidPS->GetBufferPointer(), sdfFluidPS->GetBufferSize() };
        psoSdf.RasterizerState      = rast;
        psoSdf.BlendState           = blendDesc;
        psoSdf.DepthStencilState    = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoSdf.DepthStencilState.DepthEnable = FALSE;
        psoSdf.DepthStencilState.StencilEnable = FALSE;
        psoSdf.SampleMask           = UINT_MAX;
        psoSdf.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoSdf.NumRenderTargets     = 1;
        psoSdf.RTVFormats[0]        = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoSdf.SampleDesc.Count     = 1;
        psoSdf.DSVFormat            = DXGI_FORMAT_UNKNOWN; // no depth attachment needed
        if (!createPipelineState("SdfFluid", psoSdf, m_sdfFluidPipelineState)) { return false; }

        // --- RayMarch root signature (single root CBV at b0, pixel shader only) ---
        {
            D3D12_ROOT_PARAMETER rmParam = {};
            rmParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            rmParam.Descriptor.ShaderRegister = 0;
            rmParam.Descriptor.RegisterSpace  = 0;
            rmParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_ROOT_SIGNATURE_DESC rmRsDesc = {};
            rmRsDesc.NumParameters = 1;
            rmRsDesc.pParameters   = &rmParam;
            rmRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;

            ComPtr<ID3DBlob> rmSig, rmErr;
            hr = D3D12SerializeRootSignature(&rmRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rmSig, &rmErr);
            if (FAILED(hr)) {
                LogFailureMessage("RenderPipelineStateCache: RayMarch SerializeRootSignature", hr);
                return false;
            }
            hr = device.CreateRootSignature(0, rmSig->GetBufferPointer(), rmSig->GetBufferSize(), m_rayMarchRootSignature);
            if (FAILED(hr)) {
                LogFailureMessage("RenderPipelineStateCache: RayMarch CreateRootSignature", hr);
                return false;
            }
        }

        // RayMarch PSO: fullscreen triangle, no input layout, no depth test
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoRM = {};
        psoRM.InputLayout           = { nullptr, 0 };
        psoRM.pRootSignature        = m_rayMarchRootSignature.Get();
        psoRM.VS                    = { rayMarchVS->GetBufferPointer(), rayMarchVS->GetBufferSize() };
        psoRM.PS                    = { rayMarchPS->GetBufferPointer(), rayMarchPS->GetBufferSize() };
        psoRM.RasterizerState       = rast;
        psoRM.BlendState            = blendDesc;
        psoRM.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoRM.DepthStencilState.DepthEnable = FALSE;
        psoRM.DepthStencilState.StencilEnable = FALSE;
        psoRM.SampleMask            = UINT_MAX;
        psoRM.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoRM.NumRenderTargets      = 1;
        psoRM.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoRM.SampleDesc.Count      = 1;
        psoRM.DSVFormat             = DXGI_FORMAT_UNKNOWN;
        if (!createPipelineState("RayMarch", psoRM, m_rayMarchPipelineState)) { return false; }

        // --- VolumetricCloud root signature (single root CBV at b0, VS+PS visible) ---
        {
            D3D12_ROOT_PARAMETER cloudParam = {};
            cloudParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            cloudParam.Descriptor.ShaderRegister = 0;
            cloudParam.Descriptor.RegisterSpace  = 0;
            cloudParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // VS reads none, PS reads all

            D3D12_ROOT_SIGNATURE_DESC cloudRsDesc = {};
            cloudRsDesc.NumParameters = 1;
            cloudRsDesc.pParameters   = &cloudParam;
            cloudRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

            ComPtr<ID3DBlob> cloudSig, cloudErr;
            hr = D3D12SerializeRootSignature(&cloudRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &cloudSig, &cloudErr);
            if (FAILED(hr)) {
                LogFailureMessage("RenderPipelineStateCache: VolumetricCloud SerializeRootSignature", hr);
                return false;
            }
            hr = device.CreateRootSignature(0, cloudSig->GetBufferPointer(), cloudSig->GetBufferSize(),
                                             m_volumetricCloudRootSignature);
            if (FAILED(hr)) {
                LogFailureMessage("RenderPipelineStateCache: VolumetricCloud CreateRootSignature", hr);
                return false;
            }
        }

        // VolumetricCloud PSO:
        //   Fullscreen triangle, alpha blend, depth LESS_EQUAL with no write
        //   (so the triangle at NDC z=1 composites over sky pixels only).
        {
            D3D12_BLEND_DESC cloudBlend = {};
            cloudBlend.AlphaToCoverageEnable = FALSE;
            cloudBlend.IndependentBlendEnable = FALSE;
            auto& rt = cloudBlend.RenderTarget[0];
            rt.BlendEnable           = TRUE;
            rt.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOp               = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha         = D3D12_BLEND_ONE;
            rt.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

            D3D12_DEPTH_STENCIL_DESC cloudDS = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            cloudDS.DepthEnable    = TRUE;
            cloudDS.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;  // no depth write
            cloudDS.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            cloudDS.StencilEnable  = FALSE;

            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoCloud = {};
            psoCloud.InputLayout          = { nullptr, 0 };
            psoCloud.pRootSignature       = m_volumetricCloudRootSignature.Get();
            psoCloud.VS                   = { volumetricCloudVS->GetBufferPointer(), volumetricCloudVS->GetBufferSize() };
            psoCloud.PS                   = { volumetricCloudPS->GetBufferPointer(), volumetricCloudPS->GetBufferSize() };
            psoCloud.RasterizerState      = rast;
            psoCloud.BlendState           = cloudBlend;
            psoCloud.DepthStencilState    = cloudDS;
            psoCloud.SampleMask           = UINT_MAX;
            psoCloud.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            psoCloud.NumRenderTargets     = 1;
            psoCloud.RTVFormats[0]        = DXGI_FORMAT_R8G8B8A8_UNORM; // SceneColor
            psoCloud.SampleDesc.Count     = 1;
            psoCloud.DSVFormat            = DXGI_FORMAT_D32_FLOAT;
            if (!createPipelineState("VolumetricCloud", psoCloud, m_volumetricCloudPipelineState)) {
                return false;
            }
        }

        // --- SSAO root signature ---
        // [0] t0 - depth SRV (PS only, descriptor table)
        // [1] t1 - normal SRV (PS only, descriptor table)
        // [2] b0 - SSAO constant buffer (all stages, root CBV)
        // Static sampler: point-clamp at s0 (PS only)
        D3D12_DESCRIPTOR_RANGE ssaoDepthRange{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 0,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        D3D12_DESCRIPTOR_RANGE ssaoNormalRange{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 1,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_ROOT_PARAMETER ssaoRootParams[3] = {};
        ssaoRootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        ssaoRootParams[0].DescriptorTable.NumDescriptorRanges = 1;
        ssaoRootParams[0].DescriptorTable.pDescriptorRanges = &ssaoDepthRange;
        ssaoRootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        ssaoRootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        ssaoRootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        ssaoRootParams[1].DescriptorTable.pDescriptorRanges = &ssaoNormalRange;
        ssaoRootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        ssaoRootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        ssaoRootParams[2].Descriptor.ShaderRegister = 0; // b0
        ssaoRootParams[2].Descriptor.RegisterSpace = 0;
        ssaoRootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC ssaoSamplerDesc = {};
        ssaoSamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        ssaoSamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ssaoSamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ssaoSamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ssaoSamplerDesc.ShaderRegister = 0;
        ssaoSamplerDesc.RegisterSpace = 0;
        ssaoSamplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC ssaoRootSigDesc = {};
        ssaoRootSigDesc.NumParameters = 3;
        ssaoRootSigDesc.pParameters = ssaoRootParams;
        ssaoRootSigDesc.NumStaticSamplers = 1;
        ssaoRootSigDesc.pStaticSamplers = &ssaoSamplerDesc;
        ssaoRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        {
            ComPtr<ID3DBlob> ssaoSig;
            ComPtr<ID3DBlob> ssaoSigErr;
            hr = D3D12SerializeRootSignature(&ssaoRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &ssaoSig, &ssaoSigErr);
            if (FAILED(hr)) {
                if (ssaoSigErr && ssaoSigErr->GetBufferPointer() && ssaoSigErr->GetBufferSize() > 0) {
                    DebugLog(static_cast<const char*>(ssaoSigErr->GetBufferPointer()));
                    DebugLog("\n");
                }
                LogFailureMessage("RenderPipelineStateCache::Initialize: SSAO D3D12SerializeRootSignature", hr);
                return false;
            }
            hr = device.CreateRootSignature(0, ssaoSig->GetBufferPointer(), ssaoSig->GetBufferSize(),
                                            m_ssaoRootSignature);
            if (FAILED(hr)) {
                LogFailureMessage("RenderPipelineStateCache::Initialize: SSAO CreateRootSignature", hr);
                return false;
            }
        }

        // --- SSAO PSO ---
        D3D12_GRAPHICS_PIPELINE_STATE_DESC ssaoPsoDesc = {};
        ssaoPsoDesc.pRootSignature = m_ssaoRootSignature.Get();
        ssaoPsoDesc.VS = { ssaoVS->GetBufferPointer(), ssaoVS->GetBufferSize() };
        ssaoPsoDesc.PS = { ssaoPS->GetBufferPointer(), ssaoPS->GetBufferSize() };
        ssaoPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        ssaoPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        ssaoPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        ssaoPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        ssaoPsoDesc.DepthStencilState.DepthEnable = FALSE;
        ssaoPsoDesc.DepthStencilState.StencilEnable = FALSE;
        ssaoPsoDesc.InputLayout = { nullptr, 0 };  // no vertex buffer
        ssaoPsoDesc.SampleMask = UINT_MAX;
        ssaoPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        ssaoPsoDesc.NumRenderTargets = 1;
        ssaoPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        ssaoPsoDesc.SampleDesc.Count = 1;
        // No DSVFormat (depth writes disabled)

        if (!createPipelineState("SSAO", ssaoPsoDesc, m_ssaoPipelineState)) { return false; }

        // --- SSAO Blur root signature ---
        // [0] t0 - raw SSAO SRV (PS only, descriptor table)
        // [1] t1 - depth SRV   (PS only, descriptor table)
        // [2] b0 - SSAO CB     (PS only, root CBV) — reuses same CB as SSAO pass
        // Static sampler: point-clamp at s0 (PS only)
        D3D12_DESCRIPTOR_RANGE blurSsaoRange{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 0, // t0
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        D3D12_DESCRIPTOR_RANGE blurDepthRange{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 1, // t1
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        D3D12_DESCRIPTOR_RANGE blurNormalRange{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 2, // t2
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_ROOT_PARAMETER blurRootParams[4] = {};
        blurRootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        blurRootParams[0].DescriptorTable.NumDescriptorRanges = 1;
        blurRootParams[0].DescriptorTable.pDescriptorRanges = &blurSsaoRange;
        blurRootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        blurRootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        blurRootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        blurRootParams[1].DescriptorTable.pDescriptorRanges = &blurDepthRange;
        blurRootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        blurRootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        blurRootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        blurRootParams[2].DescriptorTable.pDescriptorRanges = &blurNormalRange;
        blurRootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        blurRootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        blurRootParams[3].Descriptor.ShaderRegister = 0; // b0
        blurRootParams[3].Descriptor.RegisterSpace = 0;
        blurRootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC blurSamplerDesc = {};
        blurSamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        blurSamplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        blurSamplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        blurSamplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        blurSamplerDesc.ShaderRegister = 0;
        blurSamplerDesc.RegisterSpace = 0;
        blurSamplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC blurRootSigDesc = {};
        blurRootSigDesc.NumParameters = 4;
        blurRootSigDesc.pParameters = blurRootParams;
        blurRootSigDesc.NumStaticSamplers = 1;
        blurRootSigDesc.pStaticSamplers = &blurSamplerDesc;
        blurRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        {
            ComPtr<ID3DBlob> blurSig;
            ComPtr<ID3DBlob> blurSigErr;
            hr = D3D12SerializeRootSignature(&blurRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &blurSig, &blurSigErr);
            if (FAILED(hr)) {
                if (blurSigErr && blurSigErr->GetBufferPointer() && blurSigErr->GetBufferSize() > 0) {
                    DebugLog(static_cast<const char*>(blurSigErr->GetBufferPointer()));
                    DebugLog("\n");
                }
                LogFailureMessage("RenderPipelineStateCache::Initialize: SSAO Blur D3D12SerializeRootSignature", hr);
                return false;
            }
            hr = device.CreateRootSignature(0, blurSig->GetBufferPointer(), blurSig->GetBufferSize(),
                                            m_ssaoBlurRootSignature);
            if (FAILED(hr)) {
                LogFailureMessage("RenderPipelineStateCache::Initialize: SSAO Blur CreateRootSignature", hr);
                return false;
            }
        }

        // --- SSAO Blur PSO ---
        D3D12_GRAPHICS_PIPELINE_STATE_DESC blurPsoDesc = {};
        blurPsoDesc.pRootSignature = m_ssaoBlurRootSignature.Get();
        blurPsoDesc.VS = { ssaoVS->GetBufferPointer(), ssaoVS->GetBufferSize() }; // reuse same VS
        blurPsoDesc.PS = { ssaoBlurPS->GetBufferPointer(), ssaoBlurPS->GetBufferSize() };
        blurPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        blurPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        blurPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        blurPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        blurPsoDesc.DepthStencilState.DepthEnable = FALSE;
        blurPsoDesc.DepthStencilState.StencilEnable = FALSE;
        blurPsoDesc.InputLayout = { nullptr, 0 };
        blurPsoDesc.SampleMask = UINT_MAX;
        blurPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        blurPsoDesc.NumRenderTargets = 1;
        blurPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        blurPsoDesc.SampleDesc.Count = 1;

        if (!createPipelineState("SSAOBlur", blurPsoDesc, m_ssaoBlurPipelineState)) { return false; }

        // Mesh shader pipeline — optional, requires D3D12 Mesh Shader Tier 1
        InitializeMeshShaderPipeline(device, shaderModel);

        return true;
    }

    // -------------------------------------------------------------------------
    // Mesh Shader pipeline (AS + MS + PS)
    // -------------------------------------------------------------------------
    // Pipeline stream subobject helpers for CreatePipelineState
    namespace
    {
        // D3D12_PIPELINE_STATE_SUBOBJECT_TYPE wrappers to build a stream desc
        template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE SubobjectType, typename InnerType>
        struct alignas(void*) PsoSubobject
        {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type  = SubobjectType;
            InnerType                           inner = {};
        };

        using PsoRootSignature    = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,    ID3D12RootSignature*>;
        using PsoAS               = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS,                D3D12_SHADER_BYTECODE>;
        using PsoMS               = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS,                D3D12_SHADER_BYTECODE>;
        using PsoPS               = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,                D3D12_SHADER_BYTECODE>;
        using PsoBlend            = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,             D3D12_BLEND_DESC>;
        using PsoRasterizer       = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,        D3D12_RASTERIZER_DESC>;
        using PsoDepthStencil     = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL,     D3D12_DEPTH_STENCIL_DESC>;
        using PsoRenderTargets    = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY>;
        using PsoDepthStencilFmt  = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT>;
        using PsoSampleMask       = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,       UINT>;
        using PsoSampleDesc       = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,       DXGI_SAMPLE_DESC>;

        struct MeshShaderPipelineStream
        {
            PsoRootSignature   rootSignature;
            PsoAS              as;
            PsoMS              ms;
            PsoPS              ps;
            PsoBlend           blend;
            PsoRasterizer      rasterizer;
            PsoDepthStencil    depthStencil;
            PsoRenderTargets   renderTargets;
            PsoDepthStencilFmt dsvFormat;
            PsoSampleMask      sampleMask;
            PsoSampleDesc      sampleDesc;
        };
    }

    bool RenderPipelineStateCache::InitializeMeshShaderPipeline(GraphicsDevice& device,
                                                                 const std::string& shaderModel)
    {
        ID3D12Device* nativeDevice = device.GetDevice();
        if (!nativeDevice) {
            return false;
        }

        // Feature check: require Mesh Shader Tier 1
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7{};
        if (FAILED(nativeDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
                                                      &opts7, sizeof(opts7))) ||
            opts7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
            DebugLog("RenderPipelineStateCache: Mesh Shader Tier not supported on this GPU. "
                     "MeshShader pipeline will be disabled.\n");
            return true; // non-fatal
        }

        DebugLog("RenderPipelineStateCache: Mesh Shader Tier 1 supported. "
                 "Initialising MeshShader pipeline.\n");

        // Mesh/Amplification shaders require SM 6.5 minimum.
        // The Mesh Shader Tier check above already verified HW support, so it
        // is safe to target 6_5 regardless of the device's reported shader model.
        const std::string meshSmVersion = "6_5";
        const std::string asProfile = "as_" + meshSmVersion;
        const std::string msProfile = "ms_" + meshSmVersion;
        const std::string psProfile = "ps_" + shaderModel;

        ComPtr<ID3DBlob> asBlob, msBlob, psBlob;
        const std::array<ShaderSpec, 3> meshShaderSpecs{ {
            { L"MeshShader/MeshShader_AS.hlsl",    "AS_Meshlet", asProfile.c_str(),  &asBlob },
            { L"MeshShader/MeshShader_MS.hlsl",    "MS_Meshlet", msProfile.c_str(),  &msBlob },
            { L"CookTorranceGGX_PS.hlsl",          "PSMain",     psProfile.c_str(),  &psBlob },
        } };

        // loadOrCompileShader is captured from the outer scope via the lambda
        // We replicate the logic inline here since the lambda is local to Initialize().
        for (const ShaderSpec& spec : meshShaderSpecs)
        {
            const std::filesystem::path sourcePath   = ResolveShaderPath(spec.sourceRelativePath);
            const std::filesystem::path compiledPath = ResolveCompiledShaderPath(sourcePath, spec.entry, spec.target);
            spec.output->Reset();

            bool loaded = false;
            if (IsCompiledShaderUpToDate(compiledPath, sourcePath))
            {
                HRESULT rhr = D3DReadFileToBlob(compiledPath.c_str(), spec.output->GetAddressOf());
                if (SUCCEEDED(rhr)) {
                    LogShaderResolveMessage(sourcePath, spec.entry, spec.target,
                                            "loaded precompiled shader", compiledPath);
                    loaded = true;
                }
            }

            if (!loaded)
            {
                LogShaderResolveMessage(sourcePath, spec.entry, spec.target,
                                        "precompiled shader missing or stale, runtime compiling", compiledPath);
                if (!CompileShaderWithDxc(sourcePath, spec.entry, spec.target, *spec.output))
                {
                    std::string msg = "RenderPipelineStateCache: MeshShader compile failed: ";
                    msg += sourcePath.string();
                    msg += "\n";
                    DebugLog(msg.c_str());
                    return false;
                }

                std::error_code ec;
                std::filesystem::create_directories(compiledPath.parent_path(), ec);
                if (!ec) {
                    D3DWriteBlobToFile(spec.output->Get(), compiledPath.c_str(), TRUE);
                }
            }
        }

        // --- Root Signature ---
        // [0] t0 - MeshletDesc SRV   (AS + MS, inline root SRV)
        // [1] t1 - Vertex SRV        (MS, inline root SRV)
        // [2] t2 - MeshletIndex SRV  (MS, inline root SRV)
        // [3]    - DrawCB 32-bit constants (AS + MS) : 34 uint32 = model(16) + invModel(16) + offset + count + 2 pads
        // [4] b0 - CameraCB          (AS + MS, root CBV)
        // Static sampler s0 (PS, linear wrap) - reuse same as main pipeline

        D3D12_ROOT_PARAMETER msRootParams[5] = {};

        // [0] t0 - meshlet desc SRV (inline)
        msRootParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        msRootParams[0].Descriptor.ShaderRegister = 0; // t0
        msRootParams[0].Descriptor.RegisterSpace  = 0;
        msRootParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL; // AS + MS

        // [1] t1 - vertex buffer SRV (inline)
        msRootParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        msRootParams[1].Descriptor.ShaderRegister = 1; // t1
        msRootParams[1].Descriptor.RegisterSpace  = 0;
        msRootParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_MESH;

        // [2] t2 - meshlet index SRV (inline)
        msRootParams[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        msRootParams[2].Descriptor.ShaderRegister = 2; // t2
        msRootParams[2].Descriptor.RegisterSpace  = 0;
        msRootParams[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_MESH;

        // [3] DrawCB as 32-bit constants (model+invModel = 32 floats, meshletOffset, meshletCount, 2 pads)
        msRootParams[3].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        msRootParams[3].Constants.ShaderRegister = 1; // b1
        msRootParams[3].Constants.RegisterSpace  = 0;
        msRootParams[3].Constants.Num32BitValues = 34; // 16 + 16 + 2 + 2 pads (34 total)
        msRootParams[3].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        // [4] b0 CameraCB (inline root CBV)
        msRootParams[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        msRootParams[4].Descriptor.ShaderRegister = 0; // b0
        msRootParams[4].Descriptor.RegisterSpace  = 0;
        msRootParams[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC msSampler = {};
        msSampler.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        msSampler.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        msSampler.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        msSampler.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        msSampler.ShaderRegister = 0;
        msSampler.RegisterSpace  = 0;
        msSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC msRsDesc = {};
        msRsDesc.NumParameters    = 5;
        msRsDesc.pParameters      = msRootParams;
        msRsDesc.NumStaticSamplers = 1;
        msRsDesc.pStaticSamplers  = &msSampler;
        // No IA flag since mesh shaders don't use input assembler
        msRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                         D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                         D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                         D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ComPtr<ID3DBlob> msSig, msSigErr;
        HRESULT hr = D3D12SerializeRootSignature(&msRsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &msSig, &msSigErr);
        if (FAILED(hr)) {
            if (msSigErr && msSigErr->GetBufferSize() > 0) {
                DebugLog(static_cast<const char*>(msSigErr->GetBufferPointer()));
                DebugLog("\n");
            }
            LogFailureMessage("RenderPipelineStateCache: MeshShader SerializeRootSignature", hr);
            return false;
        }

        hr = device.CreateRootSignature(0, msSig->GetBufferPointer(), msSig->GetBufferSize(),
                                         m_meshShaderRootSignature);
        if (FAILED(hr)) {
            LogFailureMessage("RenderPipelineStateCache: MeshShader CreateRootSignature", hr);
            return false;
        }

        // --- Pipeline State Stream ---
        MeshShaderPipelineStream stream{};
        stream.rootSignature.inner = m_meshShaderRootSignature.Get();
        stream.as.inner            = { asBlob->GetBufferPointer(), asBlob->GetBufferSize() };
        stream.ms.inner            = { msBlob->GetBufferPointer(), msBlob->GetBufferSize() };
        stream.ps.inner            = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };

        auto blendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        blendState.RenderTarget[0].BlendEnable          = FALSE;
        blendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        stream.blend.inner = blendState;

        auto rastState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        rastState.CullMode = D3D12_CULL_MODE_BACK;
        stream.rasterizer.inner = rastState;

        auto dsState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        dsState.DepthEnable    = TRUE;
        dsState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        dsState.StencilEnable  = FALSE;
        stream.depthStencil.inner = dsState;

        D3D12_RT_FORMAT_ARRAY rtFormats{};
        rtFormats.NumRenderTargets = 5;
        rtFormats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;     // SceneColor
        rtFormats.RTFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;     // GBufferAlbedo
        rtFormats.RTFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT; // GBufferNormal
        rtFormats.RTFormats[3] = DXGI_FORMAT_R8G8B8A8_UNORM;     // GBufferMaterial
        rtFormats.RTFormats[4] = DXGI_FORMAT_R16G16B16A16_FLOAT; // GBufferEmissive
        stream.renderTargets.inner = rtFormats;

        stream.dsvFormat.inner = DXGI_FORMAT_D32_FLOAT;
        stream.sampleMask.inner = UINT_MAX;

        DXGI_SAMPLE_DESC sd{}; sd.Count = 1; sd.Quality = 0;
        stream.sampleDesc.inner = sd;

        hr = device.CreatePipelineStateFromStream(&stream, sizeof(MeshShaderPipelineStream),
                                                   m_meshShaderPipelineState);
        if (FAILED(hr)) {
            LogFailureMessage("RenderPipelineStateCache: MeshShader CreatePipelineState", hr);
            return false;
        }

        // Note: m_meshletDebugPipelineState is a VS+PS pipeline created in Initialize()
        // using SV_PrimitiveID/16 for per-meshlet colour; it does not require mesh shaders.

        DebugLog("RenderPipelineStateCache: MeshShader pipeline initialised successfully.\n");
        return true;
    }
}
