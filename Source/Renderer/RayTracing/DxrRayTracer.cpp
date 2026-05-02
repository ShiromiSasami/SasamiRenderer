#include "Renderer/RayTracing/DxrRayTracer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <dxcapi.h>
#include <windows.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "Foundation/Math/MathUtil.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "d3dx12.h"

namespace SasamiRenderer
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
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

        static std::filesystem::path GetBundledDxcRoot()
        {
            const std::filesystem::path projectRoot = FindProjectRootWithShaders(GetExecutableDir());
            if (!projectRoot.empty()) {
                return projectRoot / L"Tools" / L"DXC" / L"bin" / L"x64";
            }
            return GetExecutableDir() / L"Tools" / L"DXC" / L"bin" / L"x64";
        }

        static HRESULT CreateDxcInstance(REFCLSID clsid, REFIID iid, void** outObject)
        {
            static HMODULE dxcompilerModule = []() -> HMODULE {
                const std::filesystem::path bundledDllPath = GetBundledDxcRoot() / L"dxcompiler.dll";
                if (HMODULE module = LoadLibraryW(bundledDllPath.c_str())) {
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

        static bool CompileShaderLibrary(const std::filesystem::path& shaderPath, ComPtr<IDxcBlob>& outLibraryBlob)
        {
            ComPtr<IDxcUtils> utils;
            ComPtr<IDxcCompiler3> compiler;
            if (FAILED(CreateDxcInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
                FAILED(CreateDxcInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
                DebugLog("DxrRayTracer: failed to create DXC compiler.\n");
                return false;
            }

            ComPtr<IDxcBlobEncoding> sourceBlob;
            if (FAILED(utils->LoadFile(shaderPath.c_str(), nullptr, &sourceBlob))) {
                DebugLog("DxrRayTracer: failed to load ray tracing shader library.\n");
                return false;
            }

            ComPtr<IDxcIncludeHandler> includeHandler;
            if (FAILED(utils->CreateDefaultIncludeHandler(&includeHandler))) {
                DebugLog("DxrRayTracer: failed to create DXC include handler.\n");
                return false;
            }

            const std::wstring shaderSource = shaderPath.native();
            const std::wstring shaderRoot = GetShaderSourceRoot().native();
            std::vector<LPCWSTR> arguments{
                shaderSource.c_str(),
                L"-T", L"lib_6_6",
                L"-I", shaderRoot.c_str(),
                L"-WX",
                L"-HV", L"2021"
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
            if (FAILED(compiler->Compile(&sourceBuffer,
                                         arguments.data(),
                                         static_cast<UINT32>(arguments.size()),
                                         includeHandler.Get(),
                                         IID_PPV_ARGS(&result))) ||
                !result) {
                DebugLog("DxrRayTracer: DXC compile invocation failed.\n");
                return false;
            }

            ComPtr<IDxcBlobUtf8> errors;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
                errors &&
                errors->GetStringLength() > 0) {
                DebugLog(errors->GetStringPointer());
                DebugLog("\n");
            }

            HRESULT status = S_OK;
            if (FAILED(result->GetStatus(&status)) || FAILED(status)) {
                DebugLog("DxrRayTracer: shader library compilation failed.\n");
                return false;
            }

            if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&outLibraryBlob), nullptr)) ||
                !outLibraryBlob) {
                DebugLog("DxrRayTracer: missing DXIL object for shader library.\n");
                return false;
            }

            return true;
        }

        static UINT AlignUp(UINT value, UINT alignment)
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }

        static bool CreateBuffer(IRHIDevice& device,
                                 UINT64 size,
                                 D3D12_RESOURCE_FLAGS flags,
                                 D3D12_RESOURCE_STATES initialState,
                                 Resource& outResource)
        {
            D3D12_HEAP_PROPERTIES heapProperties{};
            heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC resourceDesc{};
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resourceDesc.Width = size;
            resourceDesc.Height = 1;
            resourceDesc.DepthOrArraySize = 1;
            resourceDesc.MipLevels = 1;
            resourceDesc.SampleDesc.Count = 1;
            resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            resourceDesc.Flags = flags;

            return SUCCEEDED(device.CreateCommittedResource(&heapProperties,
                                                            D3D12_HEAP_FLAG_NONE,
                                                            &resourceDesc,
                                                            initialState,
                                                            nullptr,
                                                            outResource));
        }

        static void WriteShaderIdentifierRecord(uint8_t* destination, ID3D12StateObjectProperties* properties, LPCWSTR exportName)
        {
            const void* shaderIdentifier = properties ? properties->GetShaderIdentifier(exportName) : nullptr;
            if (shaderIdentifier) {
                std::memcpy(destination, shaderIdentifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
            } else {
                std::memset(destination, 0, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
            }
        }

        static void CopyBufferData(IRHIDevice& device,
                                   const void* sourceData,
                                   UINT64 sourceSize,
                                   Resource& destination,
                                   D3D12_RESOURCE_STATES destinationState)
        {
            if (!sourceData || sourceSize == 0u || !destination.IsValid()) {
                return;
            }

            CommandAllocator allocator;
            CommandList commandList;
            if (FAILED(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, allocator)) ||
                FAILED(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, commandList))) {
                return;
            }

            Resource uploadBuffer;
            void* mappedPtr = nullptr;
            if (!ResourceUploadUtility::CreateUploadBuffer(device, sourceSize, uploadBuffer, &mappedPtr)) {
                return;
            }
            std::memcpy(mappedPtr, sourceData, static_cast<size_t>(sourceSize));
            uploadBuffer->Unmap(0, nullptr);

            commandList.CopyBufferRegion(destination, 0u, uploadBuffer, 0u, sourceSize);
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(destination.Get(),
                                                                      D3D12_RESOURCE_STATE_COPY_DEST,
                                                                      destinationState);
            commandList.ResourceBarrier(1u, &barrier);
            commandList.Close();
            ID3D12CommandList* commandLists[] = { commandList.Get() };
            device.GetCommandQueue()->ExecuteCommandLists(1u, commandLists);
            device.WaitForGPU();
        }

        static bool CreateTextureUav(ID3D12Device* device, Resource& texture, CpuDescriptorHandle destination)
        {
            if (!device || !texture.IsValid()) {
                return false;
            }

            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(texture.Get(), nullptr, &desc, destination);
            return true;
        }

        static void ConvertToDxrTransform(const float rowMajorRowVector[16], float outTransform[3][4])
        {
            // Engine matrices are row-major with row-vector convention.
            // DXR instance desc expects a row-major 3x4 matrix in standard affine form,
            // so transpose and keep the upper 3 rows.
            const float transposed[16] = {
                rowMajorRowVector[0], rowMajorRowVector[4], rowMajorRowVector[8],  rowMajorRowVector[12],
                rowMajorRowVector[1], rowMajorRowVector[5], rowMajorRowVector[9],  rowMajorRowVector[13],
                rowMajorRowVector[2], rowMajorRowVector[6], rowMajorRowVector[10], rowMajorRowVector[14],
                rowMajorRowVector[3], rowMajorRowVector[7], rowMajorRowVector[11], rowMajorRowVector[15],
            };

            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 4; ++column) {
                    outTransform[row][column] = transposed[row * 4 + column];
                }
            }
        }
    }

    bool DxrRayTracer::Initialize(IRHIDevice& device, const DescriptorSet& descriptors)
    {
        m_descriptors = descriptors;
        m_supported = device.SupportsHardwareRayTracing();
        if (!m_supported) {
            return true;
        }

        if (!CompileShadersAndCreateStateObject(device)) {
            m_supported = false;
            return false;
        }
        if (!CreateShaderTables(device)) {
            m_supported = false;
            return false;
        }
        if (!EnsureFrameConstantBuffer(device)) {
            m_supported = false;
            return false;
        }

        return true;
    }

    void DxrRayTracer::UpdateScene(IRHIDevice& device, const RayTracingScene& scene)
    {
        const bool geometryChanged = scene.geometryVersion != m_uploadedGeometryVersion;
        const bool materialChanged = scene.materialVersion != m_uploadedMaterialVersion;
        const bool instanceChanged = scene.instanceVersion != m_uploadedInstanceVersion;
        m_scene = scene;
        m_sceneDirty = geometryChanged || instanceChanged;
        if (!m_supported) {
            return;
        }

        if (!geometryChanged && !materialChanged && !instanceChanged) {
            return;
        }

        const auto buildStartTime = std::chrono::high_resolution_clock::now();
        if (!UploadSceneBuffers(device)) {
            DebugLog("DxrRayTracer::UpdateScene: failed to upload scene buffers.\n");
            return;
        }
        if ((geometryChanged || instanceChanged) && !BuildAccelerationStructures(device)) {
            DebugLog("DxrRayTracer::UpdateScene: failed to build acceleration structures.\n");
            return;
        }
        const auto buildEndTime = std::chrono::high_resolution_clock::now();
        m_lastSceneBuildMs = std::chrono::duration<float, std::milli>(buildEndTime - buildStartTime).count();
        m_reportSceneBuildCost = true;
        m_uploadedGeometryVersion = scene.geometryVersion;
        m_uploadedMaterialVersion = scene.materialVersion;
        m_uploadedInstanceVersion = scene.instanceVersion;
        m_sceneDirty = false;
    }

    bool DxrRayTracer::EnsureFrameConstantBuffer(IRHIDevice& device)
    {
        if (m_frameConstantsBuffer.IsValid() && m_frameConstantsPtr) {
            return true;
        }

        return ResourceUploadUtility::CreateUploadBuffer(device,
                                                         (sizeof(FrameConstants) + 255u) & ~255u,
                                                         m_frameConstantsBuffer,
                                                         reinterpret_cast<void**>(&m_frameConstantsPtr));
    }

    void DxrRayTracer::FillFrameConstants(const RayTracingFrameDesc& frameDesc, FrameConstants& outConstants) const
    {
        std::memset(&outConstants, 0, sizeof(outConstants));
        outConstants.renderWidth = frameDesc.renderWidth;
        outConstants.renderHeight = frameDesc.renderHeight;
        outConstants.outputWidth = frameDesc.width;
        outConstants.outputHeight = frameDesc.height;
        outConstants.outputDescriptorIndex = m_descriptors.outputDescriptorIndex;
        outConstants.vertexDescriptorIndex = m_descriptors.vertexDescriptorIndex;
        outConstants.indexDescriptorIndex = m_descriptors.indexDescriptorIndex;
        outConstants.materialDescriptorIndex = m_descriptors.materialDescriptorIndex;
        outConstants.instanceDescriptorIndex = m_descriptors.instanceDescriptorIndex;
        outConstants.pointLightBudget = frameDesc.pointLightBudget;
        outConstants.spotLightBudget = frameDesc.spotLightBudget;
        outConstants.qualityTier = frameDesc.qualityTier;
        outConstants.debugView = frameDesc.debugView;
        outConstants.flags = frameDesc.flags;
        outConstants.maxBounceCount = std::clamp(frameDesc.maxBounceCount,
                                                 kMinRayTracingBounceCount,
                                                 kMaxRayTracingBounceCount);
        outConstants.dynamicResolutionScale = frameDesc.dynamicResolutionScale;
        outConstants.cameraPosition[0] = frameDesc.cameraPosition[0];
        outConstants.cameraPosition[1] = frameDesc.cameraPosition[1];
        outConstants.cameraPosition[2] = frameDesc.cameraPosition[2];
        outConstants.cameraPosition[3] = 1.0f;
        std::memcpy(outConstants.inverseViewProjection,
                    frameDesc.inverseViewProjection,
                    sizeof(outConstants.inverseViewProjection));

        float forward[3] = {};
        Math::DirectionFromYawPitch(frameDesc.directionalLight.yaw,
                                    frameDesc.directionalLight.pitch,
                                    forward);
        outConstants.directionalLightDirection[0] = -forward[0];
        outConstants.directionalLightDirection[1] = -forward[1];
        outConstants.directionalLightDirection[2] = -forward[2];
        outConstants.directionalLightDirection[3] = 0.0f;

        outConstants.directionalLightColorIntensity[0] = frameDesc.directionalLight.color[0];
        outConstants.directionalLightColorIntensity[1] = frameDesc.directionalLight.color[1];
        outConstants.directionalLightColorIntensity[2] = frameDesc.directionalLight.color[2];
        outConstants.directionalLightColorIntensity[3] = frameDesc.directionalLight.intensity;
        outConstants.directionalLightMarkerParams[0] = frameDesc.directionalLightMarkerEnabled ? 1.0f : 0.0f;
        outConstants.directionalLightMarkerParams[1] = frameDesc.directionalLightMarkerAngularRadius;
        outConstants.directionalLightMarkerParams[2] = frameDesc.directionalLightMarkerHaloAngularRadius;
        outConstants.directionalLightMarkerParams[3] = frameDesc.directionalLightMarkerBrightness;

        if (frameDesc.pointLights) {
            outConstants.pointLightCount = std::min<uint32_t>(kMaxPointLights, static_cast<uint32_t>(frameDesc.pointLights->size()));
            for (uint32_t i = 0; i < outConstants.pointLightCount; ++i) {
                const RenderPointLight& light = (*frameDesc.pointLights)[i];
                outConstants.pointLights[i].posRange[0] = light.pos[0];
                outConstants.pointLights[i].posRange[1] = light.pos[1];
                outConstants.pointLights[i].posRange[2] = light.pos[2];
                outConstants.pointLights[i].posRange[3] = light.range;
                outConstants.pointLights[i].colorIntensity[0] = light.color[0];
                outConstants.pointLights[i].colorIntensity[1] = light.color[1];
                outConstants.pointLights[i].colorIntensity[2] = light.color[2];
                outConstants.pointLights[i].colorIntensity[3] = light.intensity;
            }
        }

        if (frameDesc.spotLights) {
            outConstants.spotLightCount = std::min<uint32_t>(kMaxSpotLights, static_cast<uint32_t>(frameDesc.spotLights->size()));
            for (uint32_t i = 0; i < outConstants.spotLightCount; ++i) {
                const RenderSpotLight& light = (*frameDesc.spotLights)[i];
                float direction[3] = {};
                Math::DirectionFromYawPitch(light.yaw, light.pitch, direction);
                outConstants.spotLights[i].posRange[0] = light.pos[0];
                outConstants.spotLights[i].posRange[1] = light.pos[1];
                outConstants.spotLights[i].posRange[2] = light.pos[2];
                outConstants.spotLights[i].posRange[3] = light.range;
                outConstants.spotLights[i].dirCosInner[0] = direction[0];
                outConstants.spotLights[i].dirCosInner[1] = direction[1];
                outConstants.spotLights[i].dirCosInner[2] = direction[2];
                outConstants.spotLights[i].dirCosInner[3] = std::cos(light.innerAngle);
                outConstants.spotLights[i].colorIntensity[0] = light.color[0];
                outConstants.spotLights[i].colorIntensity[1] = light.color[1];
                outConstants.spotLights[i].colorIntensity[2] = light.color[2];
                outConstants.spotLights[i].colorIntensity[3] = light.intensity;
                outConstants.spotLights[i].params[0] = std::cos(light.outerAngle);
            }
        }
    }

    bool DxrRayTracer::CompileShadersAndCreateStateObject(IRHIDevice& device)
    {
        if (!m_supported) {
            return false;
        }

        ID3D12Device5* dxrDevice = device.GetRayTracingDevice();
        if (!dxrDevice) {
            return false;
        }

        ComPtr<IDxcBlob> shaderLibrary;
        if (!CompileShaderLibrary(GetShaderSourceRoot() / L"RayTracing" / L"RayTracing.hlsl", shaderLibrary)) {
            return false;
        }

        D3D12_ROOT_PARAMETER rootParameters[2] = {};
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameters[0].Descriptor.ShaderRegister = 0;
        rootParameters[0].Descriptor.RegisterSpace = 0;
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParameters[1].Descriptor.ShaderRegister = 0;
        rootParameters[1].Descriptor.RegisterSpace = 0;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.ShaderRegister = 0;
        samplerDesc.RegisterSpace = 0;
        samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc{};
        rootSignatureDesc.NumParameters = _countof(rootParameters);
        rootSignatureDesc.pParameters = rootParameters;
        rootSignatureDesc.NumStaticSamplers = 1;
        rootSignatureDesc.pStaticSamplers = &samplerDesc;
        rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

        ComPtr<ID3DBlob> serializedRootSignature;
        ComPtr<ID3DBlob> rootSignatureErrors;
        if (FAILED(D3D12SerializeRootSignature(&rootSignatureDesc,
                                               D3D_ROOT_SIGNATURE_VERSION_1,
                                               &serializedRootSignature,
                                               &rootSignatureErrors))) {
            if (rootSignatureErrors && rootSignatureErrors->GetBufferPointer()) {
                DebugLog(static_cast<const char*>(rootSignatureErrors->GetBufferPointer()));
                DebugLog("\n");
            }
            return false;
        }

        if (FAILED(dxrDevice->CreateRootSignature(0,
                                                  serializedRootSignature->GetBufferPointer(),
                                                  serializedRootSignature->GetBufferSize(),
                                                  IID_PPV_ARGS(&m_globalRootSignature)))) {
            DebugLog("DxrRayTracer: failed to create global root signature.\n");
            return false;
        }

        D3D12_DXIL_LIBRARY_DESC dxilLibraryDesc{};
        D3D12_SHADER_BYTECODE shaderBytecode{};
        shaderBytecode.pShaderBytecode = shaderLibrary->GetBufferPointer();
        shaderBytecode.BytecodeLength = shaderLibrary->GetBufferSize();
        dxilLibraryDesc.DXILLibrary = shaderBytecode;

        D3D12_HIT_GROUP_DESC hitGroupDesc{};
        hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hitGroupDesc.ClosestHitShaderImport = L"ClosestHitShader";
        hitGroupDesc.HitGroupExport = L"SceneHitGroup";

        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
        shaderConfig.MaxPayloadSizeInBytes = 32u;
        shaderConfig.MaxAttributeSizeInBytes = D3D12_RAYTRACING_MAX_ATTRIBUTE_SIZE_IN_BYTES;

        LPCWSTR shaderExports[] = {
            L"RayGenShader",
            L"MissShader",
            L"ShadowMissShader",
            L"SceneHitGroup",
        };

        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION shaderConfigAssociation{};
        shaderConfigAssociation.NumExports = _countof(shaderExports);
        shaderConfigAssociation.pExports = shaderExports;

        D3D12_GLOBAL_ROOT_SIGNATURE globalRootSignature{};
        globalRootSignature.pGlobalRootSignature = m_globalRootSignature.Get();

        D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
        pipelineConfig.MaxTraceRecursionDepth = kMaxRayTracingBounceCount + 1u;

        D3D12_STATE_SUBOBJECT subobjects[6] = {};
        subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
        subobjects[0].pDesc = &dxilLibraryDesc;
        subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
        subobjects[1].pDesc = &hitGroupDesc;
        subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
        subobjects[2].pDesc = &shaderConfig;
        shaderConfigAssociation.pSubobjectToAssociate = &subobjects[2];
        subobjects[3].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
        subobjects[3].pDesc = &shaderConfigAssociation;
        subobjects[4].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
        subobjects[4].pDesc = &globalRootSignature;
        subobjects[5].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
        subobjects[5].pDesc = &pipelineConfig;

        D3D12_STATE_OBJECT_DESC stateObjectDesc{};
        stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        stateObjectDesc.NumSubobjects = _countof(subobjects);
        stateObjectDesc.pSubobjects = subobjects;

        if (FAILED(dxrDevice->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&m_stateObject))) ||
            FAILED(m_stateObject.As(&m_stateObjectProperties))) {
            DebugLog("DxrRayTracer: failed to create state object.\n");
            return false;
        }

        m_pipelineReady = true;
        return true;
    }

    bool DxrRayTracer::CreateShaderTables(IRHIDevice& device)
    {
        if (!m_pipelineReady || !m_stateObjectProperties) {
            return false;
        }

        m_rayGenShaderRecordSize = AlignUp(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
        m_missShaderRecordSize = m_rayGenShaderRecordSize;
        m_hitGroupShaderRecordSize = m_rayGenShaderRecordSize;

        const UINT rayGenTableSize = AlignUp(m_rayGenShaderRecordSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
        const UINT missTableSize = AlignUp(m_missShaderRecordSize * 2u, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
        const UINT hitGroupTableSize = AlignUp(m_hitGroupShaderRecordSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

        auto createShaderTable = [&device](UINT size, Resource& outResource, uint8_t*& outMappedPtr) -> bool {
            return ResourceUploadUtility::CreateUploadBuffer(device, size, outResource, reinterpret_cast<void**>(&outMappedPtr));
        };

        uint8_t* rayGenMapped = nullptr;
        uint8_t* missMapped = nullptr;
        uint8_t* hitGroupMapped = nullptr;
        if (!createShaderTable(rayGenTableSize, m_rayGenShaderTable, rayGenMapped) ||
            !createShaderTable(missTableSize, m_missShaderTable, missMapped) ||
            !createShaderTable(hitGroupTableSize, m_hitGroupShaderTable, hitGroupMapped)) {
            return false;
        }

        WriteShaderIdentifierRecord(rayGenMapped, m_stateObjectProperties.Get(), L"RayGenShader");
        WriteShaderIdentifierRecord(missMapped + m_missShaderRecordSize * 0u, m_stateObjectProperties.Get(), L"MissShader");
        WriteShaderIdentifierRecord(missMapped + m_missShaderRecordSize * 1u, m_stateObjectProperties.Get(), L"ShadowMissShader");
        WriteShaderIdentifierRecord(hitGroupMapped, m_stateObjectProperties.Get(), L"SceneHitGroup");

        return true;
    }

    bool DxrRayTracer::UploadSceneBuffers(IRHIDevice& device)
    {
        if (!m_supported) {
            return false;
        }

        std::vector<GpuVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<GpuMaterial> materials;
        std::vector<GpuInstance> instances;

        m_meshRecords.clear();
        m_meshRecords.reserve(m_scene.meshes.size());

        for (const RayTracingMaterial& material : m_scene.materials) {
            GpuMaterial gpuMaterial{};
            gpuMaterial.albedoDescriptorIndex = material.albedoDescriptorIndex;
            gpuMaterial.occlusionDescriptorIndex = material.occlusionDescriptorIndex;
            gpuMaterial.metallic = material.material.metallic;
            gpuMaterial.roughness = material.material.roughness;
            gpuMaterial.baseColor[0] = material.material.baseColor[0];
            gpuMaterial.baseColor[1] = material.material.baseColor[1];
            gpuMaterial.baseColor[2] = material.material.baseColor[2];
            gpuMaterial.baseColor[3] = material.material.baseColor[3];
            gpuMaterial.emissiveOcclusionStrength[0] = material.material.emissive[0];
            gpuMaterial.emissiveOcclusionStrength[1] = material.material.emissive[1];
            gpuMaterial.emissiveOcclusionStrength[2] = material.material.emissive[2];
            gpuMaterial.emissiveOcclusionStrength[3] = material.material.occlusionStrength;
            materials.push_back(gpuMaterial);
        }

        for (const RayTracingMesh& mesh : m_scene.meshes) {
            MeshRecord meshRecord{};
            meshRecord.vertexOffset = static_cast<uint32_t>(vertices.size());
            meshRecord.indexOffset = static_cast<uint32_t>(indices.size());
            meshRecord.vertexCount = static_cast<uint32_t>(mesh.mesh.vertices.size());
            meshRecord.indexCount = static_cast<uint32_t>(mesh.mesh.indices.empty()
                ? mesh.mesh.vertices.size()
                : mesh.mesh.indices.size());

            for (const Vertex& vertex : mesh.mesh.vertices) {
                GpuVertex gpuVertex{};
                gpuVertex.position[0] = vertex.position[0];
                gpuVertex.position[1] = vertex.position[1];
                gpuVertex.position[2] = vertex.position[2];
                gpuVertex.normal[0] = vertex.normal[0];
                gpuVertex.normal[1] = vertex.normal[1];
                gpuVertex.normal[2] = vertex.normal[2];
                gpuVertex.color[0] = vertex.color[0];
                gpuVertex.color[1] = vertex.color[1];
                gpuVertex.color[2] = vertex.color[2];
                gpuVertex.color[3] = vertex.color[3];
                gpuVertex.uv[0] = vertex.uv[0];
                gpuVertex.uv[1] = vertex.uv[1];
                vertices.push_back(gpuVertex);
            }

            if (mesh.mesh.indices.empty()) {
                for (uint32_t index = 0; index < mesh.mesh.vertices.size(); ++index) {
                    indices.push_back(index);
                }
            } else {
                for (uint32_t index : mesh.mesh.indices) {
                    indices.push_back(index);
                }
            }

            m_meshRecords.push_back(meshRecord);
        }

        for (const RayTracingInstance& instance : m_scene.instances) {
            GpuInstance gpuInstance{};
            if (instance.meshIndex >= m_meshRecords.size()) {
                continue;
            }
            gpuInstance.vertexOffset = m_meshRecords[instance.meshIndex].vertexOffset;
            gpuInstance.indexOffset = m_meshRecords[instance.meshIndex].indexOffset;
            gpuInstance.materialIndex = instance.materialIndex;
            instances.push_back(gpuInstance);
        }

        if (!vertices.empty()) {
            const UINT64 vertexBufferSize = static_cast<UINT64>(vertices.size()) * sizeof(GpuVertex);
            if (!CreateBuffer(device,
                              vertexBufferSize,
                              D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST,
                              m_vertexBuffer)) {
                return false;
            }
            CopyBufferData(device, vertices.data(), vertexBufferSize, m_vertexBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            m_vertexBuffer.Reset();
        }

        if (!indices.empty()) {
            const UINT64 indexBufferSize = static_cast<UINT64>(indices.size()) * sizeof(uint32_t);
            if (!CreateBuffer(device,
                              indexBufferSize,
                              D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST,
                              m_indexBuffer)) {
                return false;
            }
            CopyBufferData(device, indices.data(), indexBufferSize, m_indexBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            m_indexBuffer.Reset();
        }

        if (!materials.empty()) {
            const UINT64 materialBufferSize = static_cast<UINT64>(materials.size()) * sizeof(GpuMaterial);
            if (!CreateBuffer(device,
                              materialBufferSize,
                              D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST,
                              m_materialBuffer)) {
                return false;
            }
            CopyBufferData(device, materials.data(), materialBufferSize, m_materialBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            m_materialBuffer.Reset();
        }

        if (!instances.empty()) {
            const UINT64 instanceBufferSize = static_cast<UINT64>(instances.size()) * sizeof(GpuInstance);
            if (!CreateBuffer(device,
                              instanceBufferSize,
                              D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST,
                              m_instanceBuffer)) {
                return false;
            }
            CopyBufferData(device, instances.data(), instanceBufferSize, m_instanceBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            m_instanceBuffer.Reset();
        }

        ID3D12Device* nativeDevice = device.GetDevice();
        if (!nativeDevice) {
            return false;
        }

        if (m_vertexBuffer.IsValid()) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = static_cast<UINT>(vertices.size());
            srvDesc.Buffer.StructureByteStride = sizeof(GpuVertex);
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            nativeDevice->CreateShaderResourceView(m_vertexBuffer.Get(), &srvDesc, m_descriptors.vertexSrvCpu);
        }

        if (m_indexBuffer.IsValid()) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = static_cast<UINT>(indices.size());
            srvDesc.Buffer.StructureByteStride = sizeof(uint32_t);
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            nativeDevice->CreateShaderResourceView(m_indexBuffer.Get(), &srvDesc, m_descriptors.indexSrvCpu);
        }

        if (m_materialBuffer.IsValid()) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = static_cast<UINT>(materials.size());
            srvDesc.Buffer.StructureByteStride = sizeof(GpuMaterial);
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            nativeDevice->CreateShaderResourceView(m_materialBuffer.Get(), &srvDesc, m_descriptors.materialSrvCpu);
        }

        if (m_instanceBuffer.IsValid()) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = static_cast<UINT>(instances.size());
            srvDesc.Buffer.StructureByteStride = sizeof(GpuInstance);
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            nativeDevice->CreateShaderResourceView(m_instanceBuffer.Get(), &srvDesc, m_descriptors.instanceSrvCpu);
        }

        return true;
    }

    bool DxrRayTracer::BuildAccelerationStructures(IRHIDevice& device)
    {
        if (!m_supported) {
            return false;
        }

        ID3D12Device5* dxrDevice = device.GetRayTracingDevice();
        if (!dxrDevice || !m_vertexBuffer.IsValid() || !m_indexBuffer.IsValid()) {
            return false;
        }

        if (m_scene.instances.empty() || m_meshRecords.empty()) {
            m_tlas.Reset();
            return true;
        }

        CommandAllocator allocator;
        CommandList commandList;
        if (FAILED(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, allocator)) ||
            FAILED(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, commandList))) {
            return false;
        }

        ComPtr<ID3D12GraphicsCommandList4> dxrCommandList;
        if (FAILED(commandList.Get()->QueryInterface(IID_PPV_ARGS(&dxrCommandList)))) {
            return false;
        }

        std::vector<Resource> scratchBuffers;
        scratchBuffers.reserve(m_meshRecords.size() + 1u);

        for (MeshRecord& mesh : m_meshRecords) {
            D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc{};
            geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            geometryDesc.Triangles.Transform3x4 = 0u;
            geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geometryDesc.Triangles.IndexCount = mesh.indexCount;
            geometryDesc.Triangles.VertexCount = mesh.vertexCount;
            geometryDesc.Triangles.IndexBuffer = m_indexBuffer->GetGPUVirtualAddress() + static_cast<UINT64>(mesh.indexOffset) * sizeof(uint32_t);
            geometryDesc.Triangles.VertexBuffer.StartAddress = m_vertexBuffer->GetGPUVirtualAddress() + static_cast<UINT64>(mesh.vertexOffset) * sizeof(GpuVertex);
            geometryDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(GpuVertex);

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            inputs.NumDescs = 1u;
            inputs.pGeometryDescs = &geometryDesc;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo{};
            dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);
            if (prebuildInfo.ResultDataMaxSizeInBytes == 0u) {
                return false;
            }

            Resource scratchBuffer;
            if (!CreateBuffer(device,
                              prebuildInfo.ScratchDataSizeInBytes,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                              scratchBuffer) ||
                !CreateBuffer(device,
                              prebuildInfo.ResultDataMaxSizeInBytes,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                              mesh.blas)) {
                return false;
            }

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
            buildDesc.Inputs = inputs;
            buildDesc.ScratchAccelerationStructureData = scratchBuffer->GetGPUVirtualAddress();
            buildDesc.DestAccelerationStructureData = mesh.blas->GetGPUVirtualAddress();
            dxrCommandList->BuildRaytracingAccelerationStructure(&buildDesc, 0u, nullptr);

            const D3D12_RESOURCE_BARRIER barrier =
                CD3DX12_RESOURCE_BARRIER::UAV(mesh.blas.Get());
            dxrCommandList->ResourceBarrier(1u, &barrier);

            scratchBuffers.push_back(std::move(scratchBuffer));
        }

        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs(m_scene.instances.size());
        for (size_t instanceIndex = 0; instanceIndex < m_scene.instances.size(); ++instanceIndex) {
            const RayTracingInstance& sceneInstance = m_scene.instances[instanceIndex];
            if (sceneInstance.meshIndex >= m_meshRecords.size()) {
                continue;
            }

            D3D12_RAYTRACING_INSTANCE_DESC& instanceDesc = instanceDescs[instanceIndex];
            std::memset(&instanceDesc, 0, sizeof(instanceDesc));
            ConvertToDxrTransform(sceneInstance.model, instanceDesc.Transform);
            instanceDesc.InstanceID = static_cast<UINT>(instanceIndex);
            instanceDesc.InstanceContributionToHitGroupIndex = 0u;
            instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
            instanceDesc.AccelerationStructure = m_meshRecords[sceneInstance.meshIndex].blas->GetGPUVirtualAddress();
            instanceDesc.InstanceMask = 0xFFu;
        }

        Resource instanceDescBuffer;
        void* mappedInstanceData = nullptr;
        if (!ResourceUploadUtility::CreateUploadBuffer(device,
                                                       static_cast<UINT64>(instanceDescs.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
                                                       instanceDescBuffer,
                                                       &mappedInstanceData)) {
            return false;
        }
        if (!instanceDescs.empty()) {
            std::memcpy(mappedInstanceData,
                        instanceDescs.data(),
                        static_cast<size_t>(instanceDescs.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
            instanceDescBuffer->Unmap(0, nullptr);
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topLevelInputs{};
        topLevelInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        topLevelInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        topLevelInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        topLevelInputs.NumDescs = static_cast<UINT>(instanceDescs.size());
        topLevelInputs.InstanceDescs = instanceDescBuffer->GetGPUVirtualAddress();

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topLevelPrebuildInfo{};
        dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&topLevelInputs, &topLevelPrebuildInfo);

        Resource topLevelScratch;
        if (!CreateBuffer(device,
                          topLevelPrebuildInfo.ScratchDataSizeInBytes,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          topLevelScratch) ||
            !CreateBuffer(device,
                          topLevelPrebuildInfo.ResultDataMaxSizeInBytes,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                          m_tlas)) {
            return false;
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topLevelBuildDesc{};
        topLevelBuildDesc.Inputs = topLevelInputs;
        topLevelBuildDesc.ScratchAccelerationStructureData = topLevelScratch->GetGPUVirtualAddress();
        topLevelBuildDesc.DestAccelerationStructureData = m_tlas->GetGPUVirtualAddress();
        dxrCommandList->BuildRaytracingAccelerationStructure(&topLevelBuildDesc, 0u, nullptr);

        const D3D12_RESOURCE_BARRIER tlasBarrier =
            CD3DX12_RESOURCE_BARRIER::UAV(m_tlas.Get());
        dxrCommandList->ResourceBarrier(1u, &tlasBarrier);

        if (FAILED(commandList.Close())) {
            return false;
        }

        ID3D12CommandList* commandLists[] = { commandList.Get() };
        device.GetCommandQueue()->ExecuteCommandLists(1u, commandLists);
        device.WaitForGPU();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.RaytracingAccelerationStructure.Location = m_tlas->GetGPUVirtualAddress();
        device.GetDevice()->CreateShaderResourceView(nullptr, &srvDesc, m_descriptors.tlasSrvCpu);
        return true;
    }

    bool DxrRayTracer::Render(IRHIDevice& device,
                              CommandList& cmdList,
                              DescriptorHeap& srvHeap,
                              Resource& outputTexture,
                              const RayTracingFrameDesc& frameDesc,
                              RayTracingRuntimeStats& outStats)
    {
        if (!m_supported || !m_pipelineReady || m_sceneDirty || !m_tlas.IsValid()) {
            return false;
        }

        ID3D12Device* nativeDevice = device.GetDevice();
        if (!nativeDevice || !CreateTextureUav(nativeDevice, outputTexture, m_descriptors.outputUavCpu)) {
            return false;
        }

        if (!EnsureFrameConstantBuffer(device)) {
            return false;
        }

        FrameConstants constants{};
        FillFrameConstants(frameDesc, constants);
        std::memcpy(m_frameConstantsPtr, &constants, sizeof(constants));

        ComPtr<ID3D12GraphicsCommandList4> dxrCommandList;
        if (FAILED(cmdList.Get()->QueryInterface(IID_PPV_ARGS(&dxrCommandList)))) {
            return false;
        }

        const auto startTime = std::chrono::high_resolution_clock::now();

        DescriptorHeap* heaps[] = { &srvHeap };
        cmdList.SetDescriptorHeaps(1u, heaps);

        dxrCommandList->SetComputeRootSignature(m_globalRootSignature.Get());
        dxrCommandList->SetPipelineState1(m_stateObject.Get());
        dxrCommandList->SetComputeRootConstantBufferView(0u, m_frameConstantsBuffer->GetGPUVirtualAddress());
        dxrCommandList->SetComputeRootShaderResourceView(1u, m_tlas->GetGPUVirtualAddress());

        const auto outputToUavBarrier = CD3DX12_RESOURCE_BARRIER::Transition(outputTexture.Get(),
                                                                             D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dxrCommandList->ResourceBarrier(1u, &outputToUavBarrier);

        D3D12_DISPATCH_RAYS_DESC dispatchDesc{};
        dispatchDesc.RayGenerationShaderRecord.StartAddress = m_rayGenShaderTable->GetGPUVirtualAddress();
        dispatchDesc.RayGenerationShaderRecord.SizeInBytes = m_rayGenShaderRecordSize;
        dispatchDesc.MissShaderTable.StartAddress = m_missShaderTable->GetGPUVirtualAddress();
        dispatchDesc.MissShaderTable.SizeInBytes = m_missShaderRecordSize * 2u;
        dispatchDesc.MissShaderTable.StrideInBytes = m_missShaderRecordSize;
        dispatchDesc.HitGroupTable.StartAddress = m_hitGroupShaderTable->GetGPUVirtualAddress();
        dispatchDesc.HitGroupTable.SizeInBytes = m_hitGroupShaderRecordSize;
        dispatchDesc.HitGroupTable.StrideInBytes = m_hitGroupShaderRecordSize;
        dispatchDesc.Width = std::max(1u, frameDesc.renderWidth);
        dispatchDesc.Height = std::max(1u, frameDesc.renderHeight);
        dispatchDesc.Depth = 1u;
        dxrCommandList->DispatchRays(&dispatchDesc);

        const D3D12_RESOURCE_BARRIER outputToCopySource =
            CD3DX12_RESOURCE_BARRIER::Transition(outputTexture.Get(),
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
        dxrCommandList->ResourceBarrier(1u, &outputToCopySource);

        const auto endTime = std::chrono::high_resolution_clock::now();
        outStats.usingHardwarePath = true;
        outStats.usedFallback = false;
        outStats.renderWidth = frameDesc.renderWidth;
        outStats.renderHeight = frameDesc.renderHeight;
        outStats.dynamicResolutionScale = frameDesc.dynamicResolutionScale;
        outStats.qualityTier = frameDesc.qualityTier;
        outStats.sceneBuildMs = m_reportSceneBuildCost ? m_lastSceneBuildMs : 0.0f;
        outStats.primaryTraceMs = 0.0f;
        outStats.shadowTraceMs = 0.0f;
        outStats.shadeMs = 0.0f;
        outStats.resolveMs = 0.0f;
        outStats.traceMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        outStats.primaryTraceMs = outStats.traceMs;
        outStats.lastFrameMs = outStats.sceneBuildMs + outStats.traceMs + outStats.copyMs;
        outStats.bvhNodeCount = static_cast<uint32_t>(m_meshRecords.size() + 1u);
        m_reportSceneBuildCost = false;
        return true;
    }
}
