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
            gpuMaterial.transmissionParams[0] = material.material.transmission;
            gpuMaterial.transmissionParams[1] = material.material.ior;
            gpuMaterial.transmissionParams[2] = material.material.transparentShellStrength;
            gpuMaterial.transmissionParams[3] = material.material.thickness;
            gpuMaterial.volumeParams[0] = material.material.attenuationColor[0];
            gpuMaterial.volumeParams[1] = material.material.attenuationColor[1];
            gpuMaterial.volumeParams[2] = material.material.attenuationColor[2];
            gpuMaterial.volumeParams[3] = material.material.attenuationDistance;
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

        if (m_scene.instances.empty() || m_meshRecords.empty()) {
            m_tlas.Reset();
            return true;
        }

        ID3D12Device5* dxrDevice = device.GetRayTracingDevice();
        if (!dxrDevice || !m_vertexBuffer.IsValid() || !m_indexBuffer.IsValid()) {
            return false;
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
            instanceDesc.InstanceMask = sceneInstance.transparent ? 0x01u : 0xFFu;
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
