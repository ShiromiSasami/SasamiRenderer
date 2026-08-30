#define NOMINMAX
#include "Renderer/GI/IrradianceProbeGrid.h"

#include "Foundation/Math/MathUtil.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "Renderer/Resources/ShaderCompilationService.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

#include <d3dcompiler.h>
#include <wrl.h>

#include "d3dx12.h"

using Microsoft::WRL::ComPtr;

namespace SasamiRenderer
{
    namespace
    {
        bool CompileCS(ID3D12Device* dev, const wchar_t* relPath, const char* entry, ComPtr<ID3DBlob>& outBlob,
                       std::string& outError)
        {
            const std::wstring csTarget = ShaderCompilationService::ResolveEffectiveComputeShaderTarget(dev, "6_6");
            const std::string profileNarrow(csTarget.begin(), csTarget.end());

            std::vector<uint8_t> bytecode;
            if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc(
                    "GI", relPath, entry, profileNarrow.c_str(), bytecode)) {
                outError = "ShaderCompilationService: failed to compile shader";
                return false;
            }

            // Copy to ID3DBlob for compatibility with PSO creation
            ID3DBlob* blob = nullptr;
            if (FAILED(D3DCreateBlob(bytecode.size(), &blob)) || !blob) {
                return false;
            }
            memcpy(blob->GetBufferPointer(), bytecode.data(), bytecode.size());
            outBlob.Attach(blob);
            return true;
        }

        // GPU mirror of GpuPointLightRT / GpuSpotLightRT (RayTracing/SWRT/SWRT_LightTypes.hlsli).
        // Redeclared here rather than reusing GpuSoftwareRayTracer's private nested types so
        // this GI-only compute pass carries no dependency on unrelated SWRT reflection/ReSTIR code.
        struct GIPointLightGpu
        {
            float pos[3];
            float range;
            float colorIntensity[3];
            float pad;
        };
        static_assert(sizeof(GIPointLightGpu) == 32u);

