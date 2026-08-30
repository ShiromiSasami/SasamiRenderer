#define NOMINMAX
#include "Renderer/Particles/ParticleSystem.h"

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
        // Buffer read state shared by the render pass's vertex-shader SRV (t0): the buffer
        // is only bounced to UNORDERED_ACCESS while the compute dispatch is writing it.
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
                    "Particles", relPath, entry, profileNarrow.c_str(), bytecode)) {
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

    ParticleSystem::ParticleSystem() = default;

    ParticleSystem::~ParticleSystem()
    {
        if (m_cbMapped) {
            if (Resource* cb = GetConstantBufferResource()) {
                cb->Unmap(0, nullptr);
            }
        }
        m_cbMapped = nullptr;
        if (m_device) {
            if (m_cbBufferHandle.IsValid()) m_device->DestroyRhiResource(m_cbBufferHandle);
            if (m_bufferHandle.IsValid()) m_device->DestroyRhiResource(m_bufferHandle);
        }
        m_cbBufferHandle = {};
        m_cbBufferCompat = nullptr;
        m_bufferHandle = {};
        m_bufferCompat = nullptr;
    }

    // =========================================================================
    // SetCapacity
    // =========================================================================

    void ParticleSystem::SetCapacity(uint32_t capacity)
    {
        m_capacity = std::max(1u, capacity);
    }

    // =========================================================================
    // Initialize
    // =========================================================================

    bool ParticleSystem::Initialize(IRHIDevice& device)
    {
        if (m_initialized) return true;
        m_device = &device;

        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // ---- Persistently-mapped upload CB: single 256-byte slot for ParticleUpdateCBData ----
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

        // ---- Allocate particle buffer ----
        if (!AllocateBuffer(device)) return false;

        // ---- Compile shader + create pipeline ----
        if (!CreatePipeline(device)) {
            OutputDebugStringA("ParticleSystem: shader compilation failed — particle sim will be disabled.\n");
            // Non-fatal: particle sim disabled but buffers/bindings remain valid (empty buffer).
        }

        m_initialized = true;
        return true;
    }

    // =========================================================================
    // AllocateBuffer
    // Allocates (or reallocates) the particle buffer, zero-initialized (all dead
    // particles, life=0), and leaves it in the steady-state read state.
    // =========================================================================

    bool ParticleSystem::AllocateBuffer(IRHIDevice& device)
    {
        if (m_capacity == 0u) return false;

        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        const UINT64 bufSize = static_cast<UINT64>(m_capacity) * sizeof(Particle);
        std::vector<Particle> zeroData(static_cast<size_t>(m_capacity));

        m_buffer.Reset();
        if (m_bufferHandle.IsValid()) {
            // Reallocation (capacity change) may run mid-simulation while a previous
            // frame's compute dispatch is still reading/writing this buffer on the GPU.
            // Flush before destroying to avoid freeing a resource still in flight.
            device.WaitForGPU();
            device.DestroyRhiResource(m_bufferHandle);
        }
        m_bufferCompat = nullptr;
        m_bufferHandle = {};

        if (device.GetCapabilities().supportsRhiResourceCreation) {
            RhiBufferDesc rhiDesc{};
            rhiDesc.sizeInBytes = bufSize;
            rhiDesc.strideInBytes = sizeof(Particle);
            rhiDesc.usage = RhiBufferUsageFlags::Structured |
                            RhiBufferUsageFlags::ShaderResource |
                            RhiBufferUsageFlags::UnorderedAccess |
                            RhiBufferUsageFlags::CopySource |
                            RhiBufferUsageFlags::CopyDest;
            rhiDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
            rhiDesc.initialState = RhiResourceState::CopyDest;
            m_bufferHandle = device.CreateRhiBuffer(rhiDesc, zeroData.data());
            m_bufferCompat = device.GetD3D12CompatibilityResource(m_bufferHandle);
        }

        if (!m_bufferCompat) {
            if (m_bufferHandle.IsValid()) {
                device.DestroyRhiResource(m_bufferHandle);
                m_bufferHandle = {};
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
                                                       nullptr, m_buffer))) {
                return false;
            }
        }

        m_bufferCapacity = m_capacity;

        // Buffers created via CreateRhiBuffer(desc, initialData) are left in CopyDest after the
        // upload copy (mirrors IrradianceProbeGrid::ImportProbeData); transition to the
        // steady-state read state here. Raw-D3D12-fallback buffers are already in that state.
        if (m_bufferCompat != nullptr) {
            CommandAllocator allocator;
            CommandList commandList;
            if (FAILED(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, allocator)) ||
                FAILED(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, commandList))) {
                return false;
            }
            ID3D12GraphicsCommandList* cl = commandList.Get();
            Resource* buf = GetBufferResource();
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                buf->Get(), D3D12_RESOURCE_STATE_COPY_DEST, kReadState);
            cl->ResourceBarrier(1, &barrier);
            if (FAILED(commandList.Close())) return false;

            ID3D12CommandList* lists[] = { commandList.Get() };
            device.GetCommandQueue().ExecuteCommandLists(1u, lists);
            device.WaitForGPU();
        }

        return true;
    }

    Resource* ParticleSystem::GetBufferResource() const
    {
        return m_bufferCompat ? m_bufferCompat : const_cast<Resource*>(&m_buffer);
    }

    Resource* ParticleSystem::GetConstantBufferResource() const
    {
        return m_cbBufferCompat ? m_cbBufferCompat : const_cast<Resource*>(&m_cbBuffer);
    }

    D3D12_GPU_VIRTUAL_ADDRESS ParticleSystem::GetParticleBufferGpuVA() const
    {
        Resource* buf = GetBufferResource();
        return (buf && buf->IsValid()) ? buf->GetGPUVirtualAddress() : 0u;
    }

    // =========================================================================
    // CreatePipeline
    // =========================================================================

    bool ParticleSystem::CreatePipeline(IRHIDevice& device)
    {
        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // Root signature:
        //  [0] Root CBV (b0): ParticleUpdateCB
        //  [1] Root UAV (u0): particle buffer

        D3D12_ROOT_PARAMETER params[2]{};

        params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace  = 0;
        params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[1].Descriptor.ShaderRegister = 0;
        params[1].Descriptor.RegisterSpace  = 0;
        params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 2;
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
        if (!CompileCS(dev, L"Particles/ParticleUpdate_CS.hlsl", "CSMain", cs, compileError)) {
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

    void ParticleSystem::FillUpdateCB(float dt, uint32_t emitCount, ParticleUpdateCBData& out)
    {
        out.deltaTime = dt;
        out.gravity   = m_gravity;
        out.drag      = m_drag;
        out.capacity  = m_capacity;

        out.emitCursor = m_emitCursor;
        out.emitCount  = emitCount;
        out.lifeMin    = m_lifeMin;
        out.lifeMax    = m_lifeMax;

        out.sizeMin     = m_sizeMin;
        out.sizeMax     = m_sizeMax;
        out.emitSpeed   = m_emitSpeed;
        out.emitSpread  = m_emitSpread;

        out.emitOriginX = m_emitOriginX;
        out.emitOriginY = m_emitOriginY;
        out.emitOriginZ = m_emitOriginZ;
        out.emitRadius  = m_emitRadius;

        memcpy(out.colorStart, m_colorStart, sizeof(out.colorStart));
        memcpy(out.colorEnd, m_colorEnd, sizeof(out.colorEnd));

        out.frameSeed = m_frameCounter;
        out.pad1[0] = out.pad1[1] = out.pad1[2] = 0.0f;
    }

    // =========================================================================
    // Update
    // =========================================================================

    bool ParticleSystem::PrepareFrame(IRHIDevice& device)
    {
        if (!m_initialized || !m_pso || !m_enabled) return true;
        if (m_capacity != m_bufferCapacity) {
            return AllocateBuffer(device);
        }
        return true;
    }

    bool ParticleSystem::Update(float dt, IRHIDevice& device, CommandList& cmdList)
    {
        if (!m_initialized || !m_pso || !m_enabled) {
            if (m_initialized && !m_pso && !m_psoMissingLogged) {
                m_psoMissingLogged = true;
                OutputDebugStringA("ParticleSystem::Update: PSO is null (shader compile failed). Particle sim will not proceed.\n");
            }
            return true;
        }

        // Buffer (re)allocation happens in PrepareFrame(), called earlier in the frame
        // before the render graph records any reads of the particle buffer.
        if (m_capacity != m_bufferCapacity) return false;
        if (m_capacity == 0u) return true;

        m_emitAccumulator += m_emissionRate * dt;
        uint32_t emitCount = static_cast<uint32_t>(m_emitAccumulator);
        emitCount = std::min<uint32_t>(emitCount, m_capacity);
        m_emitAccumulator -= static_cast<float>(emitCount);

        if (m_cbMapped) {
            ParticleUpdateCBData cbData{};
            FillUpdateCB(dt, emitCount, cbData);
            memcpy(m_cbMapped, &cbData, sizeof(cbData));
        }

        ID3D12GraphicsCommandList* cl = cmdList.Get();
        if (!cl) return false;

        Resource* buffer  = GetBufferResource();
        Resource* cbBuffer = GetConstantBufferResource();
        if (!buffer || !buffer->IsValid() || !cbBuffer || !cbBuffer->IsValid()) {
            return false;
        }

        // ---- Transition particle buffer: read state → UNORDERED_ACCESS ----
        auto barToUAV = CD3DX12_RESOURCE_BARRIER::Transition(
            buffer->Get(), kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cl->ResourceBarrier(1, &barToUAV);

        // ---- Dispatch ----
        cl->SetPipelineState(m_pso.Get());
        cl->SetComputeRootSignature(m_rootSig.Get());
        cl->SetComputeRootConstantBufferView(0, cbBuffer->GetGPUVirtualAddress());
        cl->SetComputeRootUnorderedAccessView(1, buffer->GetGPUVirtualAddress());

        // [numthreads(64,1,1)] in ParticleUpdate_CS.hlsl
        const uint32_t groupsX = (m_capacity + 63u) / 64u;
#if defined(_DEBUG)
        DebugIncrementDispatchCount();
#endif
        cl->Dispatch(groupsX, 1, 1);

        // ---- UAV barrier (ensure writes are visible) ----
        auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(buffer->Get());
        cl->ResourceBarrier(1, &uavBarrier);

        // ---- Transition back: UNORDERED_ACCESS → read state ----
        auto barToRead = CD3DX12_RESOURCE_BARRIER::Transition(
            buffer->Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState);
        cl->ResourceBarrier(1, &barToRead);

        m_emitCursor = (m_emitCursor + emitCount) % m_capacity;
        ++m_frameCounter;
        return true;
    }

} // namespace SasamiRenderer
