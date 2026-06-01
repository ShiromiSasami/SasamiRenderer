// GpuSoftwareRayTracer_ReSTIR.cpp
// ReSTIR reflection and shadow rendering passes.
#define NOMINMAX
#include "Renderer/RayTracing/GpuSoftwareRayTracer.h"

#include "Renderer/Scene/RenderLightProxy.h"
#include "Renderer/Scene/LightSystem.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"

#include "Foundation/Math/MathUtil.h"

// NRD/NRI declarations (NRDIntegration.hpp included only in GpuSoftwareRayTracer.cpp)
#include "NRI.h"
#include "NRIDeviceCreation.h"
#include "Extensions/NRIWrapperD3D12.h"
#include "Extensions/NRIHelper.h"
#include "NRD.h"
#include "NRDIntegration.h"

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


    bool GpuSoftwareRayTracer::RenderReflectionTextureReSTIR(
                                                        const ReflectionTextureDesc& desc,
                                                        IRHIDevice& device,
                                                        CommandList& cmdList,
                                                        Resource& outTexture,
                                                        RayTracingRuntimeStats& outStats)
    {
        (void)outStats;
        if (!m_restirPipelineReady || !outTexture.IsValid()) return false;
        if (desc.width == 0 || desc.height == 0) return false;

        ID3D12Device*              dev = device.GetDevice();
        ID3D12GraphicsCommandList* cl  = cmdList.Get();
        if (!dev || !cl) return false;

        // ---- Reallocate internal buffers if resolution changed ----
        if (desc.width != m_restirWidth || desc.height != m_restirHeight)
        {
            if (!AllocateReSTIRBuffers(device, desc.width, desc.height)) return false;
        }

        // ---- NRD denoiser integration ----
        // The NRI/NRD D3D12 integration can abort inside the external SDK on
        // some debug/runtime combinations during RecreateD3D12. Keep ReSTIR
        // executable by default and fall back to the raw shaded result below.
        constexpr bool kEnableReSTIRNrdDenoiser = false;
        if (kEnableReSTIRNrdDenoiser && !m_nrdReady && m_restirWidth > 0 && m_restirHeight > 0)
        {
            InitializeNrdIntegration(device, m_restirWidth, m_restirHeight);
        }

        // ---- Upload light data (persistently-mapped upload buffer) ----
        uint32_t nPt = 0, nSp = 0;
        if (desc.frameDesc.pointLights)
        {
            nPt = std::min((uint32_t)desc.frameDesc.pointLights->size(), kMaxPointLights);
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
            nSp = std::min((uint32_t)desc.frameDesc.spotLights->size(), kMaxSpotLights);
            uint8_t* spotBase = m_lightDataMapped
                + kMaxPointLights * sizeof(GpuPointLightRT);
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
                dst[i].cosInner       = std::cos(src.innerAngle);
                dst[i].colorIntensity[0] = src.color[0] * src.intensity;
                dst[i].colorIntensity[1] = src.color[1] * src.intensity;
                dst[i].colorIntensity[2] = src.color[2] * src.intensity;
                dst[i].cosOuter       = std::cos(src.outerAngle);
            }
        }

        // ---- Fill constant buffer slots ----
        {
            ReSTIRFrameConstants base{};
            FillReSTIRConstants(desc, 1.0f, base);
            base.pointLightCount = nPt;
            base.spotLightCount  = nSp;
            memcpy(m_restirConstantsMapped, &base, sizeof(base));

            static const float kSteps[5] = { 1.f, 2.f, 4.f, 8.f, 16.f };
            for (int i = 0; i < 5; ++i)
            {
                ReSTIRFrameConstants ac{};
                FillReSTIRConstants(desc, kSteps[i], ac);
                ac.pointLightCount = nPt;
                ac.spotLightCount  = nSp;
                memcpy(m_restirConstantsMapped + (UINT64)(i + 1) * 256, &ac, sizeof(ac));
            }
            // Slot 6: shadow ReSTIR (same as base)
            memcpy(m_restirConstantsMapped + 6u * 256,
                   m_restirConstantsMapped, sizeof(ReSTIRFrameConstants));
        }

        // ---- Common heap / root state ----
        const UINT gx = (desc.width  + 15u) / 16u;
        const UINT gy = (desc.height + 15u) / 16u;

        ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(m_restirRootSignature.Get());

        const D3D12_GPU_DESCRIPTOR_HANDLE heapGpuStart =
            m_descHeap.GetGPUDescriptorHandleForHeapStart();

        D3D12_GPU_DESCRIPTOR_HANDLE bvhGpu = heapGpuStart;           // slots 0-5

        const D3D12_GPU_VIRTUAL_ADDRESS cbBase =
            m_restirConstantsBuffer.GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS ptLightsVA =
            m_lightDataBuffer.GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS spLightsVA =
            ptLightsVA + kMaxPointLights * sizeof(GpuPointLightRT);

        constexpr auto kSrv     = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        constexpr auto kUav     = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        constexpr auto kCopySrc = D3D12_RESOURCE_STATE_COPY_SOURCE;
        constexpr auto kCopyDst = D3D12_RESOURCE_STATE_COPY_DEST;
        constexpr D3D12_GPU_VIRTUAL_ADDRESS kNullVA = 0ull;

        // Per-pass descriptor slot helpers.
        // Slots are double-buffered by frame index so CPU can write frame N's descriptors
        // while the GPU is still executing frame N-1's dispatches, with no race condition
        // and no WaitForGPU required.
        // Frame slot layout: frameSlot s, pass p starts at kReSTIRPassDescBase
        // + s * passCount * stride + p * stride; SRV[0..3], UAV[4..6].
        const UINT frameSlot = m_restirFrameIndex & 1u;
        const UINT frameSlotOffset = frameSlot * kReSTIRPassCount * kReSTIRPassDescStride;
        auto PassSrvBase = [&](int p) -> UINT {
            return kReSTIRPassDescBase + frameSlotOffset + static_cast<UINT>(p) * kReSTIRPassDescStride;
        };
        auto PassUavBase = [&](int p) -> UINT {
            return PassSrvBase(p) + 4u;
        };
        auto PassSrvGpu = [&](int p) -> D3D12_GPU_DESCRIPTOR_HANDLE {
            D3D12_GPU_DESCRIPTOR_HANDLE h = heapGpuStart;
            h.ptr += PassSrvBase(p) * m_descIncrementSize;
            return h;
        };
        auto PassUavGpu = [&](int p) -> D3D12_GPU_DESCRIPTOR_HANDLE {
            D3D12_GPU_DESCRIPTOR_HANDLE h = heapGpuStart;
            h.ptr += PassUavBase(p) * m_descIncrementSize;
            return h;
        };
        auto WritePassSRV = [&](int p, UINT s, Resource& tex, DXGI_FORMAT fmt) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = m_descHeap.GetCPUDescriptorHandleForHeapStart();
            h.ptr += (PassSrvBase(p) + s) * m_descIncrementSize;
            CreateTex2DSrv(dev, tex.Get(), fmt, h);
        };
        auto WritePassSRVRaw = [&](int p, UINT s, ID3D12Resource* tex, DXGI_FORMAT fmt) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = m_descHeap.GetCPUDescriptorHandleForHeapStart();
            h.ptr += (PassSrvBase(p) + s) * m_descIncrementSize;
            CreateTex2DSrv(dev, tex, fmt, h);
        };
        auto WritePassUAV = [&](int p, UINT s, Resource& tex, DXGI_FORMAT fmt) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = m_descHeap.GetCPUDescriptorHandleForHeapStart();
            h.ptr += (PassUavBase(p) + s) * m_descIncrementSize;
            CreateTex2DUav(dev, tex.Get(), fmt, h);
        };

        // ---- Pre-write ALL per-pass descriptors before recording any dispatches ----
        // SetComputeRootDescriptorTable records a GPU heap pointer; the GPU reads the
        // actual descriptor at ExecuteCommandLists time. If multiple passes share the
        // same heap slots and the CPU overwrites them between SetDescriptorTable calls,
        // every pass sees the last-written value. Assigning unique slots per pass and
        // writing them all up front makes each pass reference stable, independent data.

        // Pass 0 (Initial): SRV[0..2]=raster GBuffer, UAV[0]=secondary GBuffer,
        // UAV[1]=secondary hit position, UAV[2]=secondary hit material.
        if (desc.gbufferNormalTex)
            WritePassSRVRaw(0, 0u, desc.gbufferNormalTex, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (desc.gbufferMaterialTex)
            WritePassSRVRaw(0, 1u, desc.gbufferMaterialTex, DXGI_FORMAT_R8G8B8A8_UNORM);
        if (desc.gbufferAlbedoTex)
            WritePassSRVRaw(0, 2u, desc.gbufferAlbedoTex, DXGI_FORMAT_R8G8B8A8_UNORM);
        WritePassUAV(0, 0u, m_gBufferTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassUAV(0, 1u, m_hitPositionTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassUAV(0, 2u, m_hitMaterialTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        // Pass 1 (Temporal): SRV[0]=gBuffer, SRV[1]=prevGBuffer, SRV[3]=hitPosition
        WritePassSRV(1, 0u, m_gBufferTexture,     DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassSRV(1, 1u, m_prevGBufferTexture,  DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassSRV(1, 3u, m_hitPositionTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        // Pass 2 (Spatial): SRV[0]=gBuffer, SRV[1]=hitMaterial, SRV[2]=hitPosition
        WritePassSRV(2, 0u, m_gBufferTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassSRV(2, 1u, m_hitMaterialTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassSRV(2, 2u, m_hitPositionTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        // Pass 3 (Shade): SRV[0]=gBuffer, SRV[1]=hitMaterial, SRV[3]=hitPosition; UAV[0]=shadedColor
        WritePassSRV(3, 0u, m_gBufferTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassSRV(3, 1u, m_hitMaterialTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassSRV(3, 3u, m_hitPositionTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassUAV(3, 0u, m_shadedColorTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        // Pass 4 (Shade temporal accumulate into prevColor): SRV[0]=shadedColor, SRV[1]=prevColor
        WritePassSRV(4, 0u, m_shadedColorTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassSRV(4, 1u, m_prevColorTexture,   DXGI_FORMAT_R16G16B16A16_FLOAT);
        // Pass 5 (NRD Pack): SRV[0]=shadedColor, SRV[1]=gBuffer, SRV[2]=material
        //                     UAV[0]=nrdDiffIn, UAV[1]=nrdViewZ, UAV[2]=nrdNormalRoughness
        WritePassSRV(5, 0u, m_shadedColorTexture,  DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassSRV(5, 1u, m_gBufferTexture,       DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassSRV(5, 2u, m_hitMaterialTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassUAV(5, 0u, m_nrdDiffIn,           DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassUAV(5, 1u, m_nrdViewZ,             DXGI_FORMAT_R16_FLOAT);
        WritePassUAV(5, 2u, m_nrdNormalRoughness,   DXGI_FORMAT_R8G8B8A8_UNORM);
        // Pass 6 (A-Trous ping→pong): SRV[0]=nrdDiffOut, SRV[1]=gBuffer; UAV[0]=atrousPingPong
        if (m_atrousReady)
        {
            WritePassSRV(6, 0u, m_nrdDiffOut,      DXGI_FORMAT_R16G16B16A16_FLOAT);
            WritePassSRV(6, 1u, m_gBufferTexture,   DXGI_FORMAT_R16G16B16A16_FLOAT);
            WritePassUAV(6, 0u, m_atrousPingPong,   DXGI_FORMAT_R16G16B16A16_FLOAT);
            // Pass 7 (A-Trous pong→ping): SRV[0]=atrousPingPong, SRV[1]=gBuffer; UAV[0]=nrdDiffOut
            WritePassSRV(7, 0u, m_atrousPingPong,   DXGI_FORMAT_R16G16B16A16_FLOAT);
            WritePassSRV(7, 1u, m_gBufferTexture,   DXGI_FORMAT_R16G16B16A16_FLOAT);
            WritePassUAV(7, 0u, m_nrdDiffOut,       DXGI_FORMAT_R16G16B16A16_FLOAT);
        }

        // Helper: bind all 9 root parameters with per-pass descriptor tables
        auto BindRoots = [&](D3D12_GPU_VIRTUAL_ADDRESS cbVA,
                             D3D12_GPU_VIRTUAL_ADDRESS lightPtVA,
                             D3D12_GPU_VIRTUAL_ADDRESS lightSpVA,
                             D3D12_GPU_VIRTUAL_ADDRESS reservoirInVA,
                             D3D12_GPU_VIRTUAL_ADDRESS reservoirOutVA,
                             D3D12_GPU_DESCRIPTOR_HANDLE passSrvGpuH,
                             D3D12_GPU_DESCRIPTOR_HANDLE passUavGpuH,
                             D3D12_GPU_VIRTUAL_ADDRESS prevTemporalVA)
        {
            cl->SetComputeRootConstantBufferView (0, cbVA);
            cl->SetComputeRootDescriptorTable    (1, bvhGpu);
            cl->SetComputeRootDescriptorTable    (2, passSrvGpuH);
            cl->SetComputeRootDescriptorTable    (3, passUavGpuH);
            cl->SetComputeRootShaderResourceView (4, lightPtVA);
            cl->SetComputeRootShaderResourceView (5, lightSpVA);
            cl->SetComputeRootShaderResourceView (6, reservoirInVA);
            cl->SetComputeRootUnorderedAccessView(7, reservoirOutVA);
            cl->SetComputeRootShaderResourceView (8, prevTemporalVA);
        };

        // =========================================================================
        // Pass 1: GBuffer capture + Initial Reservoir (WRS M=8)
        // u0 = gBuffer (scratch UAV[0]),  u3 = reservoirBuffer[0] (inline UAV)
        // t12/t13 = lights,  BVH t0-t5 used for primary rays
        // =========================================================================
        {
            D3D12_RESOURCE_BARRIER pre[] = {
                MakeTransition(m_gBufferTexture.Get(),     kSrv, kUav),
                MakeTransition(m_hitPositionTexture.Get(), kSrv, kUav),
                MakeTransition(m_hitMaterialTexture.Get(), kSrv, kUav),
                MakeTransition(m_reservoirBuffer[0].Get(), kSrv, kUav),
            };
            cl->ResourceBarrier(4, pre);

            cl->SetPipelineState(m_restirInitialPso.Get());
            BindRoots(cbBase, ptLightsVA, spLightsVA,
                      kNullVA, m_reservoirBuffer[0].GetGPUVirtualAddress(),
                      PassSrvGpu(0), PassUavGpu(0), kNullVA);
            cl->Dispatch(gx, gy, 1u);

            D3D12_RESOURCE_BARRIER post[] = {
                MakeTransition(m_gBufferTexture.Get(),     kUav, kSrv),
                MakeTransition(m_hitPositionTexture.Get(), kUav, kSrv),
                MakeTransition(m_hitMaterialTexture.Get(), kUav, kSrv),
                MakeTransition(m_reservoirBuffer[0].Get(), kUav, kSrv),
            };
            cl->ResourceBarrier(4, post);
        }

        // =========================================================================
        // Pass 2: Temporal Reservoir Reuse  (ping-pong fix)
        // t6  = gBuffer (scratch SRV[0]),  t7 = prevGBuffer (scratch SRV[1])
        // t14 = reservoirBuffer[0] (current-frame initial, inline SRV)
        // t15 = reservoirBuffer[prevTemporalIdx] (prev-frame temporal output, inline SRV)
        // u3  = reservoirBuffer[temporalWriteIdx] (temporal output, inline UAV)
        //
        // Ping-pong:  even frame → write [2], prev = [1]
        //             odd  frame → write [1], prev = [2]
        // =========================================================================
        {
            const uint32_t temporalWriteIdx = (m_restirFrameIndex % 2u == 0u) ? 2u : 1u;
            const uint32_t prevTemporalIdx  = (m_restirFrameIndex % 2u == 0u) ? 1u : 2u;

            D3D12_RESOURCE_BARRIER pre[] = {
                MakeTransition(m_reservoirBuffer[temporalWriteIdx].Get(), kSrv, kUav),
            };
            cl->ResourceBarrier(1, pre);

            cl->SetPipelineState(m_restirTemporalPso.Get());
            BindRoots(cbBase, kNullVA, kNullVA,
                      m_reservoirBuffer[0].GetGPUVirtualAddress(),
                      m_reservoirBuffer[temporalWriteIdx].GetGPUVirtualAddress(),
                      PassSrvGpu(1), PassUavGpu(1),
                      m_reservoirBuffer[prevTemporalIdx].GetGPUVirtualAddress());
            cl->Dispatch(gx, gy, 1u);

            D3D12_RESOURCE_BARRIER post[] = {
                MakeTransition(m_reservoirBuffer[temporalWriteIdx].Get(), kUav, kSrv),
            };
            cl->ResourceBarrier(1, post);

        // =========================================================================
        // Pass 3: Spatial Reservoir Reuse
        // t6  = gBuffer (scratch SRV[0])
        // t14 = reservoirBuffer[temporalWriteIdx] (temporal output, inline SRV)
        // u3  = reservoirBuffer[0] (spatial output -> shade input, inline UAV)
        // =========================================================================
            D3D12_RESOURCE_BARRIER pre3[] = {
                MakeTransition(m_reservoirBuffer[0].Get(), kSrv, kUav),
            };
            cl->ResourceBarrier(1, pre3);

            cl->SetPipelineState(m_restirSpatialPso.Get());
            BindRoots(cbBase, kNullVA, kNullVA,
                      m_reservoirBuffer[temporalWriteIdx].GetGPUVirtualAddress(),
                      m_reservoirBuffer[0].GetGPUVirtualAddress(),
                      PassSrvGpu(2), PassUavGpu(2), kNullVA);
            cl->Dispatch(gx, gy, 1u);

            D3D12_RESOURCE_BARRIER post3[] = {
                MakeTransition(m_reservoirBuffer[0].Get(), kUav, kSrv),
            };
            cl->ResourceBarrier(1, post3);
        }

        // =========================================================================
        // Pass 4: Final Shading (shadow ray + PBR)
        // t6 = gBuffer (scratch SRV[0]),  u0 = shadedColor (scratch UAV[0])
        // t12/t13 = lights,  t14 = reservoirBuffer[0] (inline SRV)
        // =========================================================================
        {
            D3D12_RESOURCE_BARRIER pre[] = {
                MakeTransition(m_shadedColorTexture.Get(), kSrv, kUav),
            };
            cl->ResourceBarrier(1, pre);

            cl->SetPipelineState(m_restirShadePso.Get());
            BindRoots(cbBase, ptLightsVA, spLightsVA,
                      m_reservoirBuffer[0].GetGPUVirtualAddress(),
                      kNullVA,
                      PassSrvGpu(3), PassUavGpu(3), kNullVA);
            cl->Dispatch(gx, gy, 1u);

            D3D12_RESOURCE_BARRIER post[] = {
                MakeTransition(m_shadedColorTexture.Get(), kUav, kSrv),
            };
            cl->ResourceBarrier(1, post);
        }

        // =========================================================================
        // Pass 5: NRD Pack – convert shaded color + GBuffer into NRD input textures
        // t6 = shadedColor  t7 = gBuffer  t8 = material
        // u0 = nrdDiffIn   u1 = nrdViewZ  u2 = nrdNormalRoughness
        // =========================================================================
        {
            D3D12_RESOURCE_BARRIER pre[] = {
                MakeTransition(m_nrdDiffIn.Get(),          kSrv, kUav),
                MakeTransition(m_nrdViewZ.Get(),            kSrv, kUav),
                MakeTransition(m_nrdNormalRoughness.Get(),  kSrv, kUav),
            };
            cl->ResourceBarrier(3, pre);

            cl->SetPipelineState(m_nrdPackPso.Get());
            BindRoots(cbBase, kNullVA, kNullVA, kNullVA, kNullVA,
                      PassSrvGpu(5), PassUavGpu(5), kNullVA);
            cl->Dispatch(gx, gy, 1u);

            D3D12_RESOURCE_BARRIER post[] = {
                MakeTransition(m_nrdDiffIn.Get(),          kUav, kSrv),
                MakeTransition(m_nrdViewZ.Get(),            kUav, kSrv),
                MakeTransition(m_nrdNormalRoughness.Get(),  kUav, kSrv),
            };
            cl->ResourceBarrier(3, post);
        }

        // =========================================================================
        // NRD RELAX_DIFFUSE denoise
        // =========================================================================
        if (m_nrdReady && m_nrdIntegration)
        {
            m_nrdIntegration->NewFrame();

            nrd::CommonSettings commonSettings = {};
            // Provide identity matrices – proper view/projection matrices improve quality
            // but NRD works without them (temporal accumulation is degraded only).
            commonSettings.frameIndex = m_restirFrameIndex;
            commonSettings.resourceSize[0]     = static_cast<uint16_t>(desc.width);
            commonSettings.resourceSize[1]     = static_cast<uint16_t>(desc.height);
            commonSettings.resourceSizePrev[0] = static_cast<uint16_t>(desc.width);
            commonSettings.resourceSizePrev[1] = static_cast<uint16_t>(desc.height);
            commonSettings.rectSize[0]     = static_cast<uint16_t>(desc.width);
            commonSettings.rectSize[1]     = static_cast<uint16_t>(desc.height);
            commonSettings.rectSizePrev[0] = static_cast<uint16_t>(desc.width);
            commonSettings.rectSizePrev[1] = static_cast<uint16_t>(desc.height);
            commonSettings.motionVectorScale[0] = 1.0f;
            commonSettings.motionVectorScale[1] = 1.0f;
            commonSettings.motionVectorScale[2] = 0.0f; // 2D screen-space MV
            if (desc.cameraChanged)
                commonSettings.accumulationMode = nrd::AccumulationMode::RESTART;

            m_nrdIntegration->SetCommonSettings(commonSettings);

            nrd::RelaxSettings relaxSettings = {};
            m_nrdIntegration->SetDenoiserSettings(0u, &relaxSettings);

            // Transition nrdDiffOut to UAV so NRD can write to it
            {
                D3D12_RESOURCE_BARRIER b = MakeTransition(m_nrdDiffOut.Get(), kSrv, kUav);
                cl->ResourceBarrier(1, &b);
            }

            nrd::ResourceSnapshot resourceSnapshot;
            resourceSnapshot.restoreInitialState = false;

            auto MakeNrdResource = [](ID3D12Resource* res, DXGI_FORMAT fmt,
                                      nri::AccessBits access, nri::Layout layout,
                                      nri::StageBits stages) -> nrd::Resource
            {
                nrd::Resource r{};
                r.d3d12.resource = res;
                r.d3d12.format   = static_cast<DXGIFormat>(fmt);
                r.state.access   = access;
                r.state.layout   = layout;
                r.state.stages   = stages;
                return r;
            };

            constexpr auto kSrvAccess  = nri::AccessBits::SHADER_RESOURCE;
            constexpr auto kUavAccess  = nri::AccessBits::SHADER_RESOURCE_STORAGE;
            constexpr auto kSrvLayout  = nri::Layout::SHADER_RESOURCE;
            constexpr auto kUavLayout  = nri::Layout::SHADER_RESOURCE_STORAGE;
            constexpr auto kCompStage  = nri::StageBits::COMPUTE_SHADER;

            nrd::Resource resDiffIn = MakeNrdResource(
                m_nrdDiffIn.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                kSrvAccess, kSrvLayout, kCompStage);
            nrd::Resource resViewZ = MakeNrdResource(
                m_nrdViewZ.Get(), DXGI_FORMAT_R16_FLOAT,
                kSrvAccess, kSrvLayout, kCompStage);
            nrd::Resource resNormRough = MakeNrdResource(
                m_nrdNormalRoughness.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                kSrvAccess, kSrvLayout, kCompStage);
            nrd::Resource resMV = MakeNrdResource(
                m_nrdMotionVec.Get(), DXGI_FORMAT_R16G16_FLOAT,
                kSrvAccess, kSrvLayout, kCompStage);
            nrd::Resource resDiffOut = MakeNrdResource(
                m_nrdDiffOut.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                kUavAccess, kUavLayout, kCompStage);

            resourceSnapshot.SetResource(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, resDiffIn);
            resourceSnapshot.SetResource(nrd::ResourceType::IN_VIEWZ,                 resViewZ);
            resourceSnapshot.SetResource(nrd::ResourceType::IN_NORMAL_ROUGHNESS,      resNormRough);
            resourceSnapshot.SetResource(nrd::ResourceType::IN_MV,                    resMV);
            resourceSnapshot.SetResource(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, resDiffOut);

            nri::CommandBufferD3D12Desc cmdBufDesc = {};
            cmdBufDesc.d3d12CommandList      = cl;
            cmdBufDesc.d3d12CommandAllocator = nullptr; // optional

            const nrd::Identifier denoiser0 = 0u;
            m_nrdIntegration->DenoiseD3D12(&denoiser0, 1u, cmdBufDesc, resourceSnapshot);

            // Re-bind our descriptor heap (NRD replaces it during Denoise)
            ID3D12DescriptorHeap* ownHeap = m_descHeap.Get();
            cl->SetDescriptorHeaps(1, &ownHeap);
            cl->SetComputeRootSignature(m_restirRootSignature.Get());

            // Transition nrdDiffOut back to SRV for the copy below
            {
                D3D12_RESOURCE_BARRIER b = MakeTransition(m_nrdDiffOut.Get(), kUav, kSrv);
                cl->ResourceBarrier(1, &b);
            }
        }
        else
        {
            // NRD not ready: fall back to copying shadedColor directly
        }

        // =========================================================================
        // A-Trous spatial denoising (3 passes: step=1,2,4) on NRD output
        // Pass 6: nrdDiffOut(SRV) → atrousPingPong(UAV)
        // Pass 7: atrousPingPong(SRV) → nrdDiffOut(UAV)
        // Pass 6: nrdDiffOut(SRV) → atrousPingPong(UAV)  ← final result in atrousPingPong
        // =========================================================================
        if (m_atrousReady && m_restirATrousPso && m_nrdReady)
        {
            // nrdDiffOut is already in SRV state after NRD. Run 5 A-Trous passes (step=1,2,4,8,16)
            // ping-ponging between nrdDiffOut and atrousPingPong (SVGF: Schied et al. 2017).
            // Final result lands in atrousPingPong (5 passes = odd, ping path last).
            {
                for (int ai = 0; ai < 5; ++ai)
                {
                    const bool pingPass = (ai % 2 == 0); // true = Pass6 (nrdDiffOut→pong), false = Pass7 (pong→nrdDiffOut)
                    const D3D12_GPU_VIRTUAL_ADDRESS cbATrous = cbBase + (UINT64)(ai + 1) * 256u;
                    ID3D12Resource* readTex  = pingPass ? m_nrdDiffOut.Get()     : m_atrousPingPong.Get();
                    ID3D12Resource* writeTex = pingPass ? m_atrousPingPong.Get() : m_nrdDiffOut.Get();
                    int passIdx = pingPass ? 6 : 7;

                    D3D12_RESOURCE_BARRIER pre = MakeTransition(writeTex, kSrv, kUav);
                    cl->ResourceBarrier(1, &pre);
                    (void)readTex; // SRV state maintained from previous barrier

                    cl->SetPipelineState(m_restirATrousPso.Get());
                    BindRoots(cbATrous, kNullVA, kNullVA, kNullVA, kNullVA,
                              PassSrvGpu(passIdx), PassUavGpu(passIdx), kNullVA);
                    cl->Dispatch(gx, gy, 1u);

                    D3D12_RESOURCE_BARRIER post = MakeTransition(writeTex, kUav, kSrv);
                    cl->ResourceBarrier(1, &post);
                }
                // After 5 passes: final result is in atrousPingPong (SRV state).
            }
        }

        // =========================================================================
        // Copy NRD denoised result (or shadedColor fallback) → outTexture + prevColor
        // =========================================================================
        {
            // After A-Trous: use atrousPingPong if available, else nrdDiffOut or shadedColor
            ID3D12Resource* srcTex;
            if (m_nrdReady && m_atrousReady)
                srcTex = m_atrousPingPong.Get();
            else if (m_nrdReady)
                srcTex = m_nrdDiffOut.Get();
            else
                srcTex = m_shadedColorTexture.Get();

            D3D12_RESOURCE_BARRIER pre[] = {
                MakeTransition(srcTex,                   kSrv,                               kCopySrc),
                MakeTransition(outTexture.Get(),         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kCopyDst),
                MakeTransition(m_prevColorTexture.Get(), kSrv,                               kCopyDst),
            };
            cl->ResourceBarrier(3, pre);

            cl->CopyResource(outTexture.Get(),         srcTex);
            cl->CopyResource(m_prevColorTexture.Get(), srcTex);

            D3D12_RESOURCE_BARRIER post[] = {
                MakeTransition(srcTex,                   kCopySrc, kSrv),
                MakeTransition(outTexture.Get(),         kCopyDst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                MakeTransition(m_prevColorTexture.Get(), kCopyDst, kSrv),
            };
            cl->ResourceBarrier(3, post);
        }

        // =========================================================================
        // Copy gBuffer → prevGBuffer for next frame reprojection
        // =========================================================================
        {
            D3D12_RESOURCE_BARRIER pre[] = {
                MakeTransition(m_gBufferTexture.Get(),     kSrv, kCopySrc),
                MakeTransition(m_prevGBufferTexture.Get(), kSrv, kCopyDst),
            };
            cl->ResourceBarrier(2, pre);

            cl->CopyResource(m_prevGBufferTexture.Get(), m_gBufferTexture.Get());

            D3D12_RESOURCE_BARRIER post[] = {
                MakeTransition(m_gBufferTexture.Get(),     kCopySrc, kSrv),
                MakeTransition(m_prevGBufferTexture.Get(), kCopyDst, kSrv),
            };
            cl->ResourceBarrier(2, post);
        }

        // =========================================================================
        // Advance per-frame state
        // =========================================================================
        ++m_restirFrameIndex;
        Math::Invert4x4(desc.frameDesc.inverseViewProjection, m_prevVP);
        m_prevReflectionCameraPos[0] = desc.frameDesc.cameraPosition[0];
        m_prevReflectionCameraPos[1] = desc.frameDesc.cameraPosition[1];
        m_prevReflectionCameraPos[2] = desc.frameDesc.cameraPosition[2];

        outStats.usingHardwarePath = false;
        outStats.renderWidth  = desc.width;
        outStats.renderHeight = desc.height;
        return true;
    }

    // =========================================================================
    // RenderShadowReSTIR
    // Dispatches SWRT_Shadow_ReSTIR_CS using the GBuffer populated by the most
    // recent RenderReflectionTextureReSTIR call.
    // outTexture: UAV on entry/exit (R16G16B16A16_FLOAT).
    // =========================================================================

    bool GpuSoftwareRayTracer::RenderShadowReSTIR(const ReflectionTextureDesc& desc,
                                                   IRHIDevice& device,
                                                   CommandList& cmdList,
                                                   Resource& outTexture,
                                                   RayTracingRuntimeStats& outStats)
    {
        (void)outStats;
        if (!m_restirPipelineReady || !outTexture.IsValid()) return false;
        if (!m_gBufferTexture.IsValid()) return false;
        if (desc.width == 0 || desc.height == 0) return false;

        ID3D12Device*              dev = device.GetDevice();
        ID3D12GraphicsCommandList* cl  = cmdList.Get();
        if (!dev || !cl) return false;

        // ---- Common setup ----
        const UINT gx = (desc.width  + 15u) / 16u;
        const UINT gy = (desc.height + 15u) / 16u;

        ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(m_restirRootSignature.Get());

        const D3D12_GPU_DESCRIPTOR_HANDLE heapGpuStart =
            m_descHeap.GetGPUDescriptorHandleForHeapStart();

        D3D12_GPU_DESCRIPTOR_HANDLE bvhGpu = heapGpuStart;  // slots 0-5

        D3D12_GPU_DESCRIPTOR_HANDLE scratchSrvGpu = heapGpuStart;
        scratchSrvGpu.ptr += kScratchSrvBase * m_descIncrementSize;

        D3D12_GPU_DESCRIPTOR_HANDLE scratchUavGpu = heapGpuStart;
        scratchUavGpu.ptr += kScratchUavBase * m_descIncrementSize;

        const D3D12_GPU_VIRTUAL_ADDRESS cbSlot6 =
            m_restirConstantsBuffer.GetGPUVirtualAddress() + 6u * 256u;
        const D3D12_GPU_VIRTUAL_ADDRESS ptLightsVA =
            m_lightDataBuffer.GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS spLightsVA =
            ptLightsVA + kMaxPointLights * sizeof(GpuPointLightRT);

        constexpr auto kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        constexpr auto kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        constexpr D3D12_GPU_VIRTUAL_ADDRESS kNullVA = 0ull;

        // ---- GBuffer: UAV→SRV (was left in SRV state after ReSTIR passes) ----
        // gBufferTexture is already SRV after RenderReflectionTextureReSTIR; just bind it.
        SetScratchTexSRV(dev, m_gBufferTexture, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
        for (UINT slot = 1u; slot < kScratchSrvCount; ++slot)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h = m_descHeap.GetCPUDescriptorHandleForHeapStart();
            h.ptr += (kScratchSrvBase + slot) * m_descIncrementSize;
            CreateNullTex2DSrv(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, h);
        }

        // ---- outTexture: must be in UAV state (caller guarantee) ----
        SetScratchTexUAV(dev, outTexture, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);
        for (UINT slot = 1u; slot < kScratchUavCount; ++slot)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE h = m_descHeap.GetCPUDescriptorHandleForHeapStart();
            h.ptr += (kScratchUavBase + slot) * m_descIncrementSize;
            CreateNullTex2DUav(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, h);
        }

        // ---- Bind and dispatch ----
        cl->SetPipelineState(m_shadowReSTIRPso.Get());
        cl->SetComputeRootConstantBufferView (0, cbSlot6);
        cl->SetComputeRootDescriptorTable    (1, bvhGpu);
        cl->SetComputeRootDescriptorTable    (2, scratchSrvGpu);
        cl->SetComputeRootDescriptorTable    (3, scratchUavGpu);
        cl->SetComputeRootShaderResourceView (4, ptLightsVA);
        cl->SetComputeRootShaderResourceView (5, spLightsVA);
        cl->SetComputeRootShaderResourceView (6, kNullVA);
        cl->SetComputeRootUnorderedAccessView(7, kNullVA);
        cl->SetComputeRootShaderResourceView (8, kNullVA);
        cl->Dispatch(gx, gy, 1u);

        return true;
    }

    // =========================================================================
    // RenderReflectionTexture
    // =========================================================================


    bool GpuSoftwareRayTracer::PrepareReSTIRFrame(const ReflectionTextureDesc& desc,
                                                  IRHIDevice& device,
                                                  CommandList& cmdList,
                                                  Resource& scratchTexture,
                                                  RayTracingRuntimeStats& outStats)
    {
        if (m_swrtMode != SwrtMode::ReSTIR || !m_restirPipelineReady) {
            return false;
        }
        return RenderReflectionTextureReSTIR(desc, device, cmdList, scratchTexture, outStats);
    }

} // namespace SasamiRenderer