        struct GISpotLightGpu
        {
            float pos[3];
            float range;
            float dir[3];
            float cosInner;
            float colorIntensity[3];
            float cosOuter;
        };
        static_assert(sizeof(GISpotLightGpu) == 48u);

    } // anonymous namespace

    // =========================================================================
    // Constructor / Destructor
    // =========================================================================

    IrradianceProbeGrid::IrradianceProbeGrid() = default;

    IrradianceProbeGrid::~IrradianceProbeGrid()
    {
        if (m_cbMapped) {
            if (Resource* cb = GetConstantBufferResource()) {
                cb->Unmap(0, nullptr);
            }
        }
        m_cbMapped = nullptr;
        if (m_lightDataMapped) {
            m_lightDataBuffer.Unmap(0, nullptr);
        }
        m_lightDataMapped = nullptr;
        if (m_device && m_cbBufferHandle.IsValid()) {
            m_device->DestroyRhiResource(m_cbBufferHandle);
        }
        if (m_device && m_probeBufferHandle.IsValid()) {
            m_device->DestroyRhiResource(m_probeBufferHandle);
        }
        m_cbBufferHandle = {};
        m_probeBufferHandle = {};
        m_cbBufferCompat = nullptr;
        m_probeBufferCompat = nullptr;
    }

    // =========================================================================
    // SetGridCount
    // =========================================================================

    void IrradianceProbeGrid::SetGridCount(uint32_t cx, uint32_t cy, uint32_t cz)
    {
        m_countX = std::max(1u, cx);
        m_countY = std::max(1u, cy);
        m_countZ = std::max(1u, cz);
    }

    // =========================================================================
    // FitToSceneBounds
    // Auto-sizes the grid to cover the given world AABB with a margin.
    // =========================================================================

    void IrradianceProbeGrid::FitToSceneBounds(float bMinX, float bMinY, float bMinZ,
                                                float bMaxX, float bMaxY, float bMaxZ,
                                                float margin)
    {
        const float extX = (bMaxX - bMinX) + margin * 2.0f;
        const float extY = (bMaxY - bMinY) + margin * 2.0f;
        const float extZ = (bMaxZ - bMinZ) + margin * 2.0f;

        m_originX = bMinX - margin;
        m_originY = bMinY - margin;
        m_originZ = bMinZ - margin;

        // Compute probe counts to cover the extent with current spacing
        const float sx = std::max(m_spacingX, 0.1f);
        const float sy = std::max(m_spacingY, 0.1f);
        const float sz = std::max(m_spacingZ, 0.1f);
        m_countX = std::max(2u, static_cast<uint32_t>(std::ceil(extX / sx)) + 1u);
        m_countY = std::max(2u, static_cast<uint32_t>(std::ceil(extY / sy)) + 1u);
        m_countZ = std::max(2u, static_cast<uint32_t>(std::ceil(extZ / sz)) + 1u);
    }

    // =========================================================================
    // FitToSceneBoundsWithBudget
    // Auto-sizes the grid to cover the given world AABB while keeping the total
    // probe count within probeBudget, by coarsening the spacing beforehand.
    // =========================================================================

    void IrradianceProbeGrid::FitToSceneBoundsWithBudget(float bMinX, float bMinY, float bMinZ,
                                                          float bMaxX, float bMaxY, float bMaxZ,
                                                          float margin,
                                                          std::uint32_t probeBudget)
    {
        if (probeBudget == 0u) {
            FitToSceneBounds(bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ, margin);
            return;
        }

        const float extX = std::max(0.0f, (bMaxX - bMinX) + margin * 2.0f);
        const float extY = std::max(0.0f, (bMaxY - bMinY) + margin * 2.0f);
        const float extZ = std::max(0.0f, (bMaxZ - bMinZ) + margin * 2.0f);
        const double volume = static_cast<double>(extX) * static_cast<double>(extY) * static_cast<double>(extZ);

        if (volume <= 0.0 || extX == 0.0f || extY == 0.0f || extZ == 0.0f) {
            FitToSceneBounds(bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ, margin);
            return;
        }

        // Target spacing that would yield ~probeBudget probes over this volume.
        // Never go finer than the current spacing -- this function only coarsens.
        const double target = std::cbrt(volume / static_cast<double>(probeBudget));
        const float currentSpacing = std::max(m_spacingX, std::max(m_spacingY, m_spacingZ));
        float spacing = std::max(currentSpacing, static_cast<float>(target));
        m_spacingX = m_spacingY = m_spacingZ = spacing;

        FitToSceneBounds(bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ, margin);

        // Rounding (ceil + 1 per axis) can still push the count past the budget;
        // coarsen the spacing further and retry until it fits (or we give up).
        constexpr int kMaxIterations = 16;
        for (int iter = 0; iter < kMaxIterations; ++iter) {
            const std::uint64_t totalProbes = static_cast<std::uint64_t>(m_countX) *
                                              static_cast<std::uint64_t>(m_countY) *
                                              static_cast<std::uint64_t>(m_countZ);
            if (totalProbes <= static_cast<std::uint64_t>(probeBudget)) {
                break;
            }
            spacing *= 1.15f;
            m_spacingX = m_spacingY = m_spacingZ = spacing;
            FitToSceneBounds(bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ, margin);
        }
    }

    // =========================================================================
    // Initialize
    // =========================================================================

    bool IrradianceProbeGrid::Initialize(IRHIDevice& device)
    {
        if (m_initialized) return true;
        m_device = &device;

        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // ---- Persistently-mapped upload CB ----
        // Slot 0 (256 bytes): GIProbeGridCBData   → bound to b2 in PBR_PS
        // Slot 1 (256 bytes): GIUpdateCBData       → bound to b0 in GI_ProbeUpdate_CS
        constexpr UINT64 kCbSlotSize = 256u;
        constexpr UINT64 kCbTotalSize = kCbSlotSize * 2u;
        if (device.GetCapabilities().supportsRhiResourceCreation) {
            RhiBufferDesc cbDesc{};
            cbDesc.sizeInBytes = kCbTotalSize;
            cbDesc.strideInBytes = 0u;
            cbDesc.usage = RhiBufferUsageFlags::Constant;
            cbDesc.memoryUsage = RhiMemoryUsage::CpuToGpu;
            cbDesc.initialState = RhiResourceState::Common;
            m_cbBufferHandle = device.CreateRhiBuffer(cbDesc);
            m_cbBufferCompat = device.GetD3D12CompatibilityResource(m_cbBufferHandle);
            if (m_cbBufferCompat) {
                const D3D12_RANGE emptyRange{ 0, 0 };
                if (FAILED(m_cbBufferCompat->Map(0, &emptyRange, reinterpret_cast<void**>(&m_cbMapped)))) {
                    m_cbMapped = nullptr;
                    device.DestroyRhiResource(m_cbBufferHandle);
                    m_cbBufferHandle = {};
                    m_cbBufferCompat = nullptr;
                    return false;
                }
            }
        }
        if (!m_cbMapped) {
            if (!ResourceUploadUtility::CreateUploadBuffer(device, kCbTotalSize,
                                                           m_cbBuffer,
                                                           reinterpret_cast<void**>(&m_cbMapped))) {
                return false;
            }
            m_cbBufferHandle = {};
            m_cbBufferCompat = nullptr;
        }
        // Zero-initialise (giEnabled = 0.0f → GI disabled until first update writes it)
        memset(m_cbMapped, 0, kCbTotalSize);

        // ---- Punctual light data (persistently-mapped upload buffer, root SRVs t6/t7) ----
        // Bound directly by GPU VA (no descriptor view), same style as the BVH buffers.
        {
            const UINT64 lightBufSize =
                static_cast<UINT64>(kMaxPointLights) * sizeof(GIPointLightGpu) +
                static_cast<UINT64>(kMaxSpotLights)  * sizeof(GISpotLightGpu);
            if (!ResourceUploadUtility::CreateUploadBuffer(device, lightBufSize,
                                                           m_lightDataBuffer,
                                                           reinterpret_cast<void**>(&m_lightDataMapped))) {
                return false;
            }
            memset(m_lightDataMapped, 0, lightBufSize);
        }

        // ---- Descriptor heap: [0]=probe SRV (t10), [1]=probe UAV (u0) ----
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2u;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;  // CPU-only for copying
        if (FAILED(device.CreateDescriptorHeap(hd, m_descHeap))) return false;
        m_descStride = device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // ---- Allocate probe buffer (and create null SRV/UAV for now) ----
        if (!AllocateProbeBuffer(device)) return false;

        // ---- Compile shader + create pipeline ----
        if (!CreatePipeline(device)) {
            OutputDebugStringA("IrradianceProbeGrid: shader compilation failed — GI will be disabled.\n");
            // Non-fatal: GI disabled but bindings are still valid
        }

        // Write initial grid parameters to the GPU CB so that debug visualization
        // works even before UpdateProbes() is ever called (e.g. in Raster mode).
        FlushGridCB();

        m_initialized = true;
        return true;
    }

    // =========================================================================
    // AllocateProbeBuffer
    // Allocates (or reallocates) the probe StructuredBuffer and creates SRV/UAV.
    // =========================================================================

    bool IrradianceProbeGrid::AllocateProbeBuffer(IRHIDevice& device, const void* initialData)
    {
        const uint32_t totalProbes = m_countX * m_countY * m_countZ;
        if (!initialData && totalProbes == m_probeBufferCapacity) {
            Resource* currentProbeBuffer = GetProbeBufferResource();
            if (currentProbeBuffer && currentProbeBuffer->IsValid()) {
                return true;
            }
        }

        // A buffer created without initial data starts zeroed, so whatever bake progress
        // was recorded for the previous allocation no longer describes its contents.
        // Clearing the progress here keeps IsBaked() honest and makes the next pass write
        // at full weight instead of blending fresh radiance into zeros.
        if (!initialData) {
            ResetBakeState();
            m_everBaked = false;
        }

        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // float4[9] per probe
        const UINT64 bufSize = static_cast<UINT64>(totalProbes) * 9u * sizeof(float) * 4u;

        m_probeBuffer.Reset();
        if (m_probeBufferHandle.IsValid()) {
            device.DestroyRhiResource(m_probeBufferHandle);
        }
        m_probeBufferCompat = nullptr;
        m_probeBufferHandle = {};
        if (device.GetCapabilities().supportsRhiResourceCreation) {
            RhiBufferDesc rhiDesc{};
            rhiDesc.sizeInBytes = bufSize;
            rhiDesc.strideInBytes = sizeof(float) * 4u;
            rhiDesc.usage = RhiBufferUsageFlags::Structured |
                            RhiBufferUsageFlags::ShaderResource |
                            RhiBufferUsageFlags::UnorderedAccess |
                            RhiBufferUsageFlags::CopySource |
                            RhiBufferUsageFlags::CopyDest;
            rhiDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
            rhiDesc.initialState = initialData ? RhiResourceState::CopyDest : RhiResourceState::ShaderResource;
            m_probeBufferHandle = device.CreateRhiBuffer(rhiDesc, initialData);
            m_probeBufferCompat = device.GetD3D12CompatibilityResource(m_probeBufferHandle);
        }

        if (initialData && !m_probeBufferCompat) {
            if (m_probeBufferHandle.IsValid()) {
                device.DestroyRhiResource(m_probeBufferHandle);
                m_probeBufferHandle = {};
            }
            return false;
        }

        if (!m_probeBufferCompat) {
            if (m_probeBufferHandle.IsValid()) {
                device.DestroyRhiResource(m_probeBufferHandle);
                m_probeBufferHandle = {};
            }
            // Allocate default-heap buffer with UAV flag; initial state = PIXEL_SHADER_RESOURCE
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width     = bufSize;
            desc.Height    = desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                       nullptr, m_probeBuffer))) {
                return false;
            }
        }

        m_probeBufferCapacity = totalProbes;
        Resource* probeBuffer = GetProbeBufferResource();
        if (!probeBuffer || !probeBuffer->IsValid()) {
            return false;
        }

        // ---- SRV (slot 0) ----
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = m_descHeap.GetCPUDescriptorHandleForHeapStart();
        if (m_probeBufferHandle.IsValid()) {
            RhiBufferViewDesc viewDesc{};
            viewDesc.type = RhiBufferViewType::Structured;
            viewDesc.sizeInBytes = bufSize;
            viewDesc.strideInBytes = sizeof(float) * 4u;
            if (!device.CreateRhiBufferShaderResourceView(
                    m_probeBufferHandle,
                    viewDesc,
                    RhiCpuDescriptorHandle{ srvCpu.ptr })) {
                device.DestroyRhiResource(m_probeBufferHandle);
                m_probeBufferHandle = {};
                m_probeBufferCompat = nullptr;
                return false;
            }
        } else {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format                  = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement     = 0;
            srvDesc.Buffer.NumElements      = totalProbes * 9u;
            srvDesc.Buffer.StructureByteStride = sizeof(float) * 4u;
            dev->CreateShaderResourceView(probeBuffer->Get(), &srvDesc, srvCpu);
        }

        // ---- UAV (slot 1) ----
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = m_descHeap.GetCPUDescriptorHandleForHeapStart();
        uavCpu.ptr += m_descStride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format              = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements  = totalProbes * 9u;
        uavDesc.Buffer.StructureByteStride = sizeof(float) * 4u;
        dev->CreateUnorderedAccessView(probeBuffer->Get(), nullptr, &uavDesc, uavCpu);

        // ---- GPU-visible SRV/UAV handles for root descriptors ----
        // Note: we use root SRV/UAV (inline) so no GPU-visible descriptor heap needed.
        // m_probeSrv and m_probeUav are not used as descriptor table handles;
        // instead callers use GetProbeDataGpuVA() directly.
        m_probeSrv = {};  // not used for root SRV
        m_probeUav = {};  // not used for root UAV

        // Refresh the GPU CB so any caller of ReallocAndClearProbeBuffer() (e.g. FitProbeGridToScene)
        // immediately sees the updated counts/origin in the debug visualization.
        FlushGridCB();

        return true;
    }

    // =========================================================================
    // GetProbeGridCbGpuAddress
    // Returns GPU VA for GIProbeGridCB (slot 0 of m_cbBuffer, 256-byte aligned).
    // =========================================================================

    D3D12_GPU_VIRTUAL_ADDRESS IrradianceProbeGrid::GetProbeGridCbGpuAddress() const
    {
        Resource* cb = GetConstantBufferResource();
        return (cb && cb->IsValid()) ? cb->GetGPUVirtualAddress() : 0u;
    }

    // =========================================================================
    // GetProbeDataGpuVA
    // =========================================================================

    D3D12_GPU_VIRTUAL_ADDRESS IrradianceProbeGrid::GetProbeDataGpuVA() const
    {
        Resource* probeBuffer = GetProbeBufferResource();
        return (probeBuffer && probeBuffer->IsValid()) ? probeBuffer->GetGPUVirtualAddress() : 0u;
    }

    Resource* IrradianceProbeGrid::GetProbeBufferResource() const
    {
        return m_probeBufferCompat ? m_probeBufferCompat : const_cast<Resource*>(&m_probeBuffer);
    }

    Resource* IrradianceProbeGrid::GetConstantBufferResource() const
    {
        return m_cbBufferCompat ? m_cbBufferCompat : const_cast<Resource*>(&m_cbBuffer);
    }

    // =========================================================================
    // CreatePipeline
    // =========================================================================

    bool IrradianceProbeGrid::CreatePipeline(IRHIDevice& device)
    {
        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // Root signature:
        //  [0] Root CBV  (b0): GIUpdateCB
        //  [1] Root SRV  (t0): g_bvhNodes
        //  [2] Root SRV  (t1): g_triangles
        //  [3] Root SRV  (t2): g_meshInfos
        //  [4] Root SRV  (t3): g_instances
        //  [5] Root SRV  (t4): g_tlasNodes
        //  [6] Root SRV  (t5): g_materials
        //  [7] Root SRV  (t6): g_pointLights
        //  [8] Root SRV  (t7): g_spotLights
        //  [9] Root UAV  (u0): g_probeSHOutput

        D3D12_ROOT_PARAMETER params[10]{};

        // [0] CBV b0
        params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace  = 0;
        params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        // [1-6] SRV t0-t5
        for (UINT i = 0; i < 6u; ++i) {
            params[1 + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
            params[1 + i].Descriptor.ShaderRegister = i;
            params[1 + i].Descriptor.RegisterSpace  = 0;
            params[1 + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        }

        // [7-8] SRV t6-t7 (punctual lights)
        for (UINT i = 0; i < 2u; ++i) {
            params[7 + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
            params[7 + i].Descriptor.ShaderRegister = 6u + i;
            params[7 + i].Descriptor.RegisterSpace  = 0;
            params[7 + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        }

        // [9] UAV u0
        params[9].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[9].Descriptor.ShaderRegister = 0;
        params[9].Descriptor.RegisterSpace  = 0;
        params[9].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 10;
        rsDesc.pParameters   = params;
        rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> rsBlob, rsError;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                               rsBlob.ReleaseAndGetAddressOf(),
                                               rsError.ReleaseAndGetAddressOf()))) {
            if (rsError) OutputDebugStringA((char*)rsError->GetBufferPointer());
            m_lastPipelineError = "RootSignature: serialize failed";
            if (rsError && rsError->GetBufferSize() > 0)
                m_lastPipelineError += " - " + std::string((char*)rsError->GetBufferPointer());
            return false;
        }
        if (FAILED(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                            IID_PPV_ARGS(m_rootSig.ReleaseAndGetAddressOf())))) {
            m_lastPipelineError = "RootSignature: CreateRootSignature failed";
            return false;
        }

        // Compile and create PSO
        ComPtr<ID3DBlob> cs;
        std::string compileError;
        if (!CompileCS(dev, L"RayTracing/GI/GI_ProbeUpdate_CS.hlsl", "CS_ProbeUpdate", cs, compileError)) {
            m_lastPipelineError = "ShaderCompile: " + compileError;
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.CS             = { cs->GetBufferPointer(), cs->GetBufferSize() };
        if (FAILED(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(m_pso.ReleaseAndGetAddressOf())))) {
            m_lastPipelineError = "PSO: CreateComputePipelineState failed";
            return false;
        }

        return true;
    }

    // =========================================================================
    // FlushGridCB
    // Writes current grid parameters into the persistently-mapped GPU CB slot 0.
    // Called from Initialize() and AllocateProbeBuffer() so the debug visualization
    // always has up-to-date values even when UpdateProbes() is never called (Raster mode).
    // =========================================================================

    void IrradianceProbeGrid::FlushGridCB() const
    {
        if (!m_cbMapped) return;
        GIProbeGridCBData gridCB{};
        FillProbeGridCB(gridCB);
        memcpy(m_cbMapped, &gridCB, sizeof(gridCB));
    }

    // =========================================================================
    // FillProbeGridCB
    // =========================================================================

    void IrradianceProbeGrid::FillProbeGridCB(GIProbeGridCBData& out) const
    {
        out.probeOrigin[0] = m_originX;
        out.probeOrigin[1] = m_originY;
        out.probeOrigin[2] = m_originZ;
        out.giIntensity    = m_giIntensity;
        out.probeSpacing[0] = m_spacingX;
        out.probeSpacing[1] = m_spacingY;
        out.probeSpacing[2] = m_spacingZ;
        out.giEnabled      = (m_enabled && m_pso && m_everBaked) ? 1.0f : 0.0f;
        out.probeCountX    = m_countX;
        out.probeCountY    = m_countY;
        out.probeCountZ    = m_countZ;
        out.probeTotalCount = m_countX * m_countY * m_countZ;
    }

    // =========================================================================
    // FillUpdateCB
    // =========================================================================

    void IrradianceProbeGrid::FillUpdateCB(const UpdateDesc& desc,
                                            uint32_t baseIdx, uint32_t count,
                                            GIUpdateCBData& out) const
    {
        out.probeOrigin[0]  = m_originX;
        out.probeOrigin[1]  = m_originY;
        out.probeOrigin[2]  = m_originZ;
        out.pad0            = 0.0f;
        out.probeSpacing[0] = m_spacingX;
        out.probeSpacing[1] = m_spacingY;
        out.probeSpacing[2] = m_spacingZ;
        out.pad1            = 0.0f;
        out.probeCountX     = m_countX;
        out.probeCountY     = m_countY;
        out.probeCountZ     = m_countZ;
        out.baseProbeIndex  = baseIdx;
        // First pass after a reset writes the traced irradiance at full weight.
        // The probe buffer is zero-initialised (AllocateProbeBuffer) and every probe is
        // dispatched exactly once per pass, so blending with m_emaAlpha here would leave
        // the baked field at alpha * radiance -- e.g. 1% of the correct value at
        // ema_alpha = 0.01, which reads as "GI does nothing". EMA is meaningful only
        // once a full pass has produced valid data (continuous-mode refresh passes).
        out.emaAlpha        = IsBaked() ? m_emaAlpha : 1.0f;
        out.maxTraceDistance = m_maxTraceDistance;
        out.shadowBias      = desc.shadowBias;
        out.frameIndex      = desc.frameIndex;
        out.dirLightDir[0]  = desc.dirLightDir[0];
        out.dirLightDir[1]  = desc.dirLightDir[1];
        out.dirLightDir[2]  = desc.dirLightDir[2];
        out.dirLightIntensity = desc.dirLightIntensity;
        out.dirLightColor[0] = desc.dirLightColor[0];
        out.dirLightColor[1] = desc.dirLightColor[1];
        out.dirLightColor[2] = desc.dirLightColor[2];
        out.ambientIntensity = desc.ambientIntensity;
        out.ambientColor[0]  = desc.ambientColor[0];
        out.ambientColor[1]  = desc.ambientColor[1];
        out.ambientColor[2]  = desc.ambientColor[2];
        out.probesThisDispatch = count;

        out.pointLightCount = desc.pointLights
            ? std::min(static_cast<uint32_t>(desc.pointLights->size()), kMaxPointLights) : 0u;
        out.spotLightCount = desc.spotLights
            ? std::min(static_cast<uint32_t>(desc.spotLights->size()), kMaxSpotLights) : 0u;
        out.pad2 = 0u;
        out.pad3 = 0u;
    }

    // =========================================================================
    // FillLightDataBuffer
    // Converts desc.pointLights/spotLights into the GPU light layout expected by
    // GI_ProbeUpdate_CS.hlsl and writes them into the persistently-mapped light buffer.
    // Same conversion (yaw/pitch -> direction, color*intensity) as
    // GpuSoftwareRayTracer's SWRT reflection/ReSTIR light upload.
    // =========================================================================

    void IrradianceProbeGrid::FillLightDataBuffer(const UpdateDesc& desc) const
    {
        if (!m_lightDataMapped) return;

        auto* pointDst = reinterpret_cast<GIPointLightGpu*>(m_lightDataMapped);
        if (desc.pointLights) {
            const uint32_t nPt = std::min(static_cast<uint32_t>(desc.pointLights->size()), kMaxPointLights);
            for (uint32_t i = 0; i < nPt; ++i) {
                const RenderPointLight& src = (*desc.pointLights)[i];
                pointDst[i].pos[0] = src.pos[0];
                pointDst[i].pos[1] = src.pos[1];
                pointDst[i].pos[2] = src.pos[2];
                pointDst[i].range  = src.range;
                pointDst[i].colorIntensity[0] = src.color[0] * src.intensity;
                pointDst[i].colorIntensity[1] = src.color[1] * src.intensity;
                pointDst[i].colorIntensity[2] = src.color[2] * src.intensity;
                pointDst[i].pad = 0.0f;
            }
        }

        uint8_t* spotBase = m_lightDataMapped + static_cast<size_t>(kMaxPointLights) * sizeof(GIPointLightGpu);
        auto* spotDst = reinterpret_cast<GISpotLightGpu*>(spotBase);
        if (desc.spotLights) {
            const uint32_t nSp = std::min(static_cast<uint32_t>(desc.spotLights->size()), kMaxSpotLights);
            for (uint32_t i = 0; i < nSp; ++i) {
                const RenderSpotLight& src = (*desc.spotLights)[i];
                spotDst[i].pos[0] = src.pos[0];
                spotDst[i].pos[1] = src.pos[1];
                spotDst[i].pos[2] = src.pos[2];
                spotDst[i].range  = src.range;
                float dir[3]{};
                Math::DirectionFromYawPitch(src.yaw, src.pitch, dir);
                spotDst[i].dir[0] = dir[0];
                spotDst[i].dir[1] = dir[1];
                spotDst[i].dir[2] = dir[2];
                spotDst[i].cosInner = std::cos(src.innerAngle);
                spotDst[i].colorIntensity[0] = src.color[0] * src.intensity;
                spotDst[i].colorIntensity[1] = src.color[1] * src.intensity;
                spotDst[i].colorIntensity[2] = src.color[2] * src.intensity;
                spotDst[i].cosOuter = std::cos(src.outerAngle);
            }
        }
    }

    // =========================================================================
    // UpdateProbes
    // =========================================================================

    bool IrradianceProbeGrid::UpdateProbes(const UpdateDesc& desc,
                                            const GpuSoftwareRayTracer::BvhGpuAddresses& bvhAddrs,
                                            IRHIDevice& device,
                                            CommandList& cmdList)
    {
        if (!m_initialized || !m_pso || !m_enabled) {
            if (m_initialized && !m_pso && !m_psoMissingLogged) {
                m_psoMissingLogged = true;
                OutputDebugStringA("IrradianceProbeGrid::UpdateProbes: PSO is null (GI shader compile failed). GI bake will not proceed.\n");
            }
            // Even when disabled, update the probe grid CB so the shader knows GI is off
            if (m_cbMapped) {
                GIProbeGridCBData gridCB{};
                FillProbeGridCB(gridCB);
                memcpy(m_cbMapped, &gridCB, sizeof(gridCB));
            }
            return true;
        }

        // Ensure probe buffer matches current grid size
        if (GetTotalProbeCount() != m_probeBufferCapacity) {
            if (!AllocateProbeBuffer(device)) return false;
        }

        const uint32_t totalProbes = GetTotalProbeCount();
        if (totalProbes == 0) return true;

        // Wrap round-robin index
        if (m_nextProbeIdx >= totalProbes)
            m_nextProbeIdx = 0u;

        const uint32_t probesThisFrame = std::min(kProbesPerFrame, totalProbes - m_nextProbeIdx);

        // ---- Write constants ----
        if (m_cbMapped) {
            // Slot 0: GIProbeGridCBData (for PBR_PS binding)
            GIProbeGridCBData gridCB{};
            FillProbeGridCB(gridCB);
            memcpy(m_cbMapped, &gridCB, sizeof(gridCB));

            // Slot 1: GIUpdateCBData (for probe update CS)
            GIUpdateCBData updateCB{};
            FillUpdateCB(desc, m_nextProbeIdx, probesThisFrame, updateCB);
            memcpy(m_cbMapped + 256u, &updateCB, sizeof(updateCB));
        }

        // ---- Write punctual light data (t6/t7) ----
        FillLightDataBuffer(desc);

        ID3D12GraphicsCommandList* cl = cmdList.Get();
        if (!cl) return false;

        // ---- Transition probe buffer: PIXEL_SHADER_RESOURCE → UNORDERED_ACCESS ----
        Resource* probeBuffer = GetProbeBufferResource();
        Resource* cbBuffer = GetConstantBufferResource();
        if (!probeBuffer || !probeBuffer->IsValid() || !cbBuffer || !cbBuffer->IsValid()) {
            return false;
        }

        auto barToUAV = CD3DX12_RESOURCE_BARRIER::Transition(
            probeBuffer->Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl->ResourceBarrier(1, &barToUAV);

        // ---- Dispatch ----
        cl->SetPipelineState(m_pso.Get());
        cl->SetComputeRootSignature(m_rootSig.Get());

        const D3D12_GPU_VIRTUAL_ADDRESS updateCbGpu = cbBuffer->GetGPUVirtualAddress() + 256u;
        cl->SetComputeRootConstantBufferView(0, updateCbGpu);
        cl->SetComputeRootShaderResourceView (1, bvhAddrs.bvhNodes);
        cl->SetComputeRootShaderResourceView (2, bvhAddrs.triangles);
        cl->SetComputeRootShaderResourceView (3, bvhAddrs.meshInfo);
        cl->SetComputeRootShaderResourceView (4, bvhAddrs.instances);
        cl->SetComputeRootShaderResourceView (5, bvhAddrs.tlasNodes);
        cl->SetComputeRootShaderResourceView (6, bvhAddrs.materials);
        const D3D12_GPU_VIRTUAL_ADDRESS pointLightsVA = m_lightDataBuffer.GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS spotLightsVA =
            pointLightsVA + static_cast<UINT64>(kMaxPointLights) * sizeof(GIPointLightGpu);
        cl->SetComputeRootShaderResourceView (7, pointLightsVA);
        cl->SetComputeRootShaderResourceView (8, spotLightsVA);
        cl->SetComputeRootUnorderedAccessView(9, probeBuffer->GetGPUVirtualAddress());

        // One thread group per probe, 64 threads per group
#if defined(_DEBUG)
        DebugIncrementDispatchCount();
        {
            char buf[256];
            snprintf(buf, sizeof(buf),
                "[DispatchDiag] idx=%u pass=IrradianceProbeGrid_UpdateProbes instancesVA=0x%llX gen=%llu instancesHandleId=%llu\n",
                DebugGetDispatchCount(), (unsigned long long)bvhAddrs.instances,
                (unsigned long long)bvhAddrs.generation, (unsigned long long)bvhAddrs.instancesHandleId);
            DebugLog(buf);
        }
#endif
        cl->Dispatch(probesThisFrame, 1, 1);

        // ---- UAV barrier (ensure writes are visible) ----
        auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(probeBuffer->Get());
        cl->ResourceBarrier(1, &uavBarrier);

        // ---- Transition back: UNORDERED_ACCESS → PIXEL_SHADER_RESOURCE ----
        auto barToSRV = CD3DX12_RESOURCE_BARRIER::Transition(
            probeBuffer->Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cl->ResourceBarrier(1, &barToSRV);

        // Advance round-robin index
        m_nextProbeIdx           += probesThisFrame;
        m_totalProbesDispatched  += probesThisFrame;
        if (IsBaked()) {
            m_everBaked = true;
            // FillProbeGridCB gates giEnabled on m_everBaked; refresh the CB after
            // the final dispatch so lighting can consume the completed probes.
            FlushGridCB();
        }

        return true;
    }

    uint32_t IrradianceProbeGrid::GetBakedProbeCount() const
    {
        const uint32_t total = GetTotalProbeCount();
        return std::min(m_totalProbesDispatched, total);
    }

    uint64_t IrradianceProbeGrid::GetProbeDataSizeBytes() const
    {
        return static_cast<uint64_t>(GetTotalProbeCount()) *
               9u *
               sizeof(float) * 4u;
    }

    bool IrradianceProbeGrid::ExportProbeData(IRHIDevice& device, std::vector<uint8_t>& outData) const
    {
        outData.clear();
        if (!m_initialized || !m_everBaked || !m_probeBufferHandle.IsValid()) {
            return false;
        }

        const uint64_t byteSize = GetProbeDataSizeBytes();
        if (byteSize == 0u) {
            return false;
        }

        Resource* probeBuffer = GetProbeBufferResource();
        if (!probeBuffer || !probeBuffer->IsValid()) {
            return false;
        }

        device.WaitForGPU();

        RhiBufferDesc readbackDesc{};
        readbackDesc.sizeInBytes = byteSize;
        readbackDesc.strideInBytes = sizeof(float) * 4u;
        readbackDesc.usage = RhiBufferUsageFlags::CopyDest;
        readbackDesc.memoryUsage = RhiMemoryUsage::GpuToCpu;
        readbackDesc.initialState = RhiResourceState::CopyDest;

        RhiBufferHandle readbackHandle = device.CreateRhiBuffer(readbackDesc);
        if (!readbackHandle.IsValid()) {
            return false;
        }

        bool ok = false;
        Resource* readbackBuffer = device.GetD3D12CompatibilityResource(readbackHandle);
        if (readbackBuffer && readbackBuffer->IsValid()) {
            CommandAllocator allocator;
            CommandList commandList;
            if (SUCCEEDED(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, allocator)) &&
                SUCCEEDED(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, commandList))) {
                auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(
                    probeBuffer->Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
                commandList.ResourceBarrier(1, &toCopy);
                commandList.CopyBufferRegion(*readbackBuffer, 0u, *probeBuffer, 0u, byteSize);
                auto toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                    probeBuffer->Get(),
                    D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                commandList.ResourceBarrier(1, &toSrv);

                if (SUCCEEDED(commandList.Close())) {
                    ID3D12CommandList* lists[] = { commandList.Get() };
                    device.GetCommandQueue().ExecuteCommandLists(1u, lists);
                    device.WaitForGPU();

                    outData.resize(static_cast<size_t>(byteSize));
                    ok = device.ReadRhiBuffer(readbackHandle, 0u, outData.data(), byteSize);
                    if (!ok) {
                        outData.clear();
                    }
                }
            }
        }

        device.DestroyRhiResource(readbackHandle);
        return ok;
    }

    bool IrradianceProbeGrid::ImportProbeData(IRHIDevice& device, const void* data, uint64_t sizeInBytes)
    {
        if (!m_initialized || !data || sizeInBytes == 0u ||
            sizeInBytes != GetProbeDataSizeBytes() ||
            !device.GetCapabilities().supportsRhiResourceCreation) {
            return false;
        }

        device.WaitForGPU();
        if (!AllocateProbeBuffer(device, data)) {
            return false;
        }

        Resource* probeBuffer = GetProbeBufferResource();
        if (!probeBuffer || !probeBuffer->IsValid()) {
            return false;
        }

        CommandAllocator allocator;
        CommandList commandList;
        if (FAILED(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, allocator)) ||
            FAILED(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, commandList))) {
            return false;
        }

        auto toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
            probeBuffer->Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList.ResourceBarrier(1, &toSrv);
        if (FAILED(commandList.Close())) {
            return false;
        }

        ID3D12CommandList* lists[] = { commandList.Get() };
        device.GetCommandQueue().ExecuteCommandLists(1u, lists);
        device.WaitForGPU();

        m_nextProbeIdx = GetTotalProbeCount();
        m_totalProbesDispatched = GetTotalProbeCount();
        m_everBaked = true;
        FlushGridCB();
        return true;
    }

    void IrradianceProbeGrid::ResetBakeState()
    {
        m_nextProbeIdx          = 0u;
        m_totalProbesDispatched = 0u;
    }

    bool IrradianceProbeGrid::ReallocAndClearProbeBuffer(IRHIDevice& device)
    {
        // Force a fresh zero-initialized allocation by invalidating the current capacity.
        m_probeBufferCapacity = 0u;
        ResetBakeState();
        m_everBaked = false; // stale SH data was just discarded; lighting must wait for a fresh pass
        return m_initialized ? AllocateProbeBuffer(device) : true;
    }

} // namespace SasamiRenderer
