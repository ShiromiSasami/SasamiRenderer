#define NOMINMAX
#include "Renderer/GI/IrradianceProbeGrid.h"

#include "Foundation/Math/MathUtil.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl.h>

#include "d3dx12.h"

using Microsoft::WRL::ComPtr;

namespace SasamiRenderer
{
    namespace
    {
        // ---- Minimal DXC helper (mirrors GpuSoftwareRayTracer's helper) ----
        HRESULT CreateDxcInstance(REFCLSID clsid, REFIID iid, LPVOID* out)
        {
            static HMODULE mod = LoadLibraryW(L"dxcompiler.dll");
            static auto fn = mod
                ? reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>(
                      GetProcAddress(mod, "DxcCreateInstance"))
                : nullptr;
            return fn ? fn(clsid, iid, out) : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        }

        std::filesystem::path GetExeDir()
        {
            wchar_t buf[MAX_PATH]{};
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            return std::filesystem::path(buf).parent_path();
        }

        std::filesystem::path FindProjectRoot(std::filesystem::path dir)
        {
            for (int depth = 0; depth < 16; ++depth) {
                if (std::filesystem::exists(dir / "Source" / "Renderer" / "Shaders")) return dir;
                auto p = dir.parent_path();
                if (p.empty() || p == dir) break;
                dir = p;
            }
            return {};
        }

        std::filesystem::path GetShaderRoot()
        {
            static const std::filesystem::path root = []() {
                auto pr = FindProjectRoot(GetExeDir());
                return pr.empty()
                    ? std::filesystem::path(L"Source/Renderer/Shaders")
                    : pr / L"Source" / L"Renderer" / L"Shaders";
            }();
            return root;
        }

        bool CompileCS(const wchar_t* relPath, const char* entry, ComPtr<ID3DBlob>& outBlob)
        {
            const std::filesystem::path srcPath = GetShaderRoot() / relPath;
            const std::filesystem::path incPath = GetShaderRoot();

            ComPtr<IDxcUtils> utils;
            ComPtr<IDxcCompiler3> compiler;
            if (FAILED(CreateDxcInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) return false;
            if (FAILED(CreateDxcInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) return false;

            ComPtr<IDxcBlobEncoding> src;
            if (FAILED(utils->LoadFile(srcPath.c_str(), nullptr, &src))) {
                OutputDebugStringA(("GI: failed to load shader: " + srcPath.string() + "\n").c_str());
                return false;
            }

            ComPtr<IDxcIncludeHandler> incHandler;
            utils->CreateDefaultIncludeHandler(&incHandler);

            const std::wstring srcW = srcPath.native();
            const std::wstring entW = [&]() {
                std::wstring w; for (char c : std::string(entry)) w += c; return w;
            }();
            const std::wstring incW = incPath.native();

            std::vector<LPCWSTR> args{
                srcW.c_str(), L"-E", entW.c_str(),
                L"-T", L"cs_6_6",
                L"-I", incW.c_str(),
                L"-HV", L"2021",
                L"-WX",
            };
#if defined(_DEBUG)
            args.push_back(L"-Zi"); args.push_back(L"-Od");
#else
            args.push_back(L"-O3");
#endif

            DxcBuffer buf{ src->GetBufferPointer(), src->GetBufferSize(), DXC_CP_ACP };
            ComPtr<IDxcResult> result;
            if (FAILED(compiler->Compile(&buf, args.data(), (UINT32)args.size(), incHandler.Get(), IID_PPV_ARGS(&result)))) {
                return false;
            }

            ComPtr<IDxcBlobUtf8> errors;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
                errors && errors->GetStringLength() > 0) {
                OutputDebugStringA(errors->GetStringPointer());
            }

            HRESULT hr = S_OK;
            result->GetStatus(&hr);
            if (FAILED(hr)) {
                OutputDebugStringA(("GI: shader compilation failed: " + srcPath.string() + "\n").c_str());
                return false;
            }

            ComPtr<IDxcBlob> code;
            result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&code), nullptr);
            if (!code) return false;

