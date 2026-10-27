#define NOMINMAX
#include "Renderer/Fluid/SmokeVolumeSim.h"

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

namespace SasamiEngine::Render
{
    namespace
    {
        // Steady-state resource state for both the state and pressure buffers. Unlike
        // FluidHeightfieldSim's height buffer, nothing outside this class ever samples
        // these as a pixel-shader SRV (the raymarch pass reads GetCurrentStateGpuVA()
        // as a plain non-pixel-shader resource), so a single flag is enough here.
        constexpr D3D12_RESOURCE_STATES kReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        bool CompileCS(ID3D12Device* dev, const wchar_t* relPath, const char* entry, ComPtr<ID3DBlob>& outBlob,
                       std::string& outError)
        {
            const std::string profileNarrow =
                "cs_" + ShaderCompilationService::ResolveEffectiveShaderModel(dev, "6_6");

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
    // Destructor / Release
    // =========================================================================

    SmokeVolumeSim::~SmokeVolumeSim()
    {
        Release();
    }

    void SmokeVolumeSim::Release()
    {
        if (m_cbMapped) {
            m_constantBuffer.Unmap(0, nullptr);
            m_cbMapped = nullptr;
        }
        m_constantBuffer.Reset();
        m_densityStream.Release();
        m_densityStreaming = false;
        if (m_solidMaskMapped) {
            m_solidMask.Unmap(0, nullptr);
            m_solidMaskMapped = nullptr;
        }
        m_solidMask.Reset();
        m_solidMaskWords = 0u;
        m_pending.solidActive = 0u;
        for (int i = 0; i < 2; ++i) {
            m_state[i].Reset();
            m_pressure[i].Reset();
            m_temperature[i].Reset();
        }
        m_bufferCapacity = 0u;
        m_readIndex = 0;
        m_temperatureReadIndex = 0;

        m_rootSig.Reset();
        m_advectPso.Reset();
        m_divergencePso.Reset();
        m_jacobiPso.Reset();
        m_projectPso.Reset();

        m_device = nullptr;
        m_initialized = false;
        m_psoMissingLogged = false;
    }

    // =========================================================================
    // Setters
    // =========================================================================

    void SmokeVolumeSim::SetGridSize(uint32_t countX, uint32_t countY, uint32_t countZ)
    {
        // Clamped per axis so a typo (or a designer fat-fingering a value) cannot
        // request gigabytes of GPU memory for a single volume.
        const uint32_t nx = std::clamp(countX, 1u, kMaxDimension);
        const uint32_t ny = std::clamp(countY, 1u, kMaxDimension);
        const uint32_t nz = std::clamp(countZ, 1u, kMaxDimension);
        if (nx != m_pending.countX || ny != m_pending.countY || nz != m_pending.countZ) {
            // A resize reallocates and clears anyway, but say so explicitly: the cell index is
            // x + y*countX + z*countX*countY, so a changed countX reinterprets every existing
            // cell as a different point in space.
            m_needsClear = true;
            m_pending.solidActive = 0u;
        }
        m_pending.countX = nx;
        m_pending.countY = ny;
        m_pending.countZ = nz;
    }

    void SmokeVolumeSim::SetOrigin(float x, float y, float z)
    {
        // Moving the box leaves the field where it was in grid space, which is fine, but the
        // emitter is specified in world space -- so after a move it lands in a different cell and
        // the old plume is orphaned mid-air. Clearing keeps what is on screen consistent with the
        // parameters that produced it.
        if (x != m_pending.originX || y != m_pending.originY || z != m_pending.originZ) {
            m_needsClear = true;
            m_pending.solidActive = 0u;
        }
        m_pending.originX = x;
        m_pending.originY = y;
        m_pending.originZ = z;
    }

    void SmokeVolumeSim::SetCellSize(float cellSize)
    {
        const float clamped = (cellSize > 0.0001f) ? cellSize : 0.0001f;
        if (clamped != m_pending.cellSize) {
            // The velocity field is stored in world units per second while advection back-traces in
            // cells (velocity * dt / cellSize). Changing the denominator under a field built for the
            // old one makes the trace overshoot the grid, and the emitter and buoyancy terms then
            // push the result to Inf -- after which advection keeps re-sampling its own NaN and the
            // volume never recovers, not even if the old value is restored. Observed exactly that
            // on 2026-09-15: a live cellSize change killed the plume permanently.
            m_needsClear = true;
            m_pending.solidActive = 0u;
        }
        m_pending.cellSize = clamped;
    }

    void SmokeVolumeSim::SetVorticity(float vorticity)
    {
        // The confinement force grows with the vorticity it finds, so a large epsilon is a positive
        // feedback loop: more swirl produces more force produces more swirl. Fedkiw et al. use
        // values around 10 at their grid scales; the cap here leaves room to experiment without
        // letting the field run away between frames.
        m_pending.vorticity = std::clamp(vorticity, 0.0f, 40.0f);
    }

    void SmokeVolumeSim::SetWind(float x, float y, float z, float strength)
    {
        const float lengthSq = x * x + y * y + z * z;
        if (lengthSq > 1e-8f) {
            const float invLength = 1.0f / std::sqrt(lengthSq);
            m_pending.windX = x * invLength;
            m_pending.windY = y * invLength;
            m_pending.windZ = z * invLength;
        } else {
            // A zero direction with a non-zero strength would otherwise be a silent no-op; keep the
            // previous direction so only the strength is being changed.
            m_pending.windX = 0.0f;
            m_pending.windY = 0.0f;
            m_pending.windZ = 0.0f;
        }
        m_pending.windStrength = std::max(strength, 0.0f);
    }

    void SmokeVolumeSim::SetTemperatureParams(float temperatureBuoyancy, float densityWeight,
                                              float emitterTemperature, float coolingRate)
    {
        // Negative values have no physical meaning here (a negative cooling rate would heat the
        // field without bound), so each is floored at zero rather than rejected.
        m_pending.temperatureBuoyancy = std::max(temperatureBuoyancy, 0.0f);
        m_pending.densityWeight       = std::max(densityWeight, 0.0f);
        m_pending.emitterTemperature  = std::max(emitterTemperature, 0.0f);
        m_pending.coolingRate         = std::max(coolingRate, 0.0f);
    }

    void SmokeVolumeSim::SetEmitter(float x, float y, float z, float radius)
    {
        m_pending.emitterX = x;
        m_pending.emitterY = y;
        m_pending.emitterZ = z;
        m_pending.emitterRadius = (radius > 0.0f) ? radius : 0.0f;
    }

    void SmokeVolumeSim::SetJacobiIterations(uint32_t iterations)
    {
        // Cap at a sane upper bound: 0 just skips pressure projection (velocity
        // stays divergent) and there is no benefit to hundreds of relaxation
        // passes per frame even at kMaxDimension^3 cells.
        m_jacobiIterations = std::min(iterations, 256u);
    }

    // =========================================================================
    // Initialize
    // =========================================================================

    bool SmokeVolumeSim::Initialize(GraphicsDevice& device)
    {
        if (m_initialized) return true;
        m_device = &device;

        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // ---- Persistently-mapped upload CB: single 256-byte slot for SmokeUpdateCBData ----
        constexpr UINT64 kCbSize = sizeof(SmokeUpdateCBData);
        if (!ResourceUploadUtility::CreateUploadBuffer(device, kCbSize, m_constantBuffer,
                                                       &m_cbMapped)) {
            return false;
        }
        memset(m_cbMapped, 0, kCbSize);

        // ---- Allocate ping-pong state/pressure/temperature buffers ----
        if (!CreateBuffers(device, GetTotalCellCount())) return false;

        // ---- Compile shaders + create pipelines ----
        if (!CreatePipelines(device)) {
            OutputDebugStringA("SmokeVolumeSim: shader compilation failed — smoke sim will be disabled.\n");
            // Non-fatal: smoke sim disabled but buffers/bindings remain valid (empty volume).
        }

        m_initialized = true;
        return true;
    }

    // =========================================================================
    // CreateBuffers
    // Allocates (or reallocates) the state, pressure and temperature ping-pong pairs,
    // sized for totalCells, and leaves all six in the steady-state read state.
    // =========================================================================

    bool SmokeVolumeSim::CreateBuffers(GraphicsDevice& device, uint32_t totalCells)
    {
        if (totalCells == 0u) return false;

        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // A grid-size change may reallocate mid-run while a previous frame's compute
        // dispatch is still reading/writing the old buffers on the GPU. Flush before
        // destroying them to avoid freeing a resource still in flight.
        if (m_bufferCapacity != 0u) {
            device.WaitForGPU();
        }
        // Sized for the old grid; Update re-creates it for the new one.
        m_densityStream.Release();

        const UINT64 stateBufSize = static_cast<UINT64>(totalCells) * sizeof(StateCell);
        const UINT64 pressureBufSize = static_cast<UINT64>(totalCells) * sizeof(PressureCell);

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC stateDesc{};
        stateDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        stateDesc.Width     = stateBufSize;
        stateDesc.Height    = stateDesc.DepthOrArraySize = stateDesc.MipLevels = 1;
        stateDesc.SampleDesc.Count = 1;
        stateDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        stateDesc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12_RESOURCE_DESC pressureDesc = stateDesc;
        pressureDesc.Width = pressureBufSize;

        D3D12_RESOURCE_DESC temperatureDesc = stateDesc;
        temperatureDesc.Width = static_cast<UINT64>(totalCells) * sizeof(float);

        for (int i = 0; i < 2; ++i) {
            m_state[i].Reset();
            m_pressure[i].Reset();
            m_temperature[i].Reset();

            // Raw default-heap buffers created directly in the steady-state read
            // state, same as FluidHeightfieldSim's/ParticleSystem's raw-D3D12
            // fallback path. Content is undefined at creation; the first Advect
            // dispatch overwrites every state cell before anything reads it as a
            // "previous" state, and Divergence does the same for pressure.
            if (FAILED(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &stateDesc,
                                                       kReadState, nullptr, m_state[i]))) {
                return false;
            }
            if (FAILED(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &pressureDesc,
                                                       kReadState, nullptr, m_pressure[i]))) {
                return false;
            }
            if (FAILED(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &temperatureDesc,
                                                       kReadState, nullptr, m_temperature[i]))) {
                return false;
            }
        }

