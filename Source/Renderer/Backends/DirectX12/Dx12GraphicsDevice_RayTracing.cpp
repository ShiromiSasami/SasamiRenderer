// Dx12GraphicsDevice_RayTracing.cpp
// DX12 implementation of RHI ray tracing device methods.

#include "Renderer/Backends/DirectX12/Dx12GraphicsDevice.h"

#include <algorithm>
#include <cstring>
#include <vector>
#include <windows.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "d3dx12.h"

namespace SasamiRenderer
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        static bool CreateAsBuffer(ID3D12Device* device,
                                   UINT64 size,
                                   D3D12_RESOURCE_FLAGS flags,
                                   D3D12_RESOURCE_STATES initialState,
                                   Resource& out)
        {
            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width            = size;
            desc.Height           = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels        = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags            = flags;
            // Resource wraps ComPtr<ID3D12Resource>; use GetAddressOf() for IID_PPV_ARGS
            return SUCCEEDED(device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
                IID_PPV_ARGS(out.GetAddressOf())));
        }

        static UINT AlignSBT(UINT value, UINT alignment)
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }
    }

    // -----------------------------------------------------------------------
    // BuildRhiBlases
    // -----------------------------------------------------------------------
    bool Dx12GraphicsDevice::BuildRhiBlases(
        const RhiBlasDesc* descs, uint32_t count,
        RhiAccelerationStructureHandle* outHandles)
    {
        if (!descs || count == 0 || !outHandles) {
            return false;
        }

        ID3D12Device5* dxrDevice = GetRayTracingDevice();
        if (!dxrDevice) {
            return false;
        }

        for (uint32_t i = 0; i < count; ++i) {
            outHandles[i] = {};
        }

        CommandAllocator allocator;
        CommandList commandList;
        if (FAILED(CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, allocator)) ||
            FAILED(CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, commandList))) {
            return false;
        }

        ComPtr<ID3D12GraphicsCommandList4> dxrCmdList;
        if (FAILED(commandList.Get()->QueryInterface(IID_PPV_ARGS(&dxrCmdList)))) {
            return false;
        }

        std::vector<Resource> scratchBuffers;
        scratchBuffers.reserve(count);
        std::vector<uint64_t> newIds(count, 0);

        for (uint32_t i = 0; i < count; ++i) {
            const RhiBlasDesc& d = descs[i];
            if (d.geometryCount == 0 || !d.geometries) {
                return false;
            }

            std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geomDescs(d.geometryCount);
            for (uint32_t g = 0; g < d.geometryCount; ++g) {
                const RhiRayTracingGeometryDesc& rg = d.geometries[g];
                D3D12_RAYTRACING_GEOMETRY_DESC& gd = geomDescs[g];
                gd.Type    = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                gd.Flags   = rg.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
                gd.Triangles.Transform3x4              = 0;
                gd.Triangles.IndexFormat               = rg.index32Bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
                gd.Triangles.VertexFormat              = DXGI_FORMAT_R32G32B32_FLOAT;
                gd.Triangles.IndexCount                = rg.indexCount;
                gd.Triangles.VertexCount               = rg.vertexCount;
                gd.Triangles.IndexBuffer               = rg.indexBufferAddress;
                gd.Triangles.VertexBuffer.StartAddress = rg.vertexBufferAddress;
                gd.Triangles.VertexBuffer.StrideInBytes = rg.vertexStrideInBytes;
            }

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
            inputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            inputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.Flags         = d.preferFastTrace
                ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
                : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            inputs.NumDescs      = d.geometryCount;
            inputs.pGeometryDescs = geomDescs.data();

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
            dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);
            if (prebuild.ResultDataMaxSizeInBytes == 0) {
                return false;
            }

            Resource scratch, result;
            if (!CreateAsBuffer(m_device.Get(),
                                prebuild.ScratchDataSizeInBytes,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                scratch) ||
                !CreateAsBuffer(m_device.Get(),
                                prebuild.ResultDataMaxSizeInBytes,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                                result)) {
                return false;
            }

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
            buildDesc.Inputs                                  = inputs;
            buildDesc.ScratchAccelerationStructureData        = scratch->GetGPUVirtualAddress();
            buildDesc.DestAccelerationStructureData           = result->GetGPUVirtualAddress();
            dxrCmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

            const D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(result.Get());
            dxrCmdList->ResourceBarrier(1, &barrier);

            const uint64_t id = m_nextRhiAccelStructHandle++;
            newIds[i] = id;
            m_dx12AccelStructures.emplace(id, Dx12AccelStruct{ std::move(result) });
            scratchBuffers.push_back(std::move(scratch));
        }

        if (FAILED(commandList.Close())) {
            return false;
        }
        ID3D12CommandList* lists[] = { commandList.Get() };
        GetCommandQueue()->ExecuteCommandLists(1, lists);
        WaitForGPU();

        for (uint32_t i = 0; i < count; ++i) {
            outHandles[i].id = newIds[i];
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // BuildRhiTlas
    // -----------------------------------------------------------------------
    RhiAccelerationStructureHandle Dx12GraphicsDevice::BuildRhiTlas(const RhiTlasDesc& desc)
    {
        if (desc.instanceCount == 0 || !desc.instances) {
            return {};
        }

        ID3D12Device5* dxrDevice = GetRayTracingDevice();
        if (!dxrDevice) {
            return {};
        }

        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs(desc.instanceCount);
        for (uint32_t i = 0; i < desc.instanceCount; ++i) {
            const RhiTlasInstanceDesc& src = desc.instances[i];
            D3D12_RAYTRACING_INSTANCE_DESC& dst = instanceDescs[i];
            std::memset(&dst, 0, sizeof(dst));
            std::memcpy(dst.Transform, src.transform, sizeof(dst.Transform));
            dst.InstanceID                             = src.instanceID;
            dst.InstanceMask                           = src.instanceMask;
            dst.InstanceContributionToHitGroupIndex    = src.hitGroupIndex;
            dst.Flags                                  = src.forceOpaque ? D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE : 0;

            auto it = m_dx12AccelStructures.find(src.blasHandle.id);
            if (it != m_dx12AccelStructures.end() && it->second.buffer.IsValid()) {
                dst.AccelerationStructure = it->second.buffer->GetGPUVirtualAddress();
            }
        }

        const UINT64 instanceDataSize = static_cast<UINT64>(instanceDescs.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        Resource instanceBuf;
        void* mapped = nullptr;
        if (!ResourceUploadUtility::CreateUploadBuffer(*this, instanceDataSize, instanceBuf, &mapped)) {
            return {};
        }
        std::memcpy(mapped, instanceDescs.data(), static_cast<size_t>(instanceDataSize));
        instanceBuf->Unmap(0, nullptr);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        inputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.Flags         = desc.preferFastTrace
            ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
            : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        inputs.NumDescs      = desc.instanceCount;
        inputs.InstanceDescs = instanceBuf->GetGPUVirtualAddress();

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
        dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

        Resource scratch, result;
        if (!CreateAsBuffer(m_device.Get(),
                            prebuild.ScratchDataSizeInBytes,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            scratch) ||
            !CreateAsBuffer(m_device.Get(),
                            prebuild.ResultDataMaxSizeInBytes,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                            result)) {
            return {};
        }

        CommandAllocator allocator;
        CommandList commandList;
        if (FAILED(CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, allocator)) ||
            FAILED(CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, commandList))) {
            return {};
        }

        ComPtr<ID3D12GraphicsCommandList4> dxrCmdList;
        if (FAILED(commandList.Get()->QueryInterface(IID_PPV_ARGS(&dxrCmdList)))) {
            return {};
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
        buildDesc.Inputs                           = inputs;
        buildDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        buildDesc.DestAccelerationStructureData    = result->GetGPUVirtualAddress();
        dxrCmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

        const D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(result.Get());
        dxrCmdList->ResourceBarrier(1, &barrier);

        if (FAILED(commandList.Close())) {
            return {};
        }
        ID3D12CommandList* lists[] = { commandList.Get() };
        GetCommandQueue()->ExecuteCommandLists(1, lists);
        WaitForGPU();

        const uint64_t id = m_nextRhiAccelStructHandle++;
        m_dx12AccelStructures.emplace(id, Dx12AccelStruct{ std::move(result) });
        return RhiAccelerationStructureHandle{ id };
    }

    // -----------------------------------------------------------------------
    // DestroyRhiAccelerationStructure
    // -----------------------------------------------------------------------
    bool Dx12GraphicsDevice::DestroyRhiAccelerationStructure(RhiAccelerationStructureHandle handle)
    {
        return m_dx12AccelStructures.erase(handle.id) > 0;
    }

    // -----------------------------------------------------------------------
    // GetRhiAccelerationStructureGpuAddress
    // -----------------------------------------------------------------------
    RhiGpuAddress Dx12GraphicsDevice::GetRhiAccelerationStructureGpuAddress(
        RhiAccelerationStructureHandle handle)
    {
        auto it = m_dx12AccelStructures.find(handle.id);
        if (it == m_dx12AccelStructures.end() || !it->second.buffer.IsValid()) {
            return 0;
        }
        return static_cast<RhiGpuAddress>(it->second.buffer->GetGPUVirtualAddress());
    }

    // -----------------------------------------------------------------------
    // CreateRhiAccelerationStructureSrv
    // -----------------------------------------------------------------------
    bool Dx12GraphicsDevice::CreateRhiAccelerationStructureSrv(
        RhiAccelerationStructureHandle handle, RhiCpuDescriptorHandle dest)
    {
        auto it = m_dx12AccelStructures.find(handle.id);
        if (it == m_dx12AccelStructures.end() || !it->second.buffer.IsValid()) {
            return false;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.RaytracingAccelerationStructure.Location = it->second.buffer->GetGPUVirtualAddress();
        m_device->CreateShaderResourceView(nullptr, &srvDesc, { static_cast<SIZE_T>(dest.ptr) });
        return true;
    }

    // -----------------------------------------------------------------------
    // CreateRhiRayTracingPipeline
    // -----------------------------------------------------------------------
    RhiRayTracingPipelineHandle Dx12GraphicsDevice::CreateRhiRayTracingPipeline(
        const RhiRayTracingPipelineDesc& desc)
    {
        if (!desc.libraryBytecode || desc.libraryBytecodeSizeInBytes == 0) {
            return {};
        }

        ID3D12Device5* dxrDevice = GetRayTracingDevice();
        if (!dxrDevice) {
            return {};
        }

        // Root signature: CBV0 (frame constants) + SRV0 (TLAS) + static sampler
        D3D12_ROOT_PARAMETER rootParams[2] = {};
        rootParams[0].ParameterType              = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[0].Descriptor.ShaderRegister  = 0;
        rootParams[0].ShaderVisibility           = D3D12_SHADER_VISIBILITY_ALL;
        rootParams[1].ParameterType              = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[1].Descriptor.ShaderRegister  = 0;
        rootParams[1].ShaderVisibility           = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter     = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU   = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressV   = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressW   = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootSigDesc{};
        rootSigDesc.NumParameters    = _countof(rootParams);
        rootSigDesc.pParameters      = rootParams;
        rootSigDesc.NumStaticSamplers = 1;
        rootSigDesc.pStaticSamplers  = &samplerDesc;
        rootSigDesc.Flags            = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

        ComPtr<ID3DBlob> sigBlob, sigErrors;
        if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                               &sigBlob, &sigErrors))) {
            DebugLog("Dx12GraphicsDevice: failed to serialize ray tracing root signature.\n");
            return {};
        }

        ComPtr<ID3D12RootSignature> rootSignature;
        if (FAILED(m_device->CreateRootSignature(0,
                                                  sigBlob->GetBufferPointer(),
                                                  sigBlob->GetBufferSize(),
                                                  IID_PPV_ARGS(&rootSignature)))) {
            DebugLog("Dx12GraphicsDevice: failed to create ray tracing root signature.\n");
            return {};
        }

        // Convert RHI group descs to wide strings and collect hit group info
        struct HitGroupInfo {
            std::wstring hitGroupExport;
            std::wstring closestHitExport;
        };
        std::vector<HitGroupInfo> hitGroupInfos;
        std::vector<std::wstring> groupExportNames(desc.shaderGroupCount);

        auto toWide = [](const char* utf8) -> std::wstring {
            if (!utf8 || utf8[0] == '\0') {
                return {};
            }
            const int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
            if (len <= 0) {
                return {};
            }
            std::wstring w(static_cast<size_t>(len - 1), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), len);
            return w;
        };

        for (uint32_t g = 0; g < desc.shaderGroupCount; ++g) {
            const RhiRayTracingShaderGroupDesc& sg = desc.shaderGroups[g];
            if (sg.type == RhiRayTracingShaderGroupDesc::Type::General) {
                groupExportNames[g] = toWide(sg.exportName);
            } else {
                HitGroupInfo info{ toWide(sg.hitGroupExport), toWide(sg.closestHitExport) };
                groupExportNames[g] = info.hitGroupExport;
                hitGroupInfos.push_back(std::move(info));
            }
        }

        // Build D3D12_HIT_GROUP_DESC array (pointed at stable strings in hitGroupInfos)
        std::vector<D3D12_HIT_GROUP_DESC> d3dHitGroups;
        d3dHitGroups.reserve(hitGroupInfos.size());
        for (const HitGroupInfo& hg : hitGroupInfos) {
            D3D12_HIT_GROUP_DESC d{};
            d.Type                   = D3D12_HIT_GROUP_TYPE_TRIANGLES;
            d.HitGroupExport         = hg.hitGroupExport.empty() ? nullptr : hg.hitGroupExport.c_str();
            d.ClosestHitShaderImport = hg.closestHitExport.empty() ? nullptr : hg.closestHitExport.c_str();
            d3dHitGroups.push_back(d);
        }

        // Collect all export names for the shader config association
        std::vector<LPCWSTR> shaderExports;
        shaderExports.reserve(groupExportNames.size());
        for (const std::wstring& name : groupExportNames) {
            if (!name.empty()) {
                shaderExports.push_back(name.c_str());
            }
        }

        D3D12_DXIL_LIBRARY_DESC dxilLib{};
        dxilLib.DXILLibrary.pShaderBytecode = desc.libraryBytecode;
        dxilLib.DXILLibrary.BytecodeLength  = static_cast<SIZE_T>(desc.libraryBytecodeSizeInBytes);

        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
        shaderConfig.MaxPayloadSizeInBytes   = desc.maxPayloadSizeBytes;
        shaderConfig.MaxAttributeSizeInBytes = desc.maxAttributeSizeBytes;

        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION shaderConfigAssoc{};
        shaderConfigAssoc.NumExports = static_cast<UINT>(shaderExports.size());
        shaderConfigAssoc.pExports   = shaderExports.empty() ? nullptr : shaderExports.data();

        D3D12_GLOBAL_ROOT_SIGNATURE globalRootSig{};
        globalRootSig.pGlobalRootSignature = rootSignature.Get();

        D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
        pipelineConfig.MaxTraceRecursionDepth = desc.maxRecursionDepth;

        // Subobjects: lib + hit groups + shader config + assoc + global root sig + pipeline config
        const UINT numSubobjects = 5u + static_cast<UINT>(d3dHitGroups.size());
        std::vector<D3D12_STATE_SUBOBJECT> subobjects(numSubobjects);
        UINT si = 0;
        subobjects[si].Type  = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
        subobjects[si++].pDesc = &dxilLib;
        for (D3D12_HIT_GROUP_DESC& hg : d3dHitGroups) {
            subobjects[si].Type  = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
            subobjects[si++].pDesc = &hg;
        }
        subobjects[si].Type  = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
        subobjects[si++].pDesc = &shaderConfig;
        shaderConfigAssoc.pSubobjectToAssociate = &subobjects[si - 1];
        subobjects[si].Type  = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
        subobjects[si++].pDesc = &shaderConfigAssoc;
        subobjects[si].Type  = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
        subobjects[si++].pDesc = &globalRootSig;
        subobjects[si].Type  = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
        subobjects[si++].pDesc = &pipelineConfig;

        D3D12_STATE_OBJECT_DESC stateObjDesc{};
        stateObjDesc.Type          = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        stateObjDesc.NumSubobjects = si;
        stateObjDesc.pSubobjects   = subobjects.data();

        Dx12RTPipeline pipeline{};
        if (FAILED(dxrDevice->CreateStateObject(&stateObjDesc, IID_PPV_ARGS(&pipeline.stateObject))) ||
            FAILED(pipeline.stateObject.As(&pipeline.props))) {
            DebugLog("Dx12GraphicsDevice: failed to create ray tracing state object.\n");
            return {};
        }
        pipeline.rootSignature    = std::move(rootSignature);
        pipeline.groupExportNames = std::move(groupExportNames);
        pipeline.shaderGroupCount = desc.shaderGroupCount;

        const uint64_t id = m_nextRhiRTPipelineHandle++;
        m_dx12RTPipelines.emplace(id, std::move(pipeline));
        return RhiRayTracingPipelineHandle{ id };
    }

    // -----------------------------------------------------------------------
    // CreateRhiShaderBindingTable
    // -----------------------------------------------------------------------
    RhiShaderBindingTableHandle Dx12GraphicsDevice::CreateRhiShaderBindingTable(
        const RhiShaderBindingTableDesc& desc)
    {
        auto pipeIt = m_dx12RTPipelines.find(desc.pipeline.id);
        if (pipeIt == m_dx12RTPipelines.end() || !pipeIt->second.props) {
            return {};
        }
        Dx12RTPipeline& pipe = pipeIt->second;

        const UINT recordSize = AlignSBT(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
                                          D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);

        auto writeRecord = [&](uint8_t* dst, uint32_t groupIdx) {
            if (groupIdx < pipe.groupExportNames.size()) {
                const std::wstring& name = pipe.groupExportNames[groupIdx];
                if (!name.empty()) {
                    const void* id = pipe.props->GetShaderIdentifier(name.c_str());
                    if (id) {
                        std::memcpy(dst, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
                        return;
                    }
                }
            }
            std::memset(dst, 0, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        };

        const UINT missCount  = std::max(1u, desc.missGroupCount);
        const UINT hitCount   = std::max(1u, desc.hitGroupCount);
        const UINT rayGenSize = AlignSBT(recordSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
        const UINT missSize   = AlignSBT(recordSize * missCount,  D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
        const UINT hitSize    = AlignSBT(recordSize * hitCount,   D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

        Dx12SBT sbt{};
        sbt.rayGenRecordSize   = recordSize;
        sbt.missRecordSize     = recordSize;
        sbt.hitGroupRecordSize = recordSize;
        sbt.missCount          = desc.missGroupCount;
        sbt.hitGroupCount      = desc.hitGroupCount;

        uint8_t* rgMapped  = nullptr;
        uint8_t* missMapped = nullptr;
        uint8_t* hitMapped  = nullptr;
        if (!ResourceUploadUtility::CreateUploadBuffer(*this, rayGenSize, sbt.rayGenTable,   reinterpret_cast<void**>(&rgMapped))   ||
            !ResourceUploadUtility::CreateUploadBuffer(*this, missSize,   sbt.missTable,     reinterpret_cast<void**>(&missMapped)) ||
            !ResourceUploadUtility::CreateUploadBuffer(*this, hitSize,    sbt.hitGroupTable, reinterpret_cast<void**>(&hitMapped))) {
            return {};
        }

        writeRecord(rgMapped, desc.rayGenGroupIndex);

        for (uint32_t m = 0; m < desc.missGroupCount; ++m) {
            writeRecord(missMapped + static_cast<UINT64>(m) * recordSize, desc.missGroupIndices[m]);
        }
        for (uint32_t h = 0; h < desc.hitGroupCount; ++h) {
            writeRecord(hitMapped + static_cast<UINT64>(h) * recordSize, desc.hitGroupIndices[h]);
        }

        const uint64_t id = m_nextRhiSBTHandle++;
        m_dx12SBTs.emplace(id, std::move(sbt));
        return RhiShaderBindingTableHandle{ id };
    }

    // -----------------------------------------------------------------------
    // DX12-specific RT accessors (used by DxrRayTracer::Render)
    // -----------------------------------------------------------------------
    ID3D12StateObject* Dx12GraphicsDevice::GetDx12RayTracingStateObject(
        RhiRayTracingPipelineHandle handle)
    {
        auto it = m_dx12RTPipelines.find(handle.id);
        return it != m_dx12RTPipelines.end() ? it->second.stateObject.Get() : nullptr;
    }

    ID3D12RootSignature* Dx12GraphicsDevice::GetDx12RayTracingRootSignature(
        RhiRayTracingPipelineHandle handle)
    {
        auto it = m_dx12RTPipelines.find(handle.id);
        return it != m_dx12RTPipelines.end() ? it->second.rootSignature.Get() : nullptr;
    }

    bool Dx12GraphicsDevice::FillDx12DispatchRaysDesc(
        RhiShaderBindingTableHandle handle, D3D12_DISPATCH_RAYS_DESC& out)
    {
        auto it = m_dx12SBTs.find(handle.id);
        if (it == m_dx12SBTs.end() || !it->second.rayGenTable.IsValid()) {
            return false;
        }
        const Dx12SBT& sbt = it->second;
        out.RayGenerationShaderRecord.StartAddress = sbt.rayGenTable->GetGPUVirtualAddress();
        out.RayGenerationShaderRecord.SizeInBytes  = sbt.rayGenRecordSize;
        out.MissShaderTable.StartAddress           = sbt.missTable->GetGPUVirtualAddress();
        out.MissShaderTable.SizeInBytes            = sbt.missRecordSize * sbt.missCount;
        out.MissShaderTable.StrideInBytes          = sbt.missRecordSize;
        out.HitGroupTable.StartAddress             = sbt.hitGroupTable->GetGPUVirtualAddress();
        out.HitGroupTable.SizeInBytes              = sbt.hitGroupRecordSize * sbt.hitGroupCount;
        out.HitGroupTable.StrideInBytes            = sbt.hitGroupRecordSize;
        return true;
    }

} // namespace SasamiRenderer
