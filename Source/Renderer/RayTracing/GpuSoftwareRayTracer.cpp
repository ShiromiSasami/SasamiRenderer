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

// ---- NRD / NRI integration (included in exactly this one TU) ----
#include "NRI.h"
#include "NRIDeviceCreation.h"
#include "Extensions/NRIWrapperD3D12.h"
#include "Extensions/NRIHelper.h"
#include "NRD.h"
#include "NRDIntegration.h"
#include "NRDIntegration.hpp"

using Microsoft::WRL::ComPtr;

namespace SasamiRenderer
{
    // -------------------------------------------------------------------------
    // Internal helpers (anonymous namespace)
    // -------------------------------------------------------------------------
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

    // =========================================================================
    // GpuSoftwareRayTracer — constructor / destructor
    // =========================================================================

    GpuSoftwareRayTracer::GpuSoftwareRayTracer() = default;
    GpuSoftwareRayTracer::~GpuSoftwareRayTracer()
    {
        if (m_frameConstantsMapped && m_frameConstantsBuffer.IsValid()) {
            m_frameConstantsBuffer.Unmap(0, nullptr);
        }
        m_frameConstantsMapped = nullptr;
        if (m_restirConstantsMapped && m_restirConstantsBuffer.IsValid()) {
            m_restirConstantsBuffer.Unmap(0, nullptr);
        }
        m_restirConstantsMapped = nullptr;
        if (m_lightDataMapped && m_lightDataBuffer.IsValid()) {
            m_lightDataBuffer.Unmap(0, nullptr);
        }
        m_lightDataMapped = nullptr;
        // Destroy NRD integration before GPU resources are freed
        if (m_nrdIntegration) {
            m_nrdIntegration->Destroy();
            m_nrdIntegration.reset();
        }
    }

    // =========================================================================
    // GetBvhGpuAddresses
    // =========================================================================

    GpuSoftwareRayTracer::BvhGpuAddresses GpuSoftwareRayTracer::GetBvhGpuAddresses() const
    {
        BvhGpuAddresses out{};
        if (!m_initialized) return out;
        out.bvhNodes  = m_bvhNodesBuffer.IsValid() ? m_bvhNodesBuffer.GetGPUVirtualAddress() : 0u;
        out.triangles = m_triangleBuffer.IsValid()  ? m_triangleBuffer.GetGPUVirtualAddress()  : 0u;
        out.meshInfo  = m_meshInfoBuffer.IsValid()  ? m_meshInfoBuffer.GetGPUVirtualAddress()  : 0u;
        out.instances = m_instanceBuffer.IsValid()  ? m_instanceBuffer.GetGPUVirtualAddress()  : 0u;
        out.tlasNodes = m_tlasBuffer.IsValid()      ? m_tlasBuffer.GetGPUVirtualAddress()      : 0u;
        out.materials = m_materialBuffer.IsValid()  ? m_materialBuffer.GetGPUVirtualAddress()  : 0u;
        out.valid = (out.bvhNodes != 0);
        return out;
    }

    // =========================================================================
    // Initialize
    // =========================================================================

