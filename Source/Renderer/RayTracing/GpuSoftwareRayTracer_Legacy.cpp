// GpuSoftwareRayTracer_Legacy.cpp
// Legacy shadow/AO/reflection render passes.
#define NOMINMAX
#include "Renderer/RayTracing/GpuSoftwareRayTracer.h"

#include "Renderer/Scene/RenderLightProxy.h"
#include "Renderer/Scene/LightSystem.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"

#include "Foundation/Math/MathUtil.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <vector>

#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl.h>

#include "d3dx12.h"


using Microsoft::WRL::ComPtr;


namespace SasamiRenderer
{
    namespace
    {
        // ---- Create a default-heap GPU buffer and upload data ----
        // Creates its own command list, uploads, and stalls GPU. Use only on dirty/init.
        bool CreateAndUploadBuffer(IRHIDevice& device,
                                   const void* data, UINT64 byteSize,
                                   D3D12_RESOURCE_STATES finalState,
                                   Resource& outBuffer)
        {
            if (byteSize == 0) { outBuffer.Reset(); return true; }

            D3D12_HEAP_PROPERTIES defHeap{};
            defHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC bdesc{};
            bdesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bdesc.Width     = byteSize;
            bdesc.Height = bdesc.DepthOrArraySize = bdesc.MipLevels = 1;
            bdesc.SampleDesc.Count = 1;
            bdesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device.CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE,
                                                       &bdesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr, outBuffer))) {
                return false;
            }

            // Upload staging
            D3D12_HEAP_PROPERTIES upHeap{};
            upHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
            Resource staging;
            if (FAILED(device.CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE,
                                                       &bdesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr, staging))) {
                return false;
            }
            void* mapped = nullptr;
            if (FAILED(staging.Map(0, nullptr, &mapped))) return false;
            memcpy(mapped, data, byteSize);
            staging.Unmap(0, nullptr);

            CommandAllocator alloc; CommandList cmdList;
            if (FAILED(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, alloc))) return false;
            if (FAILED(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, cmdList))) return false;

            cmdList.Get()->CopyBufferRegion(outBuffer.Get(), 0, staging.Get(), 0, byteSize);
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(outBuffer.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, finalState);
            cmdList.Get()->ResourceBarrier(1, &barrier);
            cmdList.Get()->Close();
            ID3D12CommandList* lists[] = { cmdList.Get() };
            device.GetCommandQueue()->ExecuteCommandLists(1, lists);
            device.WaitForGPU();
            return true;
        }

        // ---- Create a StructuredBuffer SRV ----
        void CreateStructuredSrv(ID3D12Device* dev, Resource& buf,
                                 UINT elemCount, UINT stride,
                                 D3D12_CPU_DESCRIPTOR_HANDLE dest)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Format                  = DXGI_FORMAT_UNKNOWN;
            d.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            d.Buffer.FirstElement     = 0;
            d.Buffer.NumElements      = elemCount;
            d.Buffer.StructureByteStride = stride;
            d.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_NONE;
            dev->CreateShaderResourceView(buf.Get(), &d, dest);
        }

        // ---- Create a null SRV (for empty buffers) ----
        void CreateNullStructuredSrv(ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE dest)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Format                  = DXGI_FORMAT_UNKNOWN;
            d.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            d.Buffer.NumElements      = 0;
            d.Buffer.StructureByteStride = 4u;
            dev->CreateShaderResourceView(nullptr, &d, dest);
        }

        // ---- Create a UAV-capable default-heap buffer (no initial data) ----
        bool CreateUavBuffer(IRHIDevice& device, UINT64 byteSize,
                             D3D12_RESOURCE_STATES initialState, Resource& out)
        {
            if (byteSize == 0) { out.Reset(); return false; }
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width     = byteSize;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return SUCCEEDED(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                &desc, initialState, nullptr, out));
        }

        // ---- Create a Texture2D with UAV support ----
        bool CreateUavTexture2D(IRHIDevice& device, UINT w, UINT h, DXGI_FORMAT fmt,
                                D3D12_RESOURCE_STATES initialState, Resource& out)
        {
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width     = w; desc.Height = h;
            desc.DepthOrArraySize = 1; desc.MipLevels = 1;
            desc.Format    = fmt;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return SUCCEEDED(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                &desc, initialState, nullptr, out));
        }

        // ---- Transition barrier shorthand ----
        D3D12_RESOURCE_BARRIER MakeTransition(ID3D12Resource* res,
                                               D3D12_RESOURCE_STATES from,
                                               D3D12_RESOURCE_STATES to)
        {
            return CD3DX12_RESOURCE_BARRIER::Transition(res, from, to);
        }

        // ---- Create a Texture2D SRV into a given CPU handle ----
        void CreateTex2DSrv(ID3D12Device* dev, ID3D12Resource* tex,
                            DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE dest)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Format      = fmt;
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(tex, &d, dest);
        }

        void CreateNullTex2DSrv(ID3D12Device* dev, DXGI_FORMAT fmt,
                                D3D12_CPU_DESCRIPTOR_HANDLE dest)
        {
            CreateTex2DSrv(dev, nullptr, fmt, dest);
        }

        // ---- Create a Texture2D UAV into a given CPU handle ----
        void CreateTex2DUav(ID3D12Device* dev, ID3D12Resource* tex,
                            DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE dest)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
            d.Format        = fmt;
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            dev->CreateUnorderedAccessView(tex, nullptr, &d, dest);
        }

        void CreateNullTex2DUav(ID3D12Device* dev, DXGI_FORMAT fmt,
                                D3D12_CPU_DESCRIPTOR_HANDLE dest)
        {
            CreateTex2DUav(dev, nullptr, fmt, dest);
        }

    } // anonymous namespace


    void GpuSoftwareRayTracer::FillShadowConstants(const DirectionalShadowMapDesc& desc,
                                                    ShadowFrameConstants& out) const
    {
        memset(&out, 0, sizeof(out));
        memcpy(out.invLightVP, desc.inverseLightViewProjection, 64);
        memcpy(out.lightVP,    desc.lightViewProjection,        64);
        out.width   = desc.width;
        out.height  = desc.height;
        out.tMin    = 0.001f;
        out.depthBias = std::max(0.0f, desc.depthBias);
        out.arraySlice = desc.arraySlice;
    }

    void GpuSoftwareRayTracer::FillReflectionConstants(const ReflectionTextureDesc& desc,
                                                        ReflectionFrameConstants& out) const
    {
        memset(&out, 0, sizeof(out));
        memcpy(out.invVP, desc.frameDesc.inverseViewProjection, 64);
        out.cameraPos[0]       = desc.frameDesc.cameraPosition[0];
        out.cameraPos[1]       = desc.frameDesc.cameraPosition[1];
        out.cameraPos[2]       = desc.frameDesc.cameraPosition[2];
        out.tMin               = 0.001f;
        out.renderWidth        = desc.width;
        out.renderHeight       = desc.height;
        out.updatePhaseCount   = desc.updatePhaseCount;
        out.updatePhaseIndex   = desc.updatePhaseIndex;
        out.maxSurfaceRoughness   = desc.maxSurfaceRoughness;
        out.maxPrimaryHitDistance = desc.maxPrimaryHitDistance;
        out.maxReflectionDistance = desc.maxReflectionTraceDistance;
        out.minReflectionEnergy   = desc.minReflectionEnergy;

        const auto& dl = desc.frameDesc.directionalLight;
        // Compute direction from yaw/pitch (towards the light, opposite of forward)
        {
            float fwd[3]{};
            Math::DirectionFromYawPitch(dl.yaw, dl.pitch, fwd);
            out.dirLightDir[0] = -fwd[0];
            out.dirLightDir[1] = -fwd[1];
            out.dirLightDir[2] = -fwd[2];
        }
        out.dirLightIntensity = dl.intensity;
        out.dirLightColor[0]  = dl.color[0];
        out.dirLightColor[1]  = dl.color[1];
        out.dirLightColor[2]  = dl.color[2];
        out.shadowBias        = 0.002f;
        out.ambientColor[0]   = 0.1f;
        out.ambientColor[1]   = 0.1f;
        out.ambientColor[2]   = 0.1f;
        out.ambientIntensity  = 1.0f;
        out.iblEnabled           = desc.iblEnabled ? 1.0f : 0.0f;
        out.iblIntensity         = desc.iblIntensity;
        out.iblPrefilterMaxMip   = desc.iblPrefilterMaxMip;
        out.proceduralSkyEnabled = desc.proceduralSkyEnabled ? 1.0f : 0.0f;
        uint32_t nPt = 0, nSp = 0;
        if (desc.frameDesc.pointLights)
            nPt = std::min((uint32_t)desc.frameDesc.pointLights->size(), kMaxPointLights);
        if (desc.frameDesc.spotLights)
            nSp = std::min((uint32_t)desc.frameDesc.spotLights->size(), kMaxSpotLights);
        out.pointLightCount   = nPt;
        out.spotLightCount    = nSp;
        out.samplesPerPixel   = std::max(1u, desc.samplesPerPixel);
        out.samplingMode      = std::min(desc.samplingMode, 2u);
        out.frameIndex        = m_restirFrameIndex;
        // GBuffer native resolution — may be larger than renderWidth/Height when the
        // reflection is rendered at a reduced resolution scale.  The CS uses these to
        // scale its Load() coordinates so each reflection pixel samples the correct
        // G-Buffer texel rather than a biased top-left region of the screen.
        out.gbufferWidth  = (desc.gbufferWidth  > 0) ? desc.gbufferWidth  : desc.width;
        out.gbufferHeight = (desc.gbufferHeight > 0) ? desc.gbufferHeight : desc.height;
        out.maxBounces    = std::max(1u, desc.maxBounces);
        out.debugView     = desc.debugView;
    }

    void GpuSoftwareRayTracer::FillAmbientOcclusionConstants(const AmbientOcclusionTextureDesc& desc,
                                                             AmbientOcclusionFrameConstants& out) const
    {
        memset(&out, 0, sizeof(out));
        memcpy(out.invVP, desc.inverseViewProjection, 64);
        out.cameraPos[0] = desc.cameraPosition[0];
        out.cameraPos[1] = desc.cameraPosition[1];
        out.cameraPos[2] = desc.cameraPosition[2];
        out.tMin         = desc.tMin;
        out.renderWidth  = desc.width;
        out.renderHeight = desc.height;
        out.sampleCount  = std::max(1u, desc.sampleCount);
        out.frameIndex   = desc.frameIndex;
        out.radius       = desc.radius;
        out.power        = desc.power;
        out.gbufferWidth  = (desc.gbufferWidth > 0u) ? desc.gbufferWidth : desc.width;
        out.gbufferHeight = (desc.gbufferHeight > 0u) ? desc.gbufferHeight : desc.height;
    }

    // =========================================================================
    // RenderDirectionalShadowMap
    // =========================================================================

    bool GpuSoftwareRayTracer::RenderDirectionalShadowMap(const DirectionalShadowMapDesc& desc,
                                                           IRHIDevice& device,
                                                           CommandList& cmdList,
                                                           Resource& outTexture,
                                                           RayTracingRuntimeStats& outStats)
    {
        (void)outStats;
        if (!m_initialized || !m_shadowPso || !outTexture.IsValid()) return false;
        if (desc.width == 0 || desc.height == 0) return false;

        ID3D12Device* dev = device.GetDevice();
        ID3D12GraphicsCommandList* cl = cmdList.Get();
        if (!dev || !cl) return false;

        // Update UAV slot [6] for shadow output
        UpdateTextureUav(dev, outTexture, DXGI_FORMAT_R32_FLOAT, kShadowUavSlot,
                         LightSystem::kDirectionalCascadeCount);

        // Fill shadow constants into slot 0 of the frame constants buffer
        {
            const UINT64 cbufOffset = static_cast<UINT64>(desc.constantBufferSlot) * 256u;
            ShadowFrameConstants sc{};
            FillShadowConstants(desc, sc);
            memcpy(m_frameConstantsMapped + cbufOffset, &sc, sizeof(sc));
        }

        // Bind compute pipeline
        cl->SetPipelineState(m_shadowPso.Get());
        cl->SetComputeRootSignature(m_rootSignature.Get());

        // Bind descriptor heap
        ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
        cl->SetDescriptorHeaps(1, heaps);

        // Root[0]: inline CBV (slot 0 of frame constants buffer)
        cl->SetComputeRootConstantBufferView(0,
            m_frameConstantsBuffer.GetGPUVirtualAddress() +
            static_cast<UINT64>(desc.constantBufferSlot) * 256u);

        // Root[1]: SRV table (t0-t5, starts at heap offset 0)
        cl->SetComputeRootDescriptorTable(1, m_descHeap.GetGPUDescriptorHandleForHeapStart());

        // Root[2]: UAV table (shadow slot)
        D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
        uavGpu.ptr += kShadowUavSlot * m_descIncrementSize;
        cl->SetComputeRootDescriptorTable(2, uavGpu);

        // Root[3]: G-Buffer SRV table (t6-t8) — shadow shader doesn't use t6-t8,
        // but the root signature requires all tables to be set. Point to heap[kGBufferSrvBase]
        // which holds null SRVs (safe to bind, never accessed by shadow CS).
        D3D12_GPU_DESCRIPTOR_HANDLE gbufGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
        gbufGpu.ptr += kGBufferSrvBase * m_descIncrementSize;
        cl->SetComputeRootDescriptorTable(3, gbufGpu);

        // Dispatch (16x16 threads)
        const UINT gx = (desc.width  + 15u) / 16u;
        const UINT gy = (desc.height + 15u) / 16u;
        cl->Dispatch(gx, gy, 1u);

        outStats.usingHardwarePath = false;
        outStats.renderWidth  = desc.width;
        outStats.renderHeight = desc.height;
        return true;
    }

    bool GpuSoftwareRayTracer::RenderAmbientOcclusionTexture(const AmbientOcclusionTextureDesc& desc,
                                                             IRHIDevice& device,
                                                             CommandList& cmdList,
                                                             Resource& outTexture,
                                                             RayTracingRuntimeStats& outStats)
    {
        (void)outStats;
        if (!m_initialized || !m_aoPso || !outTexture.IsValid()) return false;
        if (desc.width == 0 || desc.height == 0) return false;

        ID3D12Device* dev = device.GetDevice();
        ID3D12GraphicsCommandList* cl = cmdList.Get();
        if (!dev || !cl) return false;

        UpdateTextureUav(dev, outTexture, DXGI_FORMAT_R8G8B8A8_UNORM, kReflectionUavSlot);

        {
            AmbientOcclusionFrameConstants ao{};
            FillAmbientOcclusionConstants(desc, ao);
            memcpy(m_frameConstantsMapped, &ao, sizeof(ao));
        }

        // Bind GBuffer SRVs. The AO shader only reads t6, but the shared root signature
        // exposes t6-t8 as a single table so we populate all three slots.
        auto setRawSrv = [&](ID3D12Resource* resource, DXGI_FORMAT format, UINT slot) {
            D3D12_CPU_DESCRIPTOR_HANDLE dest = m_descHeap.GetCPUDescriptorHandleForHeapStart();
            dest.ptr += slot * m_descIncrementSize;

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(resource, &srvDesc, dest);
        };
        setRawSrv(desc.gbufferNormalTex, DXGI_FORMAT_R16G16B16A16_FLOAT, kGBufferSrvBase + 0u);
        setRawSrv(desc.gbufferMaterialTex, DXGI_FORMAT_R8G8B8A8_UNORM, kGBufferSrvBase + 1u);
        setRawSrv(desc.gbufferAlbedoTex, DXGI_FORMAT_R8G8B8A8_UNORM, kGBufferSrvBase + 2u);

        cl->SetPipelineState(m_aoPso.Get());
        cl->SetComputeRootSignature(m_rootSignature.Get());

        ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootConstantBufferView(0, m_frameConstantsBuffer.GetGPUVirtualAddress());
        cl->SetComputeRootDescriptorTable(1, m_descHeap.GetGPUDescriptorHandleForHeapStart());

        D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
        uavGpu.ptr += kReflectionUavSlot * m_descIncrementSize;
        cl->SetComputeRootDescriptorTable(2, uavGpu);

        D3D12_GPU_DESCRIPTOR_HANDLE gbufGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
        gbufGpu.ptr += kGBufferSrvBase * m_descIncrementSize;
        cl->SetComputeRootDescriptorTable(3, gbufGpu);

        const UINT gx = (desc.width + 15u) / 16u;
        const UINT gy = (desc.height + 15u) / 16u;
        cl->Dispatch(gx, gy, 1u);

        outStats.usingHardwarePath = false;
        outStats.renderWidth = desc.width;
        outStats.renderHeight = desc.height;
        return true;
    }

    // =========================================================================
    // RenderReflectionTextureReSTIR
    // =========================================================================


    bool GpuSoftwareRayTracer::RenderReflectionTexture(const ReflectionTextureDesc& desc,
                                                        IRHIDevice& device,
                                                        CommandList& cmdList,
                                                        Resource& outTexture,
                                                        RayTracingRuntimeStats& outStats)
    {
        if (!m_initialized || !outTexture.IsValid()) return false;
        if (desc.width == 0 || desc.height == 0) return false;

        if (m_swrtMode == SwrtMode::ReSTIR)
        {
            if (m_restirPipelineReady)
            {
                return RenderReflectionTextureReSTIR(desc, device, cmdList, outTexture, outStats);
            }

            OutputDebugStringA("GpuSWRT: ReSTIR mode requested but pipeline is not ready; using legacy reflection.\n");
        }

        (void)outStats;
        if (!m_reflectionPso) return false;

        ID3D12Device* dev = device.GetDevice();
        ID3D12GraphicsCommandList* cl = cmdList.Get();
        if (!dev || !cl) return false;

        // Update UAV slot [7] for reflection output
        UpdateTextureUav(dev, outTexture, DXGI_FORMAT_R16G16B16A16_FLOAT, kReflectionUavSlot);

        // Fill reflection constants into slot 1 of the frame constants buffer (offset 256)
        {
            ReflectionFrameConstants rc{};
            FillReflectionConstants(desc, rc);
            memcpy(m_frameConstantsMapped + 256u, &rc, sizeof(rc));
        }

        // Bind compute pipeline
        cl->SetPipelineState(m_reflectionPso.Get());
        cl->SetComputeRootSignature(m_rootSignature.Get());

        // Bind descriptor heap
        ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
        cl->SetDescriptorHeaps(1, heaps);

        // Root[0]: inline CBV (slot 1 of frame constants buffer)
        cl->SetComputeRootConstantBufferView(0,
            m_frameConstantsBuffer.GetGPUVirtualAddress() + 256u);

        // Root[1]: SRV table (t0-t5)
        cl->SetComputeRootDescriptorTable(1, m_descHeap.GetGPUDescriptorHandleForHeapStart());

        // Root[2]: UAV table (reflection slot)
        D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
        uavGpu.ptr += kReflectionUavSlot * m_descIncrementSize;
        cl->SetComputeRootDescriptorTable(2, uavGpu);

        // Root[3]: G-Buffer SRV table (t6-t8): Normal, Material, Albedo.
        // Populate heap slots kGBufferSrvBase+0..+3 from desc.
        // Null SRVs are written when the texture pointer is null (shader outputs black).
        {
            auto writeGBufSrv = [&](ID3D12Resource* tex, DXGI_FORMAT fmt, UINT slotOffset)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE h = m_descHeap.GetCPUDescriptorHandleForHeapStart();
                h.ptr += (kGBufferSrvBase + slotOffset) * m_descIncrementSize;
                if (tex)
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                    srvDesc.Format                  = fmt;
                    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srvDesc.Texture2D.MipLevels     = 1;
                    dev->CreateShaderResourceView(tex, &srvDesc, h);
                }
                else
                {
                    // Null descriptor: shader reads (0,0,0,0)
                    D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc{};
                    nullDesc.Format                  = fmt;
                    nullDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
                    nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    nullDesc.Texture2D.MipLevels     = 1;
                    dev->CreateShaderResourceView(nullptr, &nullDesc, h);
                }
            };
            auto writeCubeSrv = [&](ID3D12Resource* tex, DXGI_FORMAT fmt, UINT slotOffset, UINT mipLevels)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE h = m_descHeap.GetCPUDescriptorHandleForHeapStart();
                h.ptr += (kGBufferSrvBase + slotOffset) * m_descIncrementSize;
                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Format                  = fmt;
                srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.TextureCube.MostDetailedMip = 0;
                srvDesc.TextureCube.MipLevels       = mipLevels;
                srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
                dev->CreateShaderResourceView(tex, &srvDesc, h);
            };
            writeGBufSrv(desc.gbufferNormalTex,   DXGI_FORMAT_R16G16B16A16_FLOAT, 0u);
            writeGBufSrv(desc.gbufferMaterialTex, DXGI_FORMAT_R8G8B8A8_UNORM,     1u);
            writeGBufSrv(desc.gbufferAlbedoTex,   DXGI_FORMAT_R8G8B8A8_UNORM,     2u);
            writeCubeSrv(desc.iblPrefilterTex,    DXGI_FORMAT_R16G16B16A16_FLOAT, 3u,
                         std::max(1u, static_cast<UINT>(desc.iblPrefilterMaxMip) + 1u));
        }
        D3D12_GPU_DESCRIPTOR_HANDLE gbufGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
        gbufGpu.ptr += kGBufferSrvBase * m_descIncrementSize;
        cl->SetComputeRootDescriptorTable(3, gbufGpu);

        if (m_lightDataMapped && m_lightDataBuffer.IsValid())
        {
            if (desc.frameDesc.pointLights)
            {
                const uint32_t nPt = std::min((uint32_t)desc.frameDesc.pointLights->size(), kMaxPointLights);
                auto* dst = reinterpret_cast<GpuPointLightRT*>(m_lightDataMapped);
                for (uint32_t i = 0; i < nPt; ++i)
                {
                    const auto& src = (*desc.frameDesc.pointLights)[i];
                    dst[i].pos[0] = src.pos[0];
                    dst[i].pos[1] = src.pos[1];
                    dst[i].pos[2] = src.pos[2];
                    dst[i].range  = src.range;
                    dst[i].colorIntensity[0] = src.color[0] * src.intensity;
                    dst[i].colorIntensity[1] = src.color[1] * src.intensity;
                    dst[i].colorIntensity[2] = src.color[2] * src.intensity;
                    dst[i].pad = 0.0f;
                }
            }
            if (desc.frameDesc.spotLights)
            {
                const uint32_t nSp = std::min((uint32_t)desc.frameDesc.spotLights->size(), kMaxSpotLights);
                uint8_t* spotBase = m_lightDataMapped + kMaxPointLights * sizeof(GpuPointLightRT);
                auto* dst = reinterpret_cast<GpuSpotLightRT*>(spotBase);
                for (uint32_t i = 0; i < nSp; ++i)
                {
                    const auto& src = (*desc.frameDesc.spotLights)[i];
                    dst[i].pos[0] = src.pos[0];
                    dst[i].pos[1] = src.pos[1];
                    dst[i].pos[2] = src.pos[2];
                    dst[i].range  = src.range;
                    float dir[3]{};
                    Math::DirectionFromYawPitch(src.yaw, src.pitch, dir);
                    dst[i].dir[0] = dir[0];
                    dst[i].dir[1] = dir[1];
                    dst[i].dir[2] = dir[2];
                    dst[i].cosInner = std::cos(src.innerAngle);
                    dst[i].colorIntensity[0] = src.color[0] * src.intensity;
                    dst[i].colorIntensity[1] = src.color[1] * src.intensity;
                    dst[i].colorIntensity[2] = src.color[2] * src.intensity;
                    dst[i].cosOuter = std::cos(src.outerAngle);
                }
            }

            const D3D12_GPU_VIRTUAL_ADDRESS ptLightsVA = m_lightDataBuffer.GetGPUVirtualAddress();
            const D3D12_GPU_VIRTUAL_ADDRESS spLightsVA =
                ptLightsVA + kMaxPointLights * sizeof(GpuPointLightRT);
            cl->SetComputeRootShaderResourceView(4, ptLightsVA);
            cl->SetComputeRootShaderResourceView(5, spLightsVA);
        }

        // Dispatch (16x16 threads)
        const UINT gx = (desc.width  + 15u) / 16u;
        const UINT gy = (desc.height + 15u) / 16u;
        cl->Dispatch(gx, gy, 1u);

        outStats.usingHardwarePath = false;
        outStats.renderWidth  = desc.width;
        outStats.renderHeight = desc.height;

        // -----------------------------------------------------------------------
        // Temporal EMA pass (Legacy mode only)
        // Blends current-frame reflection with accumulated history to reduce noise.
        // Only runs on full-refresh frames (updatePhaseCount == 1) to avoid
        // blending stale partial-update pixels into the history.
        // -----------------------------------------------------------------------
        if (desc.denoiserEnabled && m_reflTemporalPso && desc.updatePhaseCount == 1u)
        {
            bool firstFrame = (m_reflHistoryWidth  != desc.width ||
                               m_reflHistoryHeight != desc.height);

                // Lazily allocate / reallocate ping-pong history textures on size change
                if (firstFrame)
                {
                    const auto kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    m_reflHistoryA.Reset();
                    m_reflHistoryB.Reset();
                    m_reflHistoryMetaA.Reset();
                    m_reflHistoryMetaB.Reset();
                    m_reflHistoryMaterialA.Reset();
                    m_reflHistoryMaterialB.Reset();
                    m_reflAtrousScratch.Reset();
                    const bool allocOk =
                        CreateUavTexture2D(device, desc.width, desc.height,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_reflHistoryA) &&
                        CreateUavTexture2D(device, desc.width, desc.height,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_reflHistoryB) &&
                        CreateUavTexture2D(device, desc.width, desc.height,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_reflHistoryMetaA) &&
                        CreateUavTexture2D(device, desc.width, desc.height,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_reflHistoryMetaB) &&
                        CreateUavTexture2D(device, desc.width, desc.height,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_reflHistoryMaterialA) &&
                        CreateUavTexture2D(device, desc.width, desc.height,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_reflHistoryMaterialB) &&
                        CreateUavTexture2D(device, desc.width, desc.height,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_reflAtrousScratch);
                if (allocOk)
                {
                    m_reflHistoryWidth  = desc.width;
                    m_reflHistoryHeight = desc.height;
                    m_reflHistoryPingA  = true;
                }
                else
                {
                    firstFrame = false;  // prevent temporal dispatch below
                    m_reflHistoryWidth = m_reflHistoryHeight = 0;
                }
            }

            if (m_reflHistoryA.IsValid() && m_reflHistoryB.IsValid() &&
                m_reflHistoryMetaA.IsValid() && m_reflHistoryMetaB.IsValid() &&
                m_reflHistoryMaterialA.IsValid() && m_reflHistoryMaterialB.IsValid())
            {
                // Ping-pong assignment: one side is read (SRV), the other is written (UAV)
                Resource& histRead  = m_reflHistoryPingA ? m_reflHistoryB : m_reflHistoryA;
                Resource& histWrite = m_reflHistoryPingA ? m_reflHistoryA : m_reflHistoryB;
                Resource& metaRead  = m_reflHistoryPingA ? m_reflHistoryMetaB : m_reflHistoryMetaA;
                Resource& metaWrite = m_reflHistoryPingA ? m_reflHistoryMetaA : m_reflHistoryMetaB;
                Resource& materialRead  = m_reflHistoryPingA ? m_reflHistoryMaterialB : m_reflHistoryMaterialA;
                Resource& materialWrite = m_reflHistoryPingA ? m_reflHistoryMaterialA : m_reflHistoryMaterialB;

                // Keep some history during camera motion instead of fully resetting.
                // A hard reset exposes the raw 1spp/low-res reflection and causes
                // black flicker while the camera is being shaken.
                const float alpha = firstFrame ? 1.0f : (desc.cameraChanged ? 0.35f : desc.temporalAlpha);

                // ---- Barriers: prepare textures for temporal CS ----
                // outTexture: UAV (just written by reflection CS) → NON_PIXEL_SHADER_RESOURCE (SRV t0)
                // histWrite:  NON_PIXEL_SHADER_RESOURCE → UAV (u0)
                D3D12_RESOURCE_BARRIER preBarriers[4] = {
                    CD3DX12_RESOURCE_BARRIER::UAV(outTexture.Get()),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        histWrite.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        metaWrite.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        materialWrite.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                };
                cl->ResourceBarrier(_countof(preBarriers), preBarriers);

                // Transition outTexture UAV → SRV (done after UAV barrier above)
                D3D12_RESOURCE_BARRIER outToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                    outTexture.Get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                cl->ResourceBarrier(1u, &outToSrv);

                // ---- Populate temporal descriptor slots (14-22) ----
                // Slot 14: SRV → outTexture   (t0 = current frame)
                // Slot 15: SRV → histRead     (t1 = previous EMA)
                // Slot 16: SRV → current meta (t2 = current surface metadata)
                // Slot 17: SRV → history meta (t3 = previous surface metadata)
                // Slot 18: SRV → current material (t4 = current material metadata)
                // Slot 19: SRV → history material (t5 = previous material metadata)
                // Slot 20: UAV → histWrite    (u0 = EMA write)
                // Slot 21: UAV → metaWrite    (u1 = surface metadata write)
                // Slot 22: UAV → materialWrite (u2 = material metadata write)
                {
                    auto cpuBase = m_descHeap.GetCPUDescriptorHandleForHeapStart();
                    auto writeSrv = [&](ID3D12Resource* res, DXGI_FORMAT fmt, UINT slot)
                    {
                        D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
                        h.ptr += slot * m_descIncrementSize;
                        CreateTex2DSrv(dev, res, fmt, h);
                    };
                    auto writeUav = [&](ID3D12Resource* res, DXGI_FORMAT fmt, UINT slot)
                    {
                        D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
                        h.ptr += slot * m_descIncrementSize;
                        CreateTex2DUav(dev, res, fmt, h);
                    };

                    writeSrv(outTexture.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalSrvBase + 0u);
                    writeSrv(histRead.Get(),   DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalSrvBase + 1u);
                    writeSrv(desc.gbufferNormalTex, DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalSrvBase + 2u);
                    writeSrv(metaRead.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalSrvBase + 3u);
                    writeSrv(desc.gbufferMaterialTex, DXGI_FORMAT_R8G8B8A8_UNORM, kTemporalSrvBase + 4u);
                    writeSrv(materialRead.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalSrvBase + 5u);
                    writeUav(histWrite.Get(),  DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalUavBase + 0u);
                    writeUav(metaWrite.Get(),  DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalUavBase + 1u);
                    writeUav(materialWrite.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalUavBase + 2u);
                }

                // ---- Bind temporal pipeline ----
                cl->SetPipelineState(m_reflTemporalPso.Get());
                cl->SetComputeRootSignature(m_reflTemporalRootSignature.Get());
                // Descriptor heap is already bound from the reflection CS dispatch above

                // Root[0]: constants (alpha, reflection size, validation flags, GBuffer size)
                {
                    const uint32_t gbufferWidth = desc.gbufferWidth != 0u ? desc.gbufferWidth : desc.width;
                    const uint32_t gbufferHeight = desc.gbufferHeight != 0u ? desc.gbufferHeight : desc.height;
                    ReflectionTemporalReprojectionConstants reprojection{};
                    memcpy(reprojection.invVP, desc.frameDesc.inverseViewProjection, 64);
                    memcpy(reprojection.prevVP, m_prevVP, 64);
                    reprojection.cameraPos[0] = desc.frameDesc.cameraPosition[0];
                    reprojection.cameraPos[1] = desc.frameDesc.cameraPosition[1];
                    reprojection.cameraPos[2] = desc.frameDesc.cameraPosition[2];
                    reprojection.prevCameraPos[0] = m_prevReflectionCameraPos[0];
                    reprojection.prevCameraPos[1] = m_prevReflectionCameraPos[1];
                    reprojection.prevCameraPos[2] = m_prevReflectionCameraPos[2];
                    if (m_restirConstantsMapped)
                    {
                        memcpy(m_restirConstantsMapped, &reprojection, sizeof(reprojection));
                    }

                    uint32_t constants[7] = {
                        *reinterpret_cast<const uint32_t*>(&alpha),
                        desc.width,
                        desc.height,
                        desc.gbufferNormalTex ? 1u : 0u,
                        gbufferWidth,
                        gbufferHeight,
                        desc.gbufferMaterialTex ? 1u : 0u
                    };
                    cl->SetComputeRoot32BitConstants(0, 7, constants, 0);
                }

                // Root[1]: SRV table starting at slot 14
                D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
                srvGpu.ptr += kTemporalSrvBase * m_descIncrementSize;
                cl->SetComputeRootDescriptorTable(1, srvGpu);

                // Root[2]: UAV table starting at slot 20
                D3D12_GPU_DESCRIPTOR_HANDLE uavGpu2 = m_descHeap.GetGPUDescriptorHandleForHeapStart();
                uavGpu2.ptr += kTemporalUavBase * m_descIncrementSize;
                cl->SetComputeRootDescriptorTable(2, uavGpu2);

                // Root[3]: reprojection constants (b1)
                if (m_restirConstantsBuffer.IsValid())
                {
                    cl->SetComputeRootConstantBufferView(3, m_restirConstantsBuffer.GetGPUVirtualAddress());
                }

                // ---- Dispatch ----
                const UINT tgx = (desc.width  + 15u) / 16u;
                const UINT tgy = (desc.height + 15u) / 16u;
                cl->Dispatch(tgx, tgy, 1u);

                // ---- Copy EMA result (histWrite) back into outTexture ----
                // Barriers: histWrite UAV→COPY_SOURCE, outTexture SRV→COPY_DEST
                D3D12_RESOURCE_BARRIER copyPre[4] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        histWrite.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        outTexture.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        metaWrite.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        materialWrite.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                };
                cl->ResourceBarrier(_countof(copyPre), copyPre);

                cl->CopyResource(outTexture.Get(), histWrite.Get());

                // Restore states: outTexture→UAV (SWRTExecutor will transition UAV→PSR),
                //                 histWrite/metaWrite→NON_PIXEL_SHADER_RESOURCE (next frame's SRVs)
                D3D12_RESOURCE_BARRIER copyPost[2] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        outTexture.Get(),
                        D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        histWrite.Get(),
                        D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                };
                cl->ResourceBarrier(_countof(copyPost), copyPost);

                // Advance ping-pong
                m_reflHistoryPingA = !m_reflHistoryPingA;
            } // end if histories valid
        } // end temporal EMA block

        if (desc.denoiserEnabled &&
            m_reflAtrousPso &&
            m_reflAtrousScratch.IsValid() &&
            desc.gbufferNormalTex &&
            desc.gbufferMaterialTex &&
            desc.updatePhaseCount == 1u &&
            desc.atrousIterations > 0u)
        {
            auto cpuBase = m_descHeap.GetCPUDescriptorHandleForHeapStart();
            auto writeSrv = [&](ID3D12Resource* res, DXGI_FORMAT fmt, UINT slot)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
                h.ptr += slot * m_descIncrementSize;
                CreateTex2DSrv(dev, res, fmt, h);
            };
            auto writeUav = [&](ID3D12Resource* res, DXGI_FORMAT fmt, UINT slot)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
                h.ptr += slot * m_descIncrementSize;
                CreateTex2DUav(dev, res, fmt, h);
            };

            ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetPipelineState(m_reflAtrousPso.Get());
            cl->SetComputeRootSignature(m_reflAtrousRootSignature.Get());

            const uint32_t iterations = std::min(desc.atrousIterations, 5u);
            for (uint32_t iter = 0; iter < iterations; ++iter)
            {
                writeSrv(outTexture.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalSrvBase + 0u);
                writeSrv(desc.gbufferNormalTex, DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalSrvBase + 1u);
                writeSrv(desc.gbufferMaterialTex, DXGI_FORMAT_R8G8B8A8_UNORM, kTemporalSrvBase + 2u);
                writeUav(m_reflAtrousScratch.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalUavBase + 0u);

                D3D12_RESOURCE_BARRIER pre[] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        outTexture.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        m_reflAtrousScratch.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                };
                cl->ResourceBarrier(_countof(pre), pre);

                const float stepWidth = static_cast<float>(1u << iter);
                const uint32_t gbufferWidth = desc.gbufferWidth != 0u ? desc.gbufferWidth : desc.width;
                const uint32_t gbufferHeight = desc.gbufferHeight != 0u ? desc.gbufferHeight : desc.height;
                uint32_t constants[6] = {
                    *reinterpret_cast<const uint32_t*>(&stepWidth),
                    desc.width,
                    desc.height,
                    *reinterpret_cast<const uint32_t*>(&desc.atrousPhiDepth),
                    gbufferWidth,
                    gbufferHeight,
                };
                cl->SetComputeRoot32BitConstants(0, 6, constants, 0);

                D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
                srvGpu.ptr += kTemporalSrvBase * m_descIncrementSize;
                cl->SetComputeRootDescriptorTable(1, srvGpu);

                D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
                uavGpu.ptr += kTemporalUavBase * m_descIncrementSize;
                cl->SetComputeRootDescriptorTable(2, uavGpu);

                const UINT tgx = (desc.width  + 15u) / 16u;
                const UINT tgy = (desc.height + 15u) / 16u;
                cl->Dispatch(tgx, tgy, 1u);

                D3D12_RESOURCE_BARRIER copyPre[] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        m_reflAtrousScratch.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        outTexture.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST),
                };
                cl->ResourceBarrier(_countof(copyPre), copyPre);
                cl->CopyResource(outTexture.Get(), m_reflAtrousScratch.Get());

                D3D12_RESOURCE_BARRIER copyPost[] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        m_reflAtrousScratch.Get(),
                        D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        outTexture.Get(),
                        D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                };
                cl->ResourceBarrier(_countof(copyPost), copyPost);
            }
        }

        // Legacy SWRT also uses this frame index for per-frame GGX jitter.
        // Without advancing it, rough reflections keep reusing the same sample.
        ++m_restirFrameIndex;
        Math::Invert4x4(desc.frameDesc.inverseViewProjection, m_prevVP);
        m_prevReflectionCameraPos[0] = desc.frameDesc.cameraPosition[0];
        m_prevReflectionCameraPos[1] = desc.frameDesc.cameraPosition[1];
        m_prevReflectionCameraPos[2] = desc.frameDesc.cameraPosition[2];
        return true;
    }


} // namespace SasamiRenderer