        if (!CreateSolidMaskBuffer(device, totalCells)) {
            return false;
        }

        m_bufferCapacity = totalCells;
        m_needsClear = true;
        m_readIndex = 0;
        m_temperatureReadIndex = 0;
        return true;
    }

    // The mask is CPU-written rarely (a debug command after voxel.scene), so an upload-heap
    // buffer read directly by the shader is enough; there is no per-frame copy. It always has at
    // least one word so t2 is bound to a valid address even with no obstacles.
    bool SmokeVolumeSim::CreateSolidMaskBuffer(GraphicsDevice& device, uint32_t totalCells)
    {
        if (m_solidMaskMapped) {
            m_solidMask.Unmap(0, nullptr);
            m_solidMaskMapped = nullptr;
        }
        m_solidMask.Reset();
        m_solidMaskWords = 0u;
        m_pending.solidActive = 0u;

        const size_t words = (std::max)(static_cast<size_t>((totalCells + 31u) / 32u), static_cast<size_t>(1u));
        void* mapped = nullptr;
        if (!ResourceUploadUtility::CreateUploadBuffer(device, words * sizeof(uint32_t), m_solidMask, &mapped) || !mapped) {
            return false;
        }
        m_solidMaskMapped = static_cast<uint32_t*>(mapped);
        m_solidMaskWords = words;
        memset(m_solidMaskMapped, 0, words * sizeof(uint32_t));
        return true;
    }

    bool SmokeVolumeSim::SetSolidMask(const uint32_t* words, size_t wordCount, bool clearField)
    {
        const uint32_t totalCells = GetTotalCellCount();
        if (!m_initialized || !m_device || !m_solidMaskMapped || !words ||
            totalCells != m_bufferCapacity || wordCount != (totalCells + 31u) / 32u ||
            wordCount > m_solidMaskWords) {
            return false;
        }
        // Frames in flight may still be reading the old bits from this upload buffer.
        m_device->WaitForGPU();
        memcpy(m_solidMaskMapped, words, wordCount * sizeof(uint32_t));
        m_pending.solidActive = 1u;
        // Smoke already inside the new walls would otherwise be zeroed in one step and leave a
        // visible pop; start from an empty field so the A/B compares like with like. A caller
        // refreshing walls that moved (voxel.update) passes clearField=false to keep the smoke.
        if (clearField) {
            m_needsClear = true;
        }
        return true;
    }

    bool SmokeVolumeSim::ReadbackDensity(std::vector<float>& outDensity)
    {
        outDensity.clear();
        const uint32_t totalCells = GetTotalCellCount();
        Resource* state = GetStateResource(m_readIndex);
        if (!m_initialized || !m_device || totalCells == 0u || totalCells != m_bufferCapacity || !state->IsValid()) {
            return false;
        }
        GraphicsDevice& device = *m_device;
        const UINT64 bytes = static_cast<UINT64>(totalCells) * sizeof(StateCell);

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Resource readback;
        if (FAILED(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, readback))) {
            return false;
        }

        CommandAllocator allocator;
        CommandList commandList;
        if (FAILED(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, allocator)) ||
            FAILED(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, commandList))) {
            return false;
        }
        // Frames in flight may still be writing the state buffer.
        device.WaitForGPU();
        ID3D12GraphicsCommandList* cl = commandList.Get();
        auto toCopy = CD3DX12_RESOURCE_BARRIER::Transition(state->Get(), kReadState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->ResourceBarrier(1, &toCopy);
        cl->CopyBufferRegion(readback.Get(), 0, state->Get(), 0, bytes);
        auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(state->Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kReadState);
        cl->ResourceBarrier(1, &toRead);
        if (FAILED(commandList.Close())) {
            return false;
        }
        ID3D12CommandList* lists[] = { commandList.Get() };
        device.GetCommandQueue().ExecuteCommandLists(1u, lists);
        device.WaitForGPU();

        void* mapped = nullptr;
        const D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(bytes) };
        if (FAILED(readback->Map(0, &readRange, &mapped)) || !mapped) {
            return false;
        }
        const StateCell* cells = static_cast<const StateCell*>(mapped);
        outDensity.resize(totalCells);
        for (uint32_t i = 0; i < totalCells; ++i) {
            outDensity[i] = cells[i].density;
        }
        const D3D12_RANGE noWrite{ 0, 0 };
        readback->Unmap(0, &noWrite);
        return true;
    }

    void SmokeVolumeSim::SetDensityStreaming(bool enabled)
    {
        if (!enabled && m_densityStream.IsReady()) {
            // Frames in flight may still copy into the ring.
            if (m_device) {
                m_device->WaitForGPU();
            }
            m_densityStream.Release();
        }
        m_densityStreaming = enabled;
    }

    bool SmokeVolumeSim::ReadStreamedDensity(std::vector<float>& outDensity, uint64_t& outRecord) const
    {
        const uint32_t totalCells = GetTotalCellCount();
        if (!m_densityStreaming || totalCells == 0u ||
            m_densityStream.Bytes() != static_cast<uint64_t>(totalCells) * sizeof(StateCell)) {
            return false;
        }
        const void* data = m_densityStream.Latest(outRecord);
        if (!data) {
            return false;
        }
        const StateCell* cells = static_cast<const StateCell*>(data);
        outDensity.resize(totalCells);
        for (uint32_t i = 0; i < totalCells; ++i) {
            outDensity[i] = cells[i].density;
        }
        return true;
    }

    void SmokeVolumeSim::ClearSolidMask()
    {
        if (m_pending.solidActive != 0u) {
            m_needsClear = true;
        }
        m_pending.solidActive = 0u;
    }

    Resource* SmokeVolumeSim::GetStateResource(int index)
    {
        return &m_state[index];
    }

    Resource* SmokeVolumeSim::GetPressureResource(int index)
    {
        return &m_pressure[index];
    }

    Resource* SmokeVolumeSim::GetTemperatureResource(int index)
    {
        return &m_temperature[index];
    }

    D3D12_GPU_VIRTUAL_ADDRESS SmokeVolumeSim::GetCurrentStateGpuVA() const
    {
        const Resource& buf = m_state[m_readIndex];
        return buf.IsValid() ? buf.GetGPUVirtualAddress() : 0u;
    }

    D3D12_GPU_VIRTUAL_ADDRESS SmokeVolumeSim::GetUpdateCBGpuVA() const
    {
        return m_constantBuffer.IsValid() ? m_constantBuffer.GetGPUVirtualAddress() : 0u;
    }

    uint32_t SmokeVolumeSim::GetTotalCellCount() const
    {
        return m_pending.countX * m_pending.countY * m_pending.countZ;
    }

    // =========================================================================
    // CreatePipelines
    // =========================================================================

    bool SmokeVolumeSim::CreatePipelines(GraphicsDevice& device)
    {
        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // Root signature shared by all four stages:
        //  [0] Root CBV (b0): SmokeUpdateCB
        //  [1] Root SRV (t0): state in
        //  [2] Root SRV (t1): pressure in
        //  [3] Root UAV (u0): state out
        //  [4] Root UAV (u1): pressure out
        //  [5] Root SRV (t2): solid mask, 1 bit per cell (StructuredBuffer<uint>)
        //  [6] Root SRV (t3): temperature in (StructuredBuffer<float>)
        //  [7] Root UAV (u2): temperature out (RWStructuredBuffer<float>)
        // Not every stage's compiled shader statically accesses all of them (e.g.
        // AdvectMain never touches t1/u1) — D3D12 allows a PSO's shader to use a
        // strict subset of the root signature it is created with, so the same
        // signature serves Advect/Divergence/Jacobi/Project without four
        // separate signatures, matching SmokeVolume_CS.hlsl's binding comment.
        D3D12_ROOT_PARAMETER params[8]{};

        params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace  = 0;
        params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1].Descriptor.ShaderRegister = 0;
        params[1].Descriptor.RegisterSpace  = 0;
        params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor.ShaderRegister = 1;
        params[2].Descriptor.RegisterSpace  = 0;
        params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[3].Descriptor.ShaderRegister = 0;
        params[3].Descriptor.RegisterSpace  = 0;
        params[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[4].Descriptor.ShaderRegister = 1;
        params[4].Descriptor.RegisterSpace  = 0;
        params[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[5].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[5].Descriptor.ShaderRegister = 2;
        params[5].Descriptor.RegisterSpace  = 0;
        params[5].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[6].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[6].Descriptor.ShaderRegister = 3;
        params[6].Descriptor.RegisterSpace  = 0;
        params[6].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[7].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[7].Descriptor.ShaderRegister = 2;
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

        struct StageEntry
        {
            const char* entry;
            ComPtr<ID3D12PipelineState>* pso;
        };
        const StageEntry stages[] = {
            { "AdvectMain",     &m_advectPso },
            { "ClearMain",      &m_clearPso },
            { "DivergenceMain", &m_divergencePso },
            { "JacobiMain",     &m_jacobiPso },
            { "ProjectMain",    &m_projectPso },
        };

        for (const StageEntry& stage : stages) {
            ComPtr<ID3DBlob> cs;
            std::string compileError;
            if (!CompileCS(dev, L"Compute/Fluid/SmokeVolume_CS.hlsl", stage.entry, cs, compileError)) {
                std::string msg = "SmokeVolumeSim: shader compile failed for ";
                msg += stage.entry;
                msg += ": ";
                msg += compileError;
                msg += "\n";
                OutputDebugStringA(msg.c_str());
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = m_rootSig.Get();
            pso.CS             = { cs->GetBufferPointer(), cs->GetBufferSize() };
            if (FAILED(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(stage.pso->ReleaseAndGetAddressOf())))) {
                return false;
            }
        }

        return true;
    }

    // =========================================================================
    // FillUpdateCB
    // m_pending already mirrors SmokeUpdateCBData field-for-field (it is the CB
    // payload plus the sim's current configuration), so filling the CB is just a
    // copy with dt patched in — unlike FluidHeightfieldSim, which keeps its grid
    // parameters in separate members and assembles the CB from them by hand.
    // =========================================================================

    void SmokeVolumeSim::FillUpdateCB(float dt, SmokeUpdateCBData& out) const
    {
        out = m_pending;
        out.dt = dt;
    }

    // =========================================================================
    // PrepareFrame
    // =========================================================================

    bool SmokeVolumeSim::PrepareFrame(GraphicsDevice& device)
    {
        const bool pipelinesReady = m_clearPso && m_advectPso && m_divergencePso && m_jacobiPso && m_projectPso;
        if (!m_initialized || !pipelinesReady || !m_enabled) return true;
        if (GetTotalCellCount() != m_bufferCapacity) {
            return CreateBuffers(device, GetTotalCellCount());
        }
        return true;
    }

    // =========================================================================
    // Update
    // =========================================================================

    bool SmokeVolumeSim::Update(CommandList& cmdList, float dt)
    {
        const bool pipelinesReady = m_advectPso && m_divergencePso && m_jacobiPso && m_projectPso;
        if (!m_initialized || !pipelinesReady || !m_enabled) {
            if (m_initialized && !pipelinesReady && !m_psoMissingLogged) {
                m_psoMissingLogged = true;
                OutputDebugStringA("SmokeVolumeSim::Update: one or more PSOs are null (shader compile failed). "
                                   "Smoke sim will not proceed.\n");
            }
            return true;
        }

        // Buffer (re)allocation happens in PrepareFrame(), called earlier in the frame
        // before the render graph records any reads of the ping-pong buffers.
        const uint32_t totalCells = GetTotalCellCount();
        if (totalCells != m_bufferCapacity) return false;
        if (totalCells == 0) return true;

        // A hitch (alt-tab, shader recompile, a breakpoint) can hand this function a
        // huge dt. Semi-Lagrangian advection is unconditionally stable for any dt in
        // theory, but the explicit Jacobi pressure solve and the emitter/dissipation
        // terms are not, so an unbounded dt can still blow the field up into NaNs.
        // Clamp to a worst-case 30fps step.
        constexpr float kMaxDt = 1.0f / 30.0f;
        dt = std::clamp(dt, 0.0f, kMaxDt);

        ID3D12GraphicsCommandList* cl = cmdList.Get();
        if (!cl) return false;

        Resource* stateRead  = GetStateResource(m_readIndex);
        Resource* stateWrite = GetStateResource(1 - m_readIndex);
        Resource* pressureA  = GetPressureResource(0);
        Resource* pressureB  = GetPressureResource(1);
        Resource* temperatureRead  = GetTemperatureResource(m_temperatureReadIndex);
        Resource* temperatureWrite = GetTemperatureResource(1 - m_temperatureReadIndex);
        if (!temperatureRead->IsValid() || !temperatureWrite->IsValid() ||
            !stateRead->IsValid() || !stateWrite->IsValid() ||
            !pressureA->IsValid() || !pressureB->IsValid() || !m_constantBuffer.IsValid() || !m_solidMask.IsValid()) {
            return false;
        }

        if (m_cbMapped) {
            SmokeUpdateCBData cbData{};
            FillUpdateCB(dt, cbData);
            memcpy(m_cbMapped, &cbData, sizeof(cbData));
        }

        const D3D12_GPU_VIRTUAL_ADDRESS cbVA = m_constantBuffer.GetGPUVirtualAddress();
        // [numthreads(8,8,8)] in SmokeVolume_CS.hlsl
        const uint32_t groupsX = (m_pending.countX + 7u) / 8u;
        const uint32_t groupsY = (m_pending.countY + 7u) / 8u;
        const uint32_t groupsZ = (m_pending.countZ + 7u) / 8u;

        cl->SetComputeRootSignature(m_rootSig.Get());
        // Root descriptors persist across SetPipelineState with the same root signature, so the
        // mask is bound once for every stage below.
        cl->SetComputeRootShaderResourceView(5, m_solidMask.GetGPUVirtualAddress());

        // ---- Stage 0: clear, once per allocation ----
        // CreateCommittedResource leaves the contents undefined, and advection samples its own
        // previous output every step, so an uninitialised float does not wash out -- it becomes a
        // NaN that spreads through the field and never leaves. Both slots of both pairs are
        // cleared, since which one is "read" alternates.
        if (m_needsClear) {
            for (int i = 0; i < 2; ++i) {
                Resource* state = GetStateResource(i);
                Resource* pressure = GetPressureResource(i);
                Resource* temperature = GetTemperatureResource(i);
                if (!state || !pressure || !temperature) continue;

                D3D12_RESOURCE_BARRIER toUAV[] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(state->Get(), kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    CD3DX12_RESOURCE_BARRIER::Transition(pressure->Get(), kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    CD3DX12_RESOURCE_BARRIER::Transition(temperature->Get(), kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                };
                cl->ResourceBarrier(_countof(toUAV), toUAV);

                cl->SetPipelineState(m_clearPso.Get());
                cl->SetComputeRootConstantBufferView(0, cbVA);
                cl->SetComputeRootUnorderedAccessView(3, state->GetGPUVirtualAddress());
                cl->SetComputeRootUnorderedAccessView(4, pressure->GetGPUVirtualAddress());
                cl->SetComputeRootUnorderedAccessView(7, temperature->GetGPUVirtualAddress());
#if defined(_DEBUG)
                DebugIncrementDispatchCount();
#endif
                cl->Dispatch(groupsX, groupsY, groupsZ);

                D3D12_RESOURCE_BARRIER toRead[] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(state->Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState),
                    CD3DX12_RESOURCE_BARRIER::Transition(pressure->Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState),
                    CD3DX12_RESOURCE_BARRIER::Transition(temperature->Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState),
                };
                cl->ResourceBarrier(_countof(toRead), toRead);
            }
            m_needsClear = false;
        }

        // ---- Stage 1: Advect (state[read] -> state[write]) ----
        {
            D3D12_RESOURCE_BARRIER toUAV[] = {
                CD3DX12_RESOURCE_BARRIER::Transition(stateWrite->Get(), kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                CD3DX12_RESOURCE_BARRIER::Transition(temperatureWrite->Get(), kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            cl->ResourceBarrier(_countof(toUAV), toUAV);

            cl->SetPipelineState(m_advectPso.Get());
            cl->SetComputeRootConstantBufferView(0, cbVA);
            cl->SetComputeRootShaderResourceView(1, stateRead->GetGPUVirtualAddress());
            cl->SetComputeRootUnorderedAccessView(3, stateWrite->GetGPUVirtualAddress());
            cl->SetComputeRootShaderResourceView(6, temperatureRead->GetGPUVirtualAddress());
            cl->SetComputeRootUnorderedAccessView(7, temperatureWrite->GetGPUVirtualAddress());
#if defined(_DEBUG)
            DebugIncrementDispatchCount();
#endif
            cl->Dispatch(groupsX, groupsY, groupsZ);

            // UAV barrier makes the Advect writes visible; the following transition
            // barrier hands state[write] over to the SRV state Divergence reads it in.
            D3D12_RESOURCE_BARRIER uav[] = {
                CD3DX12_RESOURCE_BARRIER::UAV(stateWrite->Get()),
                CD3DX12_RESOURCE_BARRIER::UAV(temperatureWrite->Get()),
            };
            cl->ResourceBarrier(_countof(uav), uav);
            D3D12_RESOURCE_BARRIER toRead[] = {
                CD3DX12_RESOURCE_BARRIER::Transition(stateWrite->Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState),
                CD3DX12_RESOURCE_BARRIER::Transition(temperatureWrite->Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState),
            };
            cl->ResourceBarrier(_countof(toRead), toRead);
        }

        // Temperature is not projected, so unlike the state pair (whose result lands back in
        // state[read]) it ping-pongs every step: this step's write becomes next step's read.
        m_temperatureReadIndex = 1 - m_temperatureReadIndex;

        // ---- Stage 2: Divergence (state[write] -> pressure[0]) ----
        {
            auto toUAV = CD3DX12_RESOURCE_BARRIER::Transition(
                pressureA->Get(), kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl->ResourceBarrier(1, &toUAV);

            cl->SetPipelineState(m_divergencePso.Get());
            cl->SetComputeRootConstantBufferView(0, cbVA);
            cl->SetComputeRootShaderResourceView(1, stateWrite->GetGPUVirtualAddress());
            cl->SetComputeRootUnorderedAccessView(4, pressureA->GetGPUVirtualAddress());
#if defined(_DEBUG)
            DebugIncrementDispatchCount();
#endif
            cl->Dispatch(groupsX, groupsY, groupsZ);

            auto uav = CD3DX12_RESOURCE_BARRIER::UAV(pressureA->Get());
            cl->ResourceBarrier(1, &uav);
            auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(
                pressureA->Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState);
            cl->ResourceBarrier(1, &toRead);
        }

        // ---- Stage 3: Jacobi, ping-ponging pressure[0] <-> pressure[1] ----
        // pressureReadIdx tracks whichever buffer holds the most recently written
        // (divergence, pressure) pair; it starts at 0 because Divergence always
        // writes there. After the loop it names the buffer holding the converged
        // result, whether m_jacobiIterations is even, odd, or zero.
        int pressureReadIdx = 0;
        for (uint32_t iter = 0; iter < m_jacobiIterations; ++iter) {
            Resource* pIn  = GetPressureResource(pressureReadIdx);
            Resource* pOut = GetPressureResource(1 - pressureReadIdx);

            auto toUAV = CD3DX12_RESOURCE_BARRIER::Transition(
                pOut->Get(), kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl->ResourceBarrier(1, &toUAV);

            cl->SetPipelineState(m_jacobiPso.Get());
            cl->SetComputeRootConstantBufferView(0, cbVA);
            cl->SetComputeRootShaderResourceView(2, pIn->GetGPUVirtualAddress());
            cl->SetComputeRootUnorderedAccessView(4, pOut->GetGPUVirtualAddress());
#if defined(_DEBUG)
            DebugIncrementDispatchCount();
#endif
            cl->Dispatch(groupsX, groupsY, groupsZ);

            // Every iteration reads what the previous one wrote, so both the UAV
            // barrier and the state flip back to a readable state are required
            // here, not just once after the whole loop.
            auto uav = CD3DX12_RESOURCE_BARRIER::UAV(pOut->Get());
            cl->ResourceBarrier(1, &uav);
            auto toRead = CD3DX12_RESOURCE_BARRIER::Transition(
                pOut->Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState);
            cl->ResourceBarrier(1, &toRead);

            pressureReadIdx = 1 - pressureReadIdx;
        }
        Resource* finalPressure = GetPressureResource(pressureReadIdx);

        // ---- Stage 4: Project (state[write] + final pressure -> state[read]) ----
        // The finished, divergence-free field is written back into state[read]
        // (m_readIndex is never flipped) rather than into state[write]: Advect
        // already consumed everything state[read] held for this frame, so it is
        // free to become this frame's output slot, while state[write] keeps its
        // fixed role as the advected-but-not-yet-projected scratch buffer every
        // frame. GetCurrentStateGpuVA() therefore always returns m_state[m_readIndex]
        // and needs no bookkeeping beyond leaving it in the steady-state read state.
        {
            auto toUAV = CD3DX12_RESOURCE_BARRIER::Transition(
                stateRead->Get(), kReadState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl->ResourceBarrier(1, &toUAV);

            cl->SetPipelineState(m_projectPso.Get());
            cl->SetComputeRootConstantBufferView(0, cbVA);
            cl->SetComputeRootShaderResourceView(1, stateWrite->GetGPUVirtualAddress());
            cl->SetComputeRootShaderResourceView(2, finalPressure->GetGPUVirtualAddress());
            cl->SetComputeRootUnorderedAccessView(3, stateRead->GetGPUVirtualAddress());
#if defined(_DEBUG)
            DebugIncrementDispatchCount();
#endif
            cl->Dispatch(groupsX, groupsY, groupsZ);

            auto uav = CD3DX12_RESOURCE_BARRIER::UAV(stateRead->Get());
            cl->ResourceBarrier(1, &uav);

            // Final resting state required by GetCurrentStateGpuVA()'s contract: the
            // raymarch pass reads this buffer as a non-pixel-shader resource this frame.
            auto toFinal = CD3DX12_RESOURCE_BARRIER::Transition(
                stateRead->Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState);
            cl->ResourceBarrier(1, &toFinal);
        }

        if (m_densityStreaming && m_device &&
            m_densityStream.Ensure(*m_device, static_cast<uint64_t>(totalCells) * sizeof(StateCell))) {
            m_densityStream.Record(cl, stateRead->Get(), kReadState);
        }

        return true;
    }

} // namespace SasamiEngine::Render