    bool GpuSoftwareRayTracer::Initialize(IRHIDevice& device)
    {
        if (m_initialized) return true;

        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // ---- Descriptor heap (GPU-visible CBV_SRV_UAV) ----
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kTotalDescriptors;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device.CreateDescriptorHeap(hd, m_descHeap))) return false;
        m_descIncrementSize = device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // Fill all SRV slots with null descriptors initially
        for (UINT i = 0; i < kSrvCount; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_descHeap.GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += i * m_descIncrementSize;
            CreateNullStructuredSrv(dev, cpu);
        }
        for (UINT i = 0; i < kScratchSrvCount; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_descHeap.GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += (kScratchSrvBase + i) * m_descIncrementSize;
            CreateNullTex2DSrv(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, cpu);
        }
        for (UINT i = 0; i < kScratchUavCount; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_descHeap.GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += (kScratchUavBase + i) * m_descIncrementSize;
            CreateNullTex2DUav(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, cpu);
        }
        for (UINT pass = 0; pass < kReSTIRFrameSlots * kReSTIRPassCount; ++pass) {
            const UINT passBase = kReSTIRPassDescBase + pass * kReSTIRPassDescStride;
            for (UINT i = 0; i < kScratchSrvCount; ++i) {
                D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_descHeap.GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += (passBase + i) * m_descIncrementSize;
                CreateNullTex2DSrv(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, cpu);
            }
            for (UINT i = 0; i < kScratchUavCount; ++i) {
                D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_descHeap.GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += (passBase + 4u + i) * m_descIncrementSize;
                CreateNullTex2DUav(dev, DXGI_FORMAT_R16G16B16A16_FLOAT, cpu);
            }
        }

        // ---- Frame constants buffer:
        // Slot 0: shared/legacy shadow or AO
        // Slot 1: legacy reflection
        // Slots 2-5: SWRT directional shadow cascades
        // ----
        constexpr UINT64 kCbufSlotSize = 256u;
        constexpr UINT64 kCbufTotalSize = kCbufSlotSize * (2u + LightSystem::kDirectionalCascadeCount);
        if (!ResourceUploadUtility::CreateUploadBuffer(device, kCbufTotalSize,
                                                        m_frameConstantsBuffer,
                                                        reinterpret_cast<void**>(&m_frameConstantsMapped))) {
            return false;
        }

        // ---- ReSTIR constants buffer: 7 slots of 256 bytes ----
        constexpr UINT64 kRestirCbufTotal = 256u * kReSTIRCbufSlotCount;
        if (!ResourceUploadUtility::CreateUploadBuffer(device, kRestirCbufTotal,
                                                        m_restirConstantsBuffer,
                                                        reinterpret_cast<void**>(&m_restirConstantsMapped))) {
            return false;
        }

        // ---- Light data upload buffer ----
        constexpr UINT64 kLightBufSize =
            kMaxPointLights * sizeof(GpuPointLightRT) +
            kMaxSpotLights  * sizeof(GpuSpotLightRT);
        if (!ResourceUploadUtility::CreateUploadBuffer(device, kLightBufSize,
                                                        m_lightDataBuffer,
                                                        reinterpret_cast<void**>(&m_lightDataMapped))) {
            return false;
        }

        // ---- Compile shaders + create pipelines ----
        if (!CreatePipelines(device)) return false;

        // ---- Try to compile ReSTIR shaders (non-fatal if they fail) ----
        if (!CreateReSTIRPipelines(device)) {
            OutputDebugStringA("GpuSWRT: ReSTIR pipeline creation failed – falling back to legacy reflection.\n");
        }

        m_initialized = true;
        return true;
    }


    // =========================================================================
    // AllocateReSTIRBuffers
    // =========================================================================

    bool GpuSoftwareRayTracer::AllocateReSTIRBuffers(IRHIDevice& device,
                                                       uint32_t w, uint32_t h)
    {
        const UINT64 pixelCount = (UINT64)w * h;
        constexpr UINT64 kReservoirStride = 16u; // sizeof(Reservoir) = uint+float+uint+float

        // Release existing resources
        m_gBufferTexture.Reset();
        m_hitPositionTexture.Reset();
        m_hitMaterialTexture.Reset();
        m_prevGBufferTexture.Reset();
        m_reservoirBuffer[0].Reset(); m_reservoirBuffer[1].Reset(); m_reservoirBuffer[2].Reset();
        m_prevColorTexture.Reset();
        m_shadedColorTexture.Reset();
        m_nrdDiffIn.Reset();
        m_nrdViewZ.Reset();
        m_nrdNormalRoughness.Reset();
        m_nrdMotionVec.Reset();
        m_nrdDiffOut.Reset();
        m_atrousPingPong.Reset();
        m_nrdReady  = false;
        m_atrousReady = false;

        constexpr auto kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_gBufferTexture))        return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_hitPositionTexture))    return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_hitMaterialTexture))    return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_prevGBufferTexture))    return false;
        if (!CreateUavBuffer(device, pixelCount * kReservoirStride, kSrv, m_reservoirBuffer[0]))               return false;
        if (!CreateUavBuffer(device, pixelCount * kReservoirStride, kSrv, m_reservoirBuffer[1]))               return false;
        if (!CreateUavBuffer(device, pixelCount * kReservoirStride, kSrv, m_reservoirBuffer[2]))               return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_prevColorTexture))       return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_shadedColorTexture))     return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_atrousPingPong))         return false;
        m_atrousReady = true;

        if (!AllocateNrdBuffers(device, w, h)) return false;

        m_restirWidth   = w;
        m_restirHeight  = h;
        m_restirFrameIndex = 0;
        memset(m_prevVP, 0, sizeof(m_prevVP));
        m_prevVP[0] = m_prevVP[5] = m_prevVP[10] = m_prevVP[15] = 1.0f;
        memset(m_prevReflectionCameraPos, 0, sizeof(m_prevReflectionCameraPos));

        return true;
    }

    // =========================================================================
    // AllocateNrdBuffers
    // =========================================================================

    bool GpuSoftwareRayTracer::AllocateNrdBuffers(IRHIDevice& device, uint32_t w, uint32_t h)
    {
        constexpr auto kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_nrdDiffIn))          return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16_FLOAT,          kSrv, m_nrdViewZ))            return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM,     kSrv, m_nrdNormalRoughness))  return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16_FLOAT,       kSrv, m_nrdMotionVec))        return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_nrdDiffOut))          return false;

        return true;
    }

    // =========================================================================
    // InitializeNrdIntegration
    // =========================================================================

    bool GpuSoftwareRayTracer::InitializeNrdIntegration(IRHIDevice& device, uint32_t w, uint32_t h)
    {
        ID3D12Device* d3d12Device = device.GetDevice();
        if (!d3d12Device) return false;

        nrd::DenoiserDesc denoiserDescs[1] = {};
        denoiserDescs[0].identifier = 0u;
        denoiserDescs[0].denoiser   = nrd::Denoiser::RELAX_DIFFUSE;

        nrd::InstanceCreationDesc instanceDesc = {};
        instanceDesc.denoisers    = denoiserDescs;
        instanceDesc.denoisersNum = 1;

        nrd::IntegrationCreationDesc integrationDesc = {};
        strncpy_s(integrationDesc.name, "RELAX_D", sizeof(integrationDesc.name) - 1);
        integrationDesc.resourceWidth       = static_cast<uint16_t>(w);
        integrationDesc.resourceHeight      = static_cast<uint16_t>(h);
        integrationDesc.queuedFrameNum      = 3;
        integrationDesc.autoWaitForIdle     = true;
        integrationDesc.residencyPriority   = 0.0f;

        nri::DeviceCreationD3D12Desc deviceD3D12Desc = {};
        deviceD3D12Desc.d3d12Device            = d3d12Device;
        deviceD3D12Desc.queueFamilies          = nullptr;
        deviceD3D12Desc.queueFamilyNum         = 0;
        deviceD3D12Desc.enableNRIValidation    = false;
        deviceD3D12Desc.disableD3D12EnhancedBarriers = true;
        deviceD3D12Desc.disableNVAPIInitialization   = true;

        m_nrdIntegration = std::make_unique<nrd::Integration>();
        nrd::Result result = m_nrdIntegration->RecreateD3D12(integrationDesc, instanceDesc, deviceD3D12Desc);
        if (result != nrd::Result::SUCCESS) {
            OutputDebugStringA("GpuSWRT: NRD RecreateD3D12 failed\n");
            m_nrdIntegration.reset();
            return false;
        }

        m_nrdReady = true;
        return true;
    }

    // =========================================================================
    // FillReSTIRConstants
    // =========================================================================

    void GpuSoftwareRayTracer::FillReSTIRConstants(const ReflectionTextureDesc& desc,
                                                    float stepWidth,
                                                    ReSTIRFrameConstants& out) const
    {
        memset(&out, 0, sizeof(out));
        memcpy(out.invVP,     desc.frameDesc.inverseViewProjection, 64);
        memcpy(out.prevVP, m_prevVP,                                 64);
        out.cameraPos[0]          = desc.frameDesc.cameraPosition[0];
        out.cameraPos[1]          = desc.frameDesc.cameraPosition[1];
        out.cameraPos[2]          = desc.frameDesc.cameraPosition[2];
        out.tMin                  = 0.001f;
        out.renderWidth           = desc.width;
        out.renderHeight          = desc.height;
        out.frameIndex            = m_restirFrameIndex;
        out.reservoirWidth        = desc.width;
        // When the camera moved since the last frame, discard temporal history entirely
        // (alpha = 1.0 = full current-frame weight) to prevent ghosting / lag.
        // When the camera is stationary, accumulate normally at 0.1 (10% current frame).
        out.temporalAlpha         = desc.cameraChanged ? 1.0f : 0.1f;
        out.phiColor              = 4.0f;
        out.phiNormal             = 128.0f;
        out.phiDepth              = 1.0f;
        out.stepWidth             = stepWidth;
        out.maxSurfaceRoughness   = desc.maxSurfaceRoughness;
        out.maxPrimaryHitDistance = desc.maxReflectionTraceDistance;
        out.minReflectionEnergy   = desc.minReflectionEnergy;
        out.shadowBias            = 0.002f;
        out.ambientColor[0] = out.ambientColor[1] = out.ambientColor[2] = 0.1f;
        out.ambientIntensity = 1.0f;
        out.cbPad0 = desc.gbufferWidth ? desc.gbufferWidth : desc.width;
        out.cbPad1 = desc.gbufferHeight ? desc.gbufferHeight : desc.height;

        const auto& dirLight = desc.frameDesc.directionalLight;
        {
            float fwd[3]{};
            Math::DirectionFromYawPitch(dirLight.yaw, dirLight.pitch, fwd);
            out.dirLightDir[0] = -fwd[0];
            out.dirLightDir[1] = -fwd[1];
            out.dirLightDir[2] = -fwd[2];
        }
        out.dirLightIntensity = dirLight.intensity;
        out.dirLightColor[0]  = dirLight.color[0];
        out.dirLightColor[1]  = dirLight.color[1];
        out.dirLightColor[2]  = dirLight.color[2];

        uint32_t nPt = 0, nSp = 0;
        if (desc.frameDesc.pointLights)
            nPt = std::min((uint32_t)desc.frameDesc.pointLights->size(), kMaxPointLights);
        if (desc.frameDesc.spotLights)
            nSp = std::min((uint32_t)desc.frameDesc.spotLights->size(), kMaxSpotLights);
        out.pointLightCount = nPt;
        out.spotLightCount  = nSp;
    }

    // =========================================================================
    // SetScratchTexSRV / SetScratchTexUAV
    // =========================================================================

    void GpuSoftwareRayTracer::SetScratchTexSRV(ID3D12Device* dev, Resource& tex,
                                                  DXGI_FORMAT fmt, UINT slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_descHeap.GetCPUDescriptorHandleForHeapStart();
        h.ptr += (kScratchSrvBase + slot) * m_descIncrementSize;
        CreateTex2DSrv(dev, tex.Get(), fmt, h);
    }

    void GpuSoftwareRayTracer::SetScratchTexUAV(ID3D12Device* dev, Resource& tex,
                                                  DXGI_FORMAT fmt, UINT slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_descHeap.GetCPUDescriptorHandleForHeapStart();
        h.ptr += (kScratchUavBase + slot) * m_descIncrementSize;
        CreateTex2DUav(dev, tex.Get(), fmt, h);
    }

    // =========================================================================
    // UpdateScene
    // =========================================================================

    void GpuSoftwareRayTracer::UpdateScene(const RayTracingScene& scene,
                                           IRHIDevice& device,
                                           CommandList& /*unusedCmdList*/)
    {
        if (!m_initialized) return;

        m_scene = scene;

        const bool geoDirty  = (scene.geometryVersion != m_bvhGeometryVersion);
        const bool matDirty  = (scene.materialVersion != m_bvhMaterialVersion);
        const bool instDirty = (scene.instanceVersion  != m_bvhInstanceVersion);
        if (!geoDirty && !matDirty && !instDirty) return;

        if (geoDirty || instDirty) {
            RebuildAccelerationStructures();
        }
        UploadBvhBuffers(device);
    }


    // =========================================================================
    // UploadBvhBuffers — flatten CPU BVH into GPU buffers
    // =========================================================================

    bool GpuSoftwareRayTracer::UploadBvhBuffers(IRHIDevice& device)
    {
        ID3D12Device* dev = device.GetDevice();
        if (!dev || !m_descHeap.Get()) return false;

        // ---- Flatten BLAS nodes ----
        std::vector<BvhNode> allNodes;
        std::vector<GpuTriangle> allTris;
        std::vector<GpuMeshInfo> meshInfos;
        for (const auto& ma : m_meshAccelerations) {
            GpuMeshInfo mi{ ma.nodeOffset, ma.triOffset, 0, 0 };
            meshInfos.push_back(mi);
            // Interior node child indices from BuildMeshBvh are mesh-local (0-based within ma.nodes).
            // Offset them to absolute global indices so the GPU traversal can index g_bvhNodes directly.
            for (BvhNode node : ma.nodes) {
                if (!node.IsLeaf()) {
                    node.leftChild    += static_cast<int32_t>(ma.nodeOffset);
                    node.rightOrCount += static_cast<int32_t>(ma.nodeOffset);
                }
                allNodes.push_back(node);
            }
            allTris.insert(allTris.end(), ma.triangles.begin(), ma.triangles.end());
        }

        // ---- Instance buffer ----
        std::vector<GpuInstanceInfo> instBuf;
        instBuf.reserve(m_topLevelInstanceOrder.size());
        for (uint32_t ordIdx=0; ordIdx<(uint32_t)m_topLevelInstanceOrder.size(); ++ordIdx) {
            const uint32_t instIdx = m_topLevelInstanceOrder[ordIdx];
            const RayTracingInstance& ri = m_scene.instances[instIdx];
            GpuInstanceInfo gi{};
            gi.meshIndex     = ri.meshIndex;
            gi.materialIndex = ri.materialIndex;
            memcpy(gi.world,    ri.model,        64);
            memcpy(gi.invWorld, ri.inverseModel,  64);
            memcpy(gi.worldBoundsMin, ri.worldBoundsMin, 12);
            memcpy(gi.worldBoundsMax, ri.worldBoundsMax, 12);
            instBuf.push_back(gi);
        }

        // ---- Material buffer ----
        std::vector<GpuMaterial> matBuf;
        matBuf.reserve(m_scene.materials.size());
        for (const auto& mat : m_scene.materials) {
            GpuMaterial gm{};
            gm.baseColor[0] = mat.material.baseColor[0];
            gm.baseColor[1] = mat.material.baseColor[1];
            gm.baseColor[2] = mat.material.baseColor[2];
            gm.baseColor[3] = mat.material.baseColor[3];
            gm.roughness = mat.material.roughness;
            gm.metallic  = mat.material.metallic;
            gm.transmission = mat.material.transmission;
            gm.ior = mat.material.ior;
            gm.specularColor[0] = mat.material.specularColor[0];
            gm.specularColor[1] = mat.material.specularColor[1];
            gm.specularColor[2] = mat.material.specularColor[2];
            gm.workflow = static_cast<float>(static_cast<uint32_t>(mat.material.workflow));
            gm.emissive[0] = mat.material.emissive[0];
            gm.emissive[1] = mat.material.emissive[1];
            gm.emissive[2] = mat.material.emissive[2];
            gm.occlusionStrength = mat.material.occlusionStrength;
            matBuf.push_back(gm);
        }

        // ---- Fallback: ensure at least one entry for empty scenes ----
        if (allNodes.empty())   allNodes.push_back({});
        if (allTris.empty())    allTris.push_back({});
        if (meshInfos.empty())  meshInfos.push_back({});
        if (instBuf.empty())    instBuf.push_back({});
        if (matBuf.empty())     matBuf.push_back({});
        if (m_topLevelNodes.empty()) m_topLevelNodes.push_back({});

        // ---- Upload to GPU (stalls GPU once if dirty) ----
        if (!CreateAndUploadBuffer(device, allNodes.data(),   allNodes.size()*sizeof(BvhNode),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m_bvhNodesBuffer)) return false;
        if (!CreateAndUploadBuffer(device, allTris.data(),    allTris.size()*sizeof(GpuTriangle),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m_triangleBuffer)) return false;
        if (!CreateAndUploadBuffer(device, meshInfos.data(),  meshInfos.size()*sizeof(GpuMeshInfo),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m_meshInfoBuffer)) return false;
        if (!CreateAndUploadBuffer(device, instBuf.data(),    instBuf.size()*sizeof(GpuInstanceInfo),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m_instanceBuffer)) return false;
        if (!CreateAndUploadBuffer(device, m_topLevelNodes.data(), m_topLevelNodes.size()*sizeof(TlasNode),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m_tlasBuffer)) return false;
        if (!CreateAndUploadBuffer(device, matBuf.data(),     matBuf.size()*sizeof(GpuMaterial),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m_materialBuffer)) return false;

        // ---- Create SRVs in descriptor heap ----
        auto cpuBase = m_descHeap.GetCPUDescriptorHandleForHeapStart();
        auto srv = [&](UINT slot) -> D3D12_CPU_DESCRIPTOR_HANDLE {
            return { cpuBase.ptr + slot * m_descIncrementSize };
        };

        CreateStructuredSrv(dev, m_bvhNodesBuffer,  (UINT)allNodes.size(),  sizeof(BvhNode),       srv(0));
        CreateStructuredSrv(dev, m_triangleBuffer,  (UINT)allTris.size(),   sizeof(GpuTriangle),   srv(1));
        CreateStructuredSrv(dev, m_meshInfoBuffer,  (UINT)meshInfos.size(), sizeof(GpuMeshInfo),   srv(2));
        CreateStructuredSrv(dev, m_instanceBuffer,  (UINT)instBuf.size(),   sizeof(GpuInstanceInfo), srv(3));
        CreateStructuredSrv(dev, m_tlasBuffer,      (UINT)m_topLevelNodes.size(), sizeof(TlasNode), srv(4));
        CreateStructuredSrv(dev, m_materialBuffer,  (UINT)matBuf.size(),    sizeof(GpuMaterial),   srv(5));

        m_uploadedGeometryVersion = m_scene.geometryVersion;
        m_uploadedMaterialVersion = m_scene.materialVersion;
        m_uploadedInstanceVersion = m_scene.instanceVersion;
        m_bvhMaterialVersion      = m_scene.materialVersion;
        return true;
    }

    // =========================================================================
    // UpdateTextureUav — write UAV descriptor for output texture
    // =========================================================================

    void GpuSoftwareRayTracer::UpdateTextureUav(ID3D12Device* dev, Resource& texture,
                                                 DXGI_FORMAT format, UINT slotIndex, UINT arraySize)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dest = m_descHeap.GetCPUDescriptorHandleForHeapStart();
        dest.ptr += slotIndex * m_descIncrementSize;

        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format        = format;
        if (arraySize > 1u)
        {
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray.ArraySize = arraySize;
            d.Texture2DArray.FirstArraySlice = 0u;
        }
        else
        {
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        }
        dev->CreateUnorderedAccessView(texture.Get(), nullptr, &d, dest);
    }

    // =========================================================================
    // FillShadowConstants / FillReflectionConstants
    // =========================================================================


} // namespace SasamiRenderer
