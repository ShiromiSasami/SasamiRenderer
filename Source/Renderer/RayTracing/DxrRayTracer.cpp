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
#include "Renderer/RayTracing/DxrRayTracerUtility.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "d3dx12.h"

namespace SasamiRenderer
{
    using Microsoft::WRL::ComPtr;

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
            if (!DxrRayTracerUtility::CreateBuffer(device,
                              vertexBufferSize,
                              D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST,
                              m_vertexBuffer)) {
                return false;
            }
            DxrRayTracerUtility::CopyBufferData(device, vertices.data(), vertexBufferSize, m_vertexBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            m_vertexBuffer.Reset();
        }

        if (!indices.empty()) {
            const UINT64 indexBufferSize = static_cast<UINT64>(indices.size()) * sizeof(uint32_t);
            if (!DxrRayTracerUtility::CreateBuffer(device,
                              indexBufferSize,
                              D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST,
                              m_indexBuffer)) {
                return false;
            }
            DxrRayTracerUtility::CopyBufferData(device, indices.data(), indexBufferSize, m_indexBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            m_indexBuffer.Reset();
        }

        if (!materials.empty()) {
            const UINT64 materialBufferSize = static_cast<UINT64>(materials.size()) * sizeof(GpuMaterial);
            if (!DxrRayTracerUtility::CreateBuffer(device,
                              materialBufferSize,
                              D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST,
                              m_materialBuffer)) {
                return false;
            }
            DxrRayTracerUtility::CopyBufferData(device, materials.data(), materialBufferSize, m_materialBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            m_materialBuffer.Reset();
        }

        if (!instances.empty()) {
            const UINT64 instanceBufferSize = static_cast<UINT64>(instances.size()) * sizeof(GpuInstance);
            if (!DxrRayTracerUtility::CreateBuffer(device,
                              instanceBufferSize,
                              D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST,
                              m_instanceBuffer)) {
                return false;
            }
            DxrRayTracerUtility::CopyBufferData(device, instances.data(), instanceBufferSize, m_instanceBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
            m_tlasHandle = {};
            return true;
        }

        if (!m_vertexBuffer.IsValid() || !m_indexBuffer.IsValid()) {
            return false;
        }

        // Build one BLAS per mesh
        std::vector<RhiRayTracingGeometryDesc> geomDescs(m_meshRecords.size());
        std::vector<RhiBlasDesc>               blasDescs(m_meshRecords.size());
        for (size_t i = 0; i < m_meshRecords.size(); ++i) {
            const MeshRecord& mesh = m_meshRecords[i];
            RhiRayTracingGeometryDesc& gd = geomDescs[i];
            gd.vertexBufferAddress = m_vertexBuffer->GetGPUVirtualAddress()
                                   + static_cast<UINT64>(mesh.vertexOffset) * sizeof(GpuVertex);
            gd.vertexFormat        = RhiFormat::R32G32B32Float;
            gd.vertexCount         = mesh.vertexCount;
            gd.vertexStrideInBytes = sizeof(GpuVertex);
            gd.indexBufferAddress  = m_indexBuffer->GetGPUVirtualAddress()
                                   + static_cast<UINT64>(mesh.indexOffset) * sizeof(uint32_t);
            gd.indexCount          = mesh.indexCount;
            gd.index32Bit          = true;
            gd.opaque              = true;

            blasDescs[i].geometries     = &geomDescs[i];
            blasDescs[i].geometryCount  = 1u;
            blasDescs[i].preferFastTrace = true;
        }

        std::vector<RhiAccelerationStructureHandle> blasHandles(m_meshRecords.size());
        if (!device.BuildRhiBlases(blasDescs.data(),
                                   static_cast<uint32_t>(m_meshRecords.size()),
                                   blasHandles.data())) {
            return false;
        }
        for (size_t i = 0; i < m_meshRecords.size(); ++i) {
            m_meshRecords[i].blasHandle = blasHandles[i];
        }

        // Build TLAS from instance list
        std::vector<RhiTlasInstanceDesc> tlasInstances;
        tlasInstances.reserve(m_scene.instances.size());
        for (size_t instanceIndex = 0; instanceIndex < m_scene.instances.size(); ++instanceIndex) {
            const RayTracingInstance& src = m_scene.instances[instanceIndex];
            if (src.meshIndex >= m_meshRecords.size()) {
                continue;
            }
            RhiTlasInstanceDesc tid{};
            DxrRayTracerUtility::ConvertToDxrTransform(src.model,
                reinterpret_cast<float(*)[4]>(tid.transform));
            tid.blasHandle    = m_meshRecords[src.meshIndex].blasHandle;
            tid.instanceID    = static_cast<uint32_t>(instanceIndex);
            tid.instanceMask  = src.transparent ? 0x01u : 0xFFu;
            tid.hitGroupIndex = 0u;
            tid.forceOpaque   = true;
            tlasInstances.push_back(tid);
        }

        RhiTlasDesc tlasDesc{};
        tlasDesc.instances     = tlasInstances.data();
        tlasDesc.instanceCount = static_cast<uint32_t>(tlasInstances.size());
        tlasDesc.preferFastTrace = true;
        m_tlasHandle = device.BuildRhiTlas(tlasDesc);
        if (m_tlasHandle.id == 0) {
            return false;
        }

        // Register TLAS SRV in the descriptor set
        RhiCpuDescriptorHandle tlasDest{};
        tlasDest.ptr = static_cast<uint64_t>(m_descriptors.tlasSrvCpu.ptr);
        device.CreateRhiAccelerationStructureSrv(m_tlasHandle, tlasDest);

        return true;
    }

    bool DxrRayTracer::Render(IRHIDevice& device,
                              CommandList& cmdList,
                              DescriptorHeap& srvHeap,
                              Resource& outputTexture,
                              const RayTracingFrameDesc& frameDesc,
                              RayTracingRuntimeStats& outStats)
    {
        if (!m_supported || !m_pipelineReady || m_sceneDirty || m_tlasHandle.id == 0) {
            return false;
        }

        ID3D12Device* nativeDevice = device.GetDevice();
        if (!nativeDevice || !DxrRayTracerUtility::CreateTextureUav(nativeDevice, outputTexture, m_descriptors.outputUavCpu)) {
            return false;
        }

        ID3D12StateObject*   stateObject   = device.GetDx12RayTracingStateObject(m_rtPipelineHandle);
        ID3D12RootSignature* rootSignature = device.GetDx12RayTracingRootSignature(m_rtPipelineHandle);
        if (!stateObject || !rootSignature) {
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

        dxrCommandList->SetComputeRootSignature(rootSignature);
        dxrCommandList->SetPipelineState1(stateObject);
        dxrCommandList->SetComputeRootConstantBufferView(0u, m_frameConstantsBuffer->GetGPUVirtualAddress());
        dxrCommandList->SetComputeRootShaderResourceView(
            1u, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(
                    device.GetRhiAccelerationStructureGpuAddress(m_tlasHandle)));

        const auto outputToUavBarrier = CD3DX12_RESOURCE_BARRIER::Transition(outputTexture.Get(),
                                                                             D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        dxrCommandList->ResourceBarrier(1u, &outputToUavBarrier);

        D3D12_DISPATCH_RAYS_DESC dispatchDesc{};
        if (!device.FillDx12DispatchRaysDesc(m_sbtHandle, dispatchDesc)) {
            return false;
        }
        dispatchDesc.Width  = std::max(1u, frameDesc.renderWidth);
        dispatchDesc.Height = std::max(1u, frameDesc.renderHeight);
        dispatchDesc.Depth  = 1u;
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
