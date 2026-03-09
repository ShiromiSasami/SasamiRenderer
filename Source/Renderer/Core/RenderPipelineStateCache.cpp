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
        // [4]=light SRVs (t2-t3), [5]=IBL SRVs (t4-t6), [6]=AO SRV (t7)
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

        D3D12_ROOT_PARAMETER rootParams[7] = {};
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

        D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.ShaderRegister = 0;
        samplerDesc.RegisterSpace = 0;
        samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.NumParameters = 7;
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
        ComPtr<ID3DBlob> tessVS, tessHS, tessDS, tessGS;
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
        const std::array<ShaderSpec, 11> shaderSpecs{ {
            { L"PBR_VS.hlsl", "VSMain", vertexProfile.c_str(), &vertexShader },
            { L"PBR_PS.hlsl", "PSMain", pixelProfile.c_str(), &pixelShader },
            { L"Opaque_VS.hlsl", "VSMain", vertexProfile.c_str(), &basicVertexShader },
            { L"Opaque_PS.hlsl", "PSMain", pixelProfile.c_str(), &basicPixelShader },
            { L"Skybox/Skybox_VS.hlsl", "VSMain", vertexProfile.c_str(), &skyboxVS },
            { L"Skybox/Skybox_HDR_PS.hlsl", "PSMain", pixelProfile.c_str(), &skyboxHdrPS },
            { L"Skybox/Skybox_PS.hlsl", "PSMain", pixelProfile.c_str(), &skyboxLdrPS },
            { L"Tessellation_VS.hlsl", "VSMain", vertexProfile.c_str(), &tessVS },
            { L"Tessellation_HS.hlsl", "HSMain", hullProfile.c_str(), &tessHS },
            { L"Tessellation_DS.hlsl", "DSMain", domainProfile.c_str(), &tessDS },
            { L"Tessellation_GS.hlsl", "GSMain", geometryProfile.c_str(), &tessGS },
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
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
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

        if (!createPipelineState("PBR", psoDesc, m_pipelineState)) { return false; }

        // Opaque raster pipeline (unlit opaque draw path)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoBasic = psoDesc;
        psoBasic.VS = { basicVertexShader->GetBufferPointer(), basicVertexShader->GetBufferSize() };
        psoBasic.PS = { basicPixelShader->GetBufferPointer(), basicPixelShader->GetBufferSize() };
        if (!createPipelineState("Opaque", psoBasic, m_basicPipelineState)) { return false; }

        // Shadow pipeline (depth-only, reuse VS; no PS)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoShadow = {};
        psoShadow.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoShadow.pRootSignature = m_rootSignature.Get();
        psoShadow.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
        psoShadow.PS = { nullptr, 0 };
        psoShadow.RasterizerState = rast;
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
        rastTess.CullMode = D3D12_CULL_MODE_BACK; // optimize overfill
        psoTess.RasterizerState = rastTess;
        psoTess.BlendState = blendDesc;
        psoTess.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoTess.DepthStencilState.DepthEnable = TRUE;
        psoTess.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        psoTess.DepthStencilState.StencilEnable = FALSE;
        psoTess.SampleMask = UINT_MAX;
        psoTess.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        psoTess.NumRenderTargets = 1;
        psoTess.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoTess.SampleDesc.Count = 1;
        psoTess.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        if (!createPipelineState("Tessellation", psoTess, m_tessPipelineState)) { return false; }

        // Tessellation shadow pipeline (VS+HS+DS; depth-only)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTessShadow = {};
        psoTessShadow.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoTessShadow.pRootSignature = m_rootSignature.Get();
        psoTessShadow.VS = { tessVS->GetBufferPointer(), tessVS->GetBufferSize() };
        psoTessShadow.HS = { tessHS->GetBufferPointer(), tessHS->GetBufferSize() };
        psoTessShadow.DS = { tessDS->GetBufferPointer(), tessDS->GetBufferSize() };
        psoTessShadow.GS = { nullptr, 0 };
        psoTessShadow.PS = { nullptr, 0 };
        psoTessShadow.RasterizerState = rastTess;
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

        return true;
    }
}
