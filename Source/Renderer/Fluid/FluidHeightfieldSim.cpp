#define NOMINMAX
#include "Renderer/Fluid/FluidHeightfieldSim.h"

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
        // Buffer read state shared by the compute shader's SRV (t0) and the vertex
        // shader's SRV (t0): either stage may sample whichever buffer currently
        // holds the "not being written this frame" role without a transition.
        constexpr D3D12_RESOURCE_STATES kReadState =
            static_cast<D3D12_RESOURCE_STATES>(
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        bool CompileCS(ID3D12Device* dev, const wchar_t* relPath, const char* entry, ComPtr<ID3DBlob>& outBlob,
                       std::string& outError)
        {
            const std::wstring csTarget = ShaderCompilationService::ResolveEffectiveComputeShaderTarget(dev, "6_6");
            const std::string profileNarrow(csTarget.begin(), csTarget.end());

            std::vector<uint8_t> bytecode;
            if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc(
                    "Fluid", relPath, entry, profileNarrow.c_str(), bytecode)) {
                outError = "ShaderCompilationService: failed to compile shader";
                return false;
            }

            ID3DBlob* blob = nullptr;
            if (FAILED(D3DCreateBlob(bytecode.size(), &blob)) || !blob) {
                return false;
            }
            memcpy(blob->GetBufferPointer(), bytecode.data(), bytecode.size());
            outBlob.Attach(blob);
            return true;
        }

    } // anonymous namespace

    // =========================================================================
    // Constructor / Destructor
    // =========================================================================

    FluidHeightfieldSim::FluidHeightfieldSim() = default;

    FluidHeightfieldSim::~FluidHeightfieldSim()
    {
        if (m_cbMapped) {
            if (Resource* cb = GetConstantBufferResource()) {
                cb->Unmap(0, nullptr);
            }
        }
        m_cbMapped = nullptr;
        if (m_device) {
            if (m_cbBufferHandle.IsValid()) m_device->DestroyRhiResource(m_cbBufferHandle);
            for (int i = 0; i < 2; ++i) {
                if (m_bufferHandle[i].IsValid()) m_device->DestroyRhiResource(m_bufferHandle[i]);
            }
        }
        m_cbBufferHandle = {};
        m_cbBufferCompat = nullptr;
        for (int i = 0; i < 2; ++i) {
            m_bufferHandle[i] = {};
            m_bufferCompat[i] = nullptr;
        }
    }

    // =========================================================================
    // SetCellSize / SetGridCount / FitToSceneBounds
    // =========================================================================

    void FluidHeightfieldSim::SetCellSize(float s)
    {
        m_cellSize = (s > 0.001f) ? s : 0.001f;
    }

    void FluidHeightfieldSim::SetGridCount(uint32_t cx, uint32_t cz)
    {
        m_countX = std::max(1u, cx);
        m_countZ = std::max(1u, cz);
    }

    void FluidHeightfieldSim::FitToSceneBounds(float bMinX, float bMinZ, float bMaxX, float bMaxZ, float margin)
    {
        const float extX = (bMaxX - bMinX) + margin * 2.0f;
        const float extZ = (bMaxZ - bMinZ) + margin * 2.0f;

        m_originX = bMinX - margin;
        m_originZ = bMinZ - margin;

        const float cs = std::max(m_cellSize, 0.01f);
        m_countX = std::max(2u, static_cast<uint32_t>(std::ceil(extX / cs)) + 1u);
        m_countZ = std::max(2u, static_cast<uint32_t>(std::ceil(extZ / cs)) + 1u);
    }

    // =========================================================================
    // Initialize
    // =========================================================================

    bool FluidHeightfieldSim::Initialize(IRHIDevice& device)
    {
        if (m_initialized) return true;
        m_device = &device;

        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // ---- Persistently-mapped upload CB: single 256-byte slot for FluidUpdateCBData ----
        constexpr UINT64 kCbSize = 256u;
        if (device.GetCapabilities().supportsRhiResourceCreation) {
            RhiBufferDesc cbDesc{};
            cbDesc.sizeInBytes = kCbSize;
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
            if (!ResourceUploadUtility::CreateUploadBuffer(device, kCbSize, m_cbBuffer,
                                                           reinterpret_cast<void**>(&m_cbMapped))) {
                return false;
            }
            m_cbBufferHandle = {};
            m_cbBufferCompat = nullptr;
        }
        memset(m_cbMapped, 0, kCbSize);

        // ---- Allocate ping-pong height/velocity buffers ----
        if (!AllocateBuffers(device)) return false;

        // ---- Compile shader + create pipeline ----
        if (!CreatePipeline(device)) {
            OutputDebugStringA("FluidHeightfieldSim: shader compilation failed — fluid sim will be disabled.\n");
            // Non-fatal: fluid sim disabled but buffers/bindings remain valid (flat surface).
        }

        m_initialized = true;
        return true;
    }

    // =========================================================================
    // AllocateBuffers
    // Allocates (or reallocates) both ping-pong buffers, zero-initialized, and
    // leaves both in the steady-state read state.
    // =========================================================================

    bool FluidHeightfieldSim::AllocateBuffers(IRHIDevice& device)
    {
        const uint32_t totalCells = GetTotalCellCount();
        if (totalCells == 0u) return false;

        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        const UINT64 bufSize = static_cast<UINT64>(totalCells) * sizeof(float) * 2u; // float2 per cell
        std::vector<float> zeroData(static_cast<size_t>(totalCells) * 2u, 0.0f);

        // Update() may call this mid-run when the grid size changes (see the
        // GetTotalCellCount() != m_bufferCapacity check), while a previous frame's
        // compute dispatch may still be reading/writing the old buffers on the GPU.
        // Flush before destroying to avoid freeing a resource still in flight.
        if (m_bufferHandle[0].IsValid() || m_bufferHandle[1].IsValid()) {
            device.WaitForGPU();
        }

        for (int i = 0; i < 2; ++i) {
            m_buffer[i].Reset();
            if (m_bufferHandle[i].IsValid()) {
                device.DestroyRhiResource(m_bufferHandle[i]);
            }
            m_bufferCompat[i] = nullptr;
            m_bufferHandle[i] = {};

            if (device.GetCapabilities().supportsRhiResourceCreation) {
                RhiBufferDesc rhiDesc{};
                rhiDesc.sizeInBytes = bufSize;
                rhiDesc.strideInBytes = sizeof(float) * 2u;
                rhiDesc.usage = RhiBufferUsageFlags::Structured |
                                RhiBufferUsageFlags::ShaderResource |
                                RhiBufferUsageFlags::UnorderedAccess |
                                RhiBufferUsageFlags::CopySource |
                                RhiBufferUsageFlags::CopyDest;
                rhiDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
                rhiDesc.initialState = RhiResourceState::CopyDest;
                m_bufferHandle[i] = device.CreateRhiBuffer(rhiDesc, zeroData.data());
                m_bufferCompat[i] = device.GetD3D12CompatibilityResource(m_bufferHandle[i]);
            }

            if (!m_bufferCompat[i]) {
                if (m_bufferHandle[i].IsValid()) {
                    device.DestroyRhiResource(m_bufferHandle[i]);
                    m_bufferHandle[i] = {};
                }
                // Fallback: raw D3D12 default-heap buffer with UAV flag, created directly in the
                // steady-state read state. Content is undefined on this path (mirrors
                // IrradianceProbeGrid's equivalent raw-D3D12 fallback, which has the same limitation).
                D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                desc.Width     = bufSize;
                desc.Height    = desc.DepthOrArraySize = desc.MipLevels = 1;
                desc.SampleDesc.Count = 1;
                desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                if (FAILED(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                           kReadState,
                                                           nullptr, m_buffer[i]))) {
                    return false;
                }
            }
        }

        m_bufferCapacity = totalCells;
        m_readIndex = 0;

        // Buffers created via CreateRhiBuffer(desc, initialData) are left in CopyDest after the
        // upload copy (mirrors IrradianceProbeGrid::ImportProbeData); transition both to the
        // steady-state read state here. Raw-D3D12-fallback buffers are already in that state.
        bool anyNeedsTransition = false;
        for (int i = 0; i < 2; ++i) {
            if (m_bufferCompat[i] != nullptr) { anyNeedsTransition = true; break; }
        }
        if (anyNeedsTransition) {
            CommandAllocator allocator;
            CommandList commandList;
            if (FAILED(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, allocator)) ||
                FAILED(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, commandList))) {
                return false;
            }
            ID3D12GraphicsCommandList* cl = commandList.Get();
            D3D12_RESOURCE_BARRIER barriers[2]{};
            UINT barrierCount = 0u;
            for (int i = 0; i < 2; ++i) {
                if (m_bufferCompat[i] == nullptr) continue;
                Resource* buf = GetBufferResource(i);
                barriers[barrierCount++] = CD3DX12_RESOURCE_BARRIER::Transition(
                    buf->Get(), D3D12_RESOURCE_STATE_COPY_DEST, kReadState);
            }
            cl->ResourceBarrier(barrierCount, barriers);
            if (FAILED(commandList.Close())) return false;

            ID3D12CommandList* lists[] = { commandList.Get() };
            device.GetCommandQueue().ExecuteCommandLists(1u, lists);
            device.WaitForGPU();
        }

        return true;
    }

    Resource* FluidHeightfieldSim::GetBufferResource(int index) const
    {
        return m_bufferCompat[index] ? m_bufferCompat[index] : const_cast<Resource*>(&m_buffer[index]);
    }

    Resource* FluidHeightfieldSim::GetConstantBufferResource() const
    {
        return m_cbBufferCompat ? m_cbBufferCompat : const_cast<Resource*>(&m_cbBuffer);
    }

    D3D12_GPU_VIRTUAL_ADDRESS FluidHeightfieldSim::GetCurrentHeightGpuVA() const
    {
        Resource* buf = GetBufferResource(m_readIndex);
        return (buf && buf->IsValid()) ? buf->GetGPUVirtualAddress() : 0u;
    }

    D3D12_GPU_VIRTUAL_ADDRESS FluidHeightfieldSim::GetUpdateCBGpuVA() const
    {
        Resource* cb = GetConstantBufferResource();
        return (cb && cb->IsValid()) ? cb->GetGPUVirtualAddress() : 0u;
    }

    // =========================================================================
    // CreatePipeline
    // =========================================================================

    bool FluidHeightfieldSim::CreatePipeline(IRHIDevice& device)
    {
        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // Root signature:
        //  [0] Root CBV (b0): FluidUpdateCB
        //  [1] Root SRV (t0): read (height, velocity) buffer
        //  [2] Root UAV (u0): write (height, velocity) buffer

        D3D12_ROOT_PARAMETER params[3]{};

        params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace  = 0;
        params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 0;
        params[1].Descriptor.RegisterSpace  = 0;
        params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[2].Descriptor.ShaderRegister = 0;
        params[2].Descriptor.RegisterSpace  = 0;
        params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 3;
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

        ComPtr<ID3DBlob> cs;
        std::string compileError;
        if (!CompileCS(dev, L"Compute/Fluid/FluidHeightfield_CS.hlsl", "CS_FluidUpdate", cs, compileError)) {
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
    // FillUpdateCB
    // =========================================================================

    void FluidHeightfieldSim::FillUpdateCB(float dt, FluidUpdateCBData& out) const
    {
        out.originX       = m_originX;
        out.originZ       = m_originZ;
        out.cellSize       = m_cellSize;
        out.invCellSizeSq  = 1.0f / (m_cellSize * m_cellSize);
        out.countX         = m_countX;
        out.countZ         = m_countZ;
        out.waveSpeedSq     = m_waveSpeed * m_waveSpeed;
        out.damping         = m_damping;
        out.dt              = dt;
        if (m_hasPendingSplash) {
            out.splashActive   = 1u;
            out.splashX        = m_pendingSplash.worldX;
            out.splashZ        = m_pendingSplash.worldZ;
            out.splashRadius   = m_pendingSplash.radius;
            out.splashStrength = m_pendingSplash.strength;
        } else {
            out.splashActive   = 0u;
            out.splashX        = 0.0f;
            out.splashZ        = 0.0f;
            out.splashRadius   = 0.0f;
            out.splashStrength = 0.0f;
        }
        out.surfaceHeight = m_surfaceHeight;
        out.pad1 = 0.0f;
    }

    // =========================================================================
    // Update
    // =========================================================================

    bool FluidHeightfieldSim::PrepareFrame(IRHIDevice& device)
    {
        if (!m_initialized || !m_pso || !m_enabled) return true;
        if (GetTotalCellCount() != m_bufferCapacity) {
            return AllocateBuffers(device);
        }
        return true;
    }

    bool FluidHeightfieldSim::Update(float dt, IRHIDevice& device, CommandList& cmdList)
    {
        if (!m_initialized || !m_pso || !m_enabled) {
            if (m_initialized && !m_pso && !m_psoMissingLogged) {
                m_psoMissingLogged = true;
                OutputDebugStringA("FluidHeightfieldSim::Update: PSO is null (shader compile failed). Fluid sim will not proceed.\n");
            }
            return true;
        }

        // Buffer (re)allocation happens in PrepareFrame(), called earlier in the frame
        // before the render graph records any reads of the ping-pong buffers.
        const uint32_t totalCells = GetTotalCellCount();
        if (totalCells != m_bufferCapacity) return false;
        if (totalCells == 0) return true;

        const int writeIndex = 1 - m_readIndex;

        if (m_cbMapped) {
            FluidUpdateCBData cb{};
            FillUpdateCB(dt, cb);
            memcpy(m_cbMapped, &cb, sizeof(cb));
        }
        m_hasPendingSplash = false; // consumed by this dispatch

        ID3D12GraphicsCommandList* cl = cmdList.Get();
        if (!cl) return false;

        Resource* readBuffer  = GetBufferResource(m_readIndex);
        Resource* writeBuffer = GetBufferResource(writeIndex);
        Resource* cbBuffer    = GetConstantBufferResource();
        if (!readBuffer || !readBuffer->IsValid() || !writeBuffer || !writeBuffer->IsValid() ||
            !cbBuffer || !cbBuffer->IsValid()) {
            return false;
        }

        // ---- Transition write-target buffer: read state → UNORDERED_ACCESS ----
        auto barToUAV = CD3DX12_RESOURCE_BARRIER::Transition(
            writeBuffer->Get(), kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl->ResourceBarrier(1, &barToUAV);

        // ---- Dispatch ----
        cl->SetPipelineState(m_pso.Get());
        cl->SetComputeRootSignature(m_rootSig.Get());
        cl->SetComputeRootConstantBufferView(0, cbBuffer->GetGPUVirtualAddress());
        cl->SetComputeRootShaderResourceView(1, readBuffer->GetGPUVirtualAddress());
        cl->SetComputeRootUnorderedAccessView(2, writeBuffer->GetGPUVirtualAddress());

        // [numthreads(8,8,1)] in FluidHeightfield_CS.hlsl
        const uint32_t groupsX = (m_countX + 7u) / 8u;
        const uint32_t groupsZ = (m_countZ + 7u) / 8u;
#if defined(_DEBUG)
        DebugIncrementDispatchCount();
#endif
        cl->Dispatch(groupsX, groupsZ, 1);

        // ---- UAV barrier (ensure writes are visible) ----
        auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(writeBuffer->Get());
        cl->ResourceBarrier(1, &uavBarrier);

        // ---- Transition back: UNORDERED_ACCESS → read state ----
        auto barToRead = CD3DX12_RESOURCE_BARRIER::Transition(
            writeBuffer->Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState);
        cl->ResourceBarrier(1, &barToRead);

        m_readIndex = writeIndex;
        return true;
    }

} // namespace SasamiRenderer