            // Copy to ID3DBlob for compatibility with PSO creation
            ID3DBlob* blob = nullptr;
            D3DCreateBlob(code->GetBufferSize(), &blob);
            memcpy(blob->GetBufferPointer(), code->GetBufferPointer(), code->GetBufferSize());
            outBlob.Attach(blob);
            return true;
        }

    } // anonymous namespace

    // =========================================================================
    // Constructor / Destructor
    // =========================================================================

    IrradianceProbeGrid::IrradianceProbeGrid() = default;

    IrradianceProbeGrid::~IrradianceProbeGrid()
    {
        if (m_cbMapped && m_cbBuffer.IsValid()) {
            m_cbBuffer.Unmap(0, nullptr);
        }
        m_cbMapped = nullptr;
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
    // Initialize
    // =========================================================================

    bool IrradianceProbeGrid::Initialize(IRHIDevice& device)
    {
        if (m_initialized) return true;

        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // ---- Persistently-mapped upload CB ----
        // Slot 0 (256 bytes): GIProbeGridCBData   → bound to b2 in PBR_PS
        // Slot 1 (256 bytes): GIUpdateCBData       → bound to b0 in GI_ProbeUpdate_CS
        constexpr UINT64 kCbSlotSize = 256u;
        constexpr UINT64 kCbTotalSize = kCbSlotSize * 2u;
        if (!ResourceUploadUtility::CreateUploadBuffer(device, kCbTotalSize,
                                                        m_cbBuffer,
                                                        reinterpret_cast<void**>(&m_cbMapped))) {
            return false;
        }
        // Zero-initialise (giEnabled = 0.0f → GI disabled until first update writes it)
        memset(m_cbMapped, 0, kCbTotalSize);

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

    bool IrradianceProbeGrid::AllocateProbeBuffer(IRHIDevice& device)
    {
        const uint32_t totalProbes = m_countX * m_countY * m_countZ;
        if (totalProbes == m_probeBufferCapacity && m_probeBuffer.IsValid())
            return true;

        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // float4[9] per probe
        const UINT64 bufSize = static_cast<UINT64>(totalProbes) * 9u * sizeof(float) * 4u;

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

        m_probeBufferCapacity = totalProbes;

        // ---- SRV (slot 0) ----
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = m_descHeap.GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format                  = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement     = 0;
        srvDesc.Buffer.NumElements      = totalProbes * 9u;
        srvDesc.Buffer.StructureByteStride = sizeof(float) * 4u;
        dev->CreateShaderResourceView(m_probeBuffer.Get(), &srvDesc, srvCpu);

        // ---- UAV (slot 1) ----
        D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = m_descHeap.GetCPUDescriptorHandleForHeapStart();
        uavCpu.ptr += m_descStride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format              = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension       = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements  = totalProbes * 9u;
        uavDesc.Buffer.StructureByteStride = sizeof(float) * 4u;
        dev->CreateUnorderedAccessView(m_probeBuffer.Get(), nullptr, &uavDesc, uavCpu);

        // ---- GPU-visible SRV/UAV handles for root descriptors ----
        // Note: we use root SRV/UAV (inline) so no GPU-visible descriptor heap needed.
        // m_probeSrv and m_probeUav are not used as descriptor table handles;
        // instead callers use GetProbeDataGpuVA() directly.
        m_probeSrv = {};  // not used for root SRV
        m_probeUav = {};  // not used for root UAV

        // Refresh the GPU CB so any caller of ReallocProbeBuffer() (e.g. FitProbeGridToScene)
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
        return m_cbBuffer.IsValid() ? m_cbBuffer.GetGPUVirtualAddress() : 0u;
    }

    // =========================================================================
    // GetProbeDataGpuVA
    // =========================================================================

    D3D12_GPU_VIRTUAL_ADDRESS IrradianceProbeGrid::GetProbeDataGpuVA() const
    {
        return m_probeBuffer.IsValid() ? m_probeBuffer.GetGPUVirtualAddress() : 0u;
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
        //  [7] Root UAV  (u0): g_probeSHOutput

        D3D12_ROOT_PARAMETER params[8]{};

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

        // [7] UAV u0
        params[7].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[7].Descriptor.ShaderRegister = 0;
        params[7].Descriptor.RegisterSpace  = 0;
        params[7].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 8;
        rsDesc.pParameters   = params;
        rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> rsBlob, rsError;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                               rsBlob.ReleaseAndGetAddressOf(),
                                               rsError.ReleaseAndGetAddressOf()))) {
            if (rsError) OutputDebugStringA((char*)rsError->GetBufferPointer());
            return false;
        }
        if (FAILED(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                            IID_PPV_ARGS(m_rootSig.ReleaseAndGetAddressOf())))) {
            return false;
        }

        // Compile and create PSO
        ComPtr<ID3DBlob> cs;
        if (!CompileCS(L"GI/GI_ProbeUpdate_CS.hlsl", "CS_ProbeUpdate", cs)) {
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_rootSig.Get();
        pso.CS             = { cs->GetBufferPointer(), cs->GetBufferSize() };
        if (FAILED(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(m_pso.ReleaseAndGetAddressOf())))) {
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
        out.giEnabled      = (m_enabled && m_pso) ? 1.0f : 0.0f;
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
        out.emaAlpha        = m_emaAlpha;
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

        ID3D12GraphicsCommandList* cl = cmdList.Get();
        if (!cl) return false;

        // ---- Transition probe buffer: PIXEL_SHADER_RESOURCE → UNORDERED_ACCESS ----
        auto barToUAV = CD3DX12_RESOURCE_BARRIER::Transition(
            m_probeBuffer.Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl->ResourceBarrier(1, &barToUAV);

        // ---- Dispatch ----
        cl->SetPipelineState(m_pso.Get());
        cl->SetComputeRootSignature(m_rootSig.Get());

        const D3D12_GPU_VIRTUAL_ADDRESS updateCbGpu = m_cbBuffer.GetGPUVirtualAddress() + 256u;
        cl->SetComputeRootConstantBufferView(0, updateCbGpu);
        cl->SetComputeRootShaderResourceView (1, bvhAddrs.bvhNodes);
        cl->SetComputeRootShaderResourceView (2, bvhAddrs.triangles);
        cl->SetComputeRootShaderResourceView (3, bvhAddrs.meshInfo);
        cl->SetComputeRootShaderResourceView (4, bvhAddrs.instances);
        cl->SetComputeRootShaderResourceView (5, bvhAddrs.tlasNodes);
        cl->SetComputeRootShaderResourceView (6, bvhAddrs.materials);
        cl->SetComputeRootUnorderedAccessView(7, m_probeBuffer.GetGPUVirtualAddress());

        // One thread group per probe, 64 threads per group
        cl->Dispatch(probesThisFrame, 1, 1);

        // ---- UAV barrier (ensure writes are visible) ----
        auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_probeBuffer.Get());
        cl->ResourceBarrier(1, &uavBarrier);

        // ---- Transition back: UNORDERED_ACCESS → PIXEL_SHADER_RESOURCE ----
        auto barToSRV = CD3DX12_RESOURCE_BARRIER::Transition(
            m_probeBuffer.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cl->ResourceBarrier(1, &barToSRV);

        // Advance round-robin index
        m_nextProbeIdx           += probesThisFrame;
        m_totalProbesDispatched  += probesThisFrame;

        return true;
    }

    float IrradianceProbeGrid::GetBakeProgress() const
    {
        const uint32_t total = GetTotalProbeCount();
        if (total == 0) return 0.0f;
        return std::min(1.0f, static_cast<float>(m_totalProbesDispatched) /
                              static_cast<float>(total));
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
        return m_initialized ? AllocateProbeBuffer(device) : true;
    }

} // namespace SasamiRenderer
