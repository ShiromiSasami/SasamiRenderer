#define NOMINMAX
#include "Renderer/VirtualGeometry/ClusterGpuCulling.h"
#include <algorithm>

#include <cstring>
#include <string>

#include "d3dx12.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Resources/ShaderCompilationService.h"

namespace
{
    // C++ mirror of ClusterSelectCB (Shaders/Raster/Geometry/MeshShader/ClusterSelect_CS.hlsl),
    // bound as a 128-byte root CBV at b0. The trailing pad brings the struct from 100 bytes of
    // named fields up to the full 128 bytes the shader's cbuffer declares.
    struct ClusterSelectCBData
    {
        float viewProj[16] = {};
        float cameraPos[3] = {};
        float lodThresholdPx = 0.0f;
        float projScale = 0.0f;
        float zNear = 0.0f;
        float orthographic = 0.0f;
        uint32_t instanceCount = 0;
        uint32_t maxVisible = 0;
        uint32_t collectStats = 0;  // g_collectStats: gates the diagnostic counters
        uint32_t pad[6] = {};
    };
    static_assert(sizeof(ClusterSelectCBData) == 128, "ClusterSelectCBData must match the 128-byte ClusterSelectCB root CBV layout.");

    constexpr wchar_t kClusterSelectShaderPath[] = L"Raster/Geometry/MeshShader/ClusterSelect_CS.hlsl";
    // Mirror of kSelectGroupsX in Shaders/Raster/Geometry/MeshShader/MeshShader_Common.hlsli.
    // CS_ClusterSelect runs one thread group per instance and rebuilds its flat instance index as
    // groupId.y * kSelectGroupsX + groupId.x, so this value and the shader's MUST stay equal: if
    // they diverge nothing fails loudly, the selection just reads the wrong instances.
    constexpr uint32_t kSelectGroupsX = 1024u;

    constexpr uint32_t kVisibleBufferStride = 8u;   // RWStructuredBuffer<uint2>
    constexpr uint32_t kCounterBufferSize   = 32u;  // [0] count, [4] overflow, [8] instances processed, [12] nodes visited, [16] max traversal levels, [20] clusters examined, [24] pre-clamp requested count
    // Byte offset of the pre-clamp visible-cluster demand inside the counter buffer, written by
    // CS_ClusterSelectArgs before it clamps byte 0. This is what the visible list is sized from, so
    // it must match the Store offset in ClusterSelect_CS.hlsl.
    constexpr uint32_t kRequestedCountOffset = 24u;
    constexpr uint32_t kArgsBufferSize      = 16u;  // 12-byte DispatchMesh args, rounded up
    constexpr uint32_t kArgsBytesToReset    = 12u;
    constexpr uint32_t kCbBufferSize        = 256u;
    constexpr uint32_t kWantedBufferSize    = SasamiEngine::Render::ClusterGpuCulling::kWantedWordCount * 4u;  // 128 KB
    constexpr uint32_t kReadbackBufferSize  = kCounterBufferSize + kWantedBufferSize;
    constexpr uint32_t kZeroBufferSize      = kWantedBufferSize;  // reset source, large enough for the counter/args resets too

    double VisibleMegabytesFor(uint32_t entries)
    {
        return static_cast<double>(static_cast<uint64_t>(entries) * kVisibleBufferStride) / (1024.0 * 1024.0);
    }
}

namespace SasamiEngine::Render
{
    ClusterGpuCulling::~ClusterGpuCulling()
    {
        Release();
    }

    bool ClusterGpuCulling::Initialize(GraphicsDevice& device, uint32_t frameCount)
    {
        Release();

        if (frameCount == 0) {
            DebugLog("ClusterGpuCulling::Initialize: frameCount must be greater than zero.\n");
            return false;
        }

        m_device = &device;
        m_frameCount = frameCount;

        if (!CreateZeroBuffer()) {
            DebugLog("ClusterGpuCulling::Initialize: failed to create the reset zero-buffer.\n");
            Release();
            return false;
        }

        if (!CreateRootSignatureAndPsos()) {
            DebugLog("ClusterGpuCulling::Initialize: failed to create the compute root signature/PSOs.\n");
            Release();
            return false;
        }

        if (!CreateCommandSignature()) {
            DebugLog("ClusterGpuCulling::Initialize: failed to create the DispatchMesh command signature.\n");
            Release();
            return false;
        }

        m_slots.resize(static_cast<size_t>(frameCount) * kViewSlotsPerFrame);
        for (ViewSlotResources& slot : m_slots) {
            if (!CreateSlotResources(slot)) {
                DebugLog("ClusterGpuCulling::Initialize: failed to create per-view-slot resources.\n");
                Release();
                return false;
            }
        }

        m_valid = true;
        return true;
    }

    void ClusterGpuCulling::Release()
    {
        for (ViewSlotResources& slot : m_slots) {
            ReleaseSlot(slot);
        }
        m_slots.clear();

        if (m_device && m_zeroBufferHandle.IsValid()) {
            m_device->DestroyRhiResource(m_zeroBufferHandle);
        }
        m_zeroBufferHandle = {};
        m_zeroBufferCompat = nullptr;

        m_commandSignature.Reset();
        m_selectPso.Reset();
        m_argsPso.Reset();
        m_rootSignature.Reset();

        m_frameCount = 0;
        m_valid = false;
        m_device = nullptr;
    }

    bool ClusterGpuCulling::IsValid() const
    {
        return m_valid;
    }

    bool ClusterGpuCulling::CreateZeroBuffer()
    {
        if (!m_device) return false;

        const std::vector<uint8_t> zeroData(kZeroBufferSize, 0u);

        RhiBufferDesc desc{};
        desc.sizeInBytes = kZeroBufferSize;
        desc.strideInBytes = 4u;
        desc.usage = RhiBufferUsageFlags::CopySource;
        desc.memoryUsage = RhiMemoryUsage::CpuToGpu;
        desc.initialState = RhiResourceState::CopySource;

        m_zeroBufferHandle = m_device->CreateRhiBuffer(desc, zeroData.data());
        m_zeroBufferCompat = m_device->GetD3D12CompatibilityResource(m_zeroBufferHandle);
        return m_zeroBufferCompat != nullptr && m_zeroBufferCompat->IsValid();
    }

    bool ClusterGpuCulling::CreateRootSignatureAndPsos()
    {
        if (!m_device) return false;
        ID3D12Device* dev = m_device->GetDevice();
        if (!dev) return false;

        // Root signature:
        //  [0] Root CBV (b0): ClusterSelectCB
        //  [1] Root SRV (t0): clusters
        //  [2] Root SRV (t1): groups
        //  [3] Root SRV (t2): nodes
        //  [4] Root SRV (t3): instances
        //  [5] Root UAV (u0): visible list
        //  [6] Root UAV (u1): counter
        //  [7] Root UAV (u2): args
        //  [8] Root UAV (u3): wanted group bitset
        //  [9] Root SRV (t4): page offset table (uint per group; page byte offset in the pool, 0xFFFFFFFF = not resident)
        //  [10] Root SRV (t5): page pool
        D3D12_ROOT_PARAMETER params[11]{};

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

        params[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[3].Descriptor.ShaderRegister = 2;
        params[3].Descriptor.RegisterSpace  = 0;
        params[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[4].Descriptor.ShaderRegister = 3;
        params[4].Descriptor.RegisterSpace  = 0;
        params[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[5].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[5].Descriptor.ShaderRegister = 0;
        params[5].Descriptor.RegisterSpace  = 0;
        params[5].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[6].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[6].Descriptor.ShaderRegister = 1;
        params[6].Descriptor.RegisterSpace  = 0;
        params[6].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[7].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[7].Descriptor.ShaderRegister = 2;
        params[7].Descriptor.RegisterSpace  = 0;
        params[7].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[8].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[8].Descriptor.ShaderRegister = 3;
        params[8].Descriptor.RegisterSpace  = 0;
        params[8].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[9].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[9].Descriptor.ShaderRegister = 4;
        params[9].Descriptor.RegisterSpace  = 0;
        params[9].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[10].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[10].Descriptor.ShaderRegister = 5;
        params[10].Descriptor.RegisterSpace  = 0;
        params[10].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 11;
        rsDesc.pParameters   = params;
        rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> rsBlob, rsError;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                               rsBlob.ReleaseAndGetAddressOf(),
                                               rsError.ReleaseAndGetAddressOf()))) {
            if (rsError && rsError->GetBufferSize() > 0) {
                DebugLog(static_cast<const char*>(rsError->GetBufferPointer()));
            }
            DebugLog("ClusterGpuCulling: root signature serialize failed.\n");
            return false;
        }
        if (FAILED(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                            IID_PPV_ARGS(m_rootSignature.ReleaseAndGetAddressOf())))) {
            DebugLog("ClusterGpuCulling: CreateRootSignature failed.\n");
            return false;
        }

        const std::string profile = "cs_" + ShaderCompilationService::ResolveEffectiveShaderModel(dev, "6_6");

        std::vector<uint8_t> selectBytecode;
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc(
                "ClusterSelect", kClusterSelectShaderPath, "CS_ClusterSelect", profile.c_str(), selectBytecode)) {
            DebugLog("ClusterGpuCulling: failed to compile CS_ClusterSelect.\n");
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC selectPsoDesc{};
        selectPsoDesc.pRootSignature = m_rootSignature.Get();
        selectPsoDesc.CS             = { selectBytecode.data(), selectBytecode.size() };
        if (FAILED(dev->CreateComputePipelineState(&selectPsoDesc, IID_PPV_ARGS(m_selectPso.ReleaseAndGetAddressOf())))) {
            DebugLog("ClusterGpuCulling: CreateComputePipelineState (CS_ClusterSelect) failed.\n");
            return false;
        }

        std::vector<uint8_t> argsBytecode;
        if (!ShaderCompilationService::GetOrCompileShaderBytecodeDxc(
                "ClusterSelect", kClusterSelectShaderPath, "CS_ClusterSelectArgs", profile.c_str(), argsBytecode)) {
            DebugLog("ClusterGpuCulling: failed to compile CS_ClusterSelectArgs.\n");
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC argsPsoDesc{};
        argsPsoDesc.pRootSignature = m_rootSignature.Get();
        argsPsoDesc.CS             = { argsBytecode.data(), argsBytecode.size() };
        if (FAILED(dev->CreateComputePipelineState(&argsPsoDesc, IID_PPV_ARGS(m_argsPso.ReleaseAndGetAddressOf())))) {
            DebugLog("ClusterGpuCulling: CreateComputePipelineState (CS_ClusterSelectArgs) failed.\n");
            return false;
        }

        return true;
    }

    bool ClusterGpuCulling::CreateCommandSignature()
    {
        if (!m_device) return false;
        ID3D12Device* dev = m_device->GetDevice();
        if (!dev) return false;

        D3D12_INDIRECT_ARGUMENT_DESC argDesc{};
        argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;

        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride       = sizeof(D3D12_DISPATCH_MESH_ARGUMENTS);
        desc.NumArgumentDescs = 1;
        desc.pArgumentDescs   = &argDesc;

        if (FAILED(dev->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(m_commandSignature.ReleaseAndGetAddressOf())))) {
            DebugLog("ClusterGpuCulling: CreateCommandSignature (DispatchMesh) failed.\n");
            return false;
        }
        return true;
    }

    bool ClusterGpuCulling::CreateSlotResources(ViewSlotResources& slot)
    {
        if (!m_device) return false;

        if (!CreateVisibleBuffer(kInitialVisibleClusters, slot.visibleHandle, slot.visibleCompat)) {
            return false;
        }
        slot.visibleState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        slot.visibleCapacity = kInitialVisibleClusters;
        slot.recordedVisibleCapacity = kInitialVisibleClusters;

        RhiBufferDesc counterDesc{};
        counterDesc.sizeInBytes = kCounterBufferSize;
        counterDesc.strideInBytes = 4u;
        counterDesc.usage = RhiBufferUsageFlags::UnorderedAccess | RhiBufferUsageFlags::CopyDest;
        counterDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
        counterDesc.initialState = RhiResourceState::UnorderedAccess;
        slot.counterHandle = m_device->CreateRhiBuffer(counterDesc);
        slot.counterCompat = m_device->GetD3D12CompatibilityResource(slot.counterHandle);
        slot.counterState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        RhiBufferDesc argsDesc{};
        argsDesc.sizeInBytes = kArgsBufferSize;
        argsDesc.strideInBytes = 4u;
        argsDesc.usage = RhiBufferUsageFlags::UnorderedAccess | RhiBufferUsageFlags::CopyDest;
        argsDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
        argsDesc.initialState = RhiResourceState::UnorderedAccess;
        slot.argsHandle = m_device->CreateRhiBuffer(argsDesc);
        slot.argsCompat = m_device->GetD3D12CompatibilityResource(slot.argsHandle);
        slot.argsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        RhiBufferDesc cbDesc{};
        cbDesc.sizeInBytes = kCbBufferSize;
        cbDesc.strideInBytes = kCbBufferSize;
        cbDesc.usage = RhiBufferUsageFlags::Constant;
        cbDesc.memoryUsage = RhiMemoryUsage::CpuToGpu;
        cbDesc.initialState = RhiResourceState::ShaderResource;
        slot.cbHandle = m_device->CreateRhiBuffer(cbDesc);
        slot.cbCompat = m_device->GetD3D12CompatibilityResource(slot.cbHandle);
        if (slot.cbCompat && slot.cbCompat->IsValid()) {
            const D3D12_RANGE emptyRange{ 0, 0 };
            if (SUCCEEDED(slot.cbCompat->Map(0, &emptyRange, &slot.cbMapped)) && slot.cbMapped) {
                std::memset(slot.cbMapped, 0, kCbBufferSize);
            }
        }

        RhiBufferDesc wantedDesc{};
        wantedDesc.sizeInBytes = kWantedBufferSize;
        wantedDesc.strideInBytes = 4u;
        wantedDesc.usage = RhiBufferUsageFlags::UnorderedAccess | RhiBufferUsageFlags::CopyDest;
        wantedDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
        wantedDesc.initialState = RhiResourceState::UnorderedAccess;
        slot.wantedHandle = m_device->CreateRhiBuffer(wantedDesc);
        slot.wantedCompat = m_device->GetD3D12CompatibilityResource(slot.wantedHandle);
        slot.wantedState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        RhiBufferDesc readbackDesc{};
        readbackDesc.sizeInBytes = kReadbackBufferSize;
        readbackDesc.strideInBytes = 4u;
        readbackDesc.usage = RhiBufferUsageFlags::CopyDest;
        readbackDesc.memoryUsage = RhiMemoryUsage::GpuToCpu;
        readbackDesc.initialState = RhiResourceState::CopyDest;
        slot.readbackHandle = m_device->CreateRhiBuffer(readbackDesc);
        slot.readbackCompat = m_device->GetD3D12CompatibilityResource(slot.readbackHandle);

        return slot.visibleCompat && slot.visibleCompat->IsValid() &&
               slot.counterCompat && slot.counterCompat->IsValid() &&
               slot.argsCompat && slot.argsCompat->IsValid() &&
               slot.cbCompat && slot.cbCompat->IsValid() && slot.cbMapped != nullptr &&
               slot.wantedCompat && slot.wantedCompat->IsValid() &&
               slot.readbackCompat && slot.readbackCompat->IsValid();
    }

    bool ClusterGpuCulling::CreateVisibleBuffer(uint32_t capacity, RhiBufferHandle& outHandle, Resource*& outCompat) const
    {
        if (!m_device || capacity == 0) {
            return false;
        }

        RhiBufferDesc visibleDesc{};
        visibleDesc.sizeInBytes = static_cast<uint64_t>(capacity) * kVisibleBufferStride;
        visibleDesc.strideInBytes = kVisibleBufferStride;
        visibleDesc.usage = RhiBufferUsageFlags::Structured | RhiBufferUsageFlags::ShaderResource |
                           RhiBufferUsageFlags::UnorderedAccess;
        visibleDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
        visibleDesc.initialState = RhiResourceState::UnorderedAccess;

        const RhiBufferHandle handle = m_device->CreateRhiBuffer(visibleDesc);
        Resource* compat = m_device->GetD3D12CompatibilityResource(handle);
        if (!compat || !compat->IsValid()) {
            if (handle.IsValid()) {
                m_device->DestroyRhiResource(handle);
            }
            return false;
        }

        outHandle = handle;
        outCompat = compat;
        return true;
    }

    uint32_t ClusterGpuCulling::ReadRequestedVisibleCount(const ViewSlotResources& slot) const
    {
        // Same completion guarantee ReadFeedback documents: this slot's frame fence was waited on
        // before the frame that reuses frameIndex got here, so the readback holds the result of the
        // last recording of this very slot.
        if (!slot.recorded || !slot.readbackCompat || !slot.readbackCompat->IsValid()) {
            return 0;
        }

        void* mapped = nullptr;
        const D3D12_RANGE readRange{ kRequestedCountOffset, kRequestedCountOffset + 4u };
        if (FAILED(slot.readbackCompat->Map(0, &readRange, &mapped)) || !mapped) {
            return 0;
        }

        uint32_t requested = 0;
        std::memcpy(&requested, static_cast<const uint8_t*>(mapped) + kRequestedCountOffset, sizeof(requested));

        const D3D12_RANGE writtenRange{ 0, 0 };
        slot.readbackCompat->Unmap(0, &writtenRange);
        return requested;
    }

    void ClusterGpuCulling::GrowVisibleBuffer(ViewSlotResources& slot, uint32_t required)
    {
        if (required <= slot.visibleCapacity || slot.visibleCapacity >= kVisibleClusterCeiling) {
            return;
        }

        // Doubling until it fits, in one step, so a slot learns the whole requirement from a single
        // feedback round instead of one doubling per round -- and so a view whose demand creeps up
        // by a cluster a frame does not reallocate every frame. Same rule as
        // ClusterInstanceBuffer::GrowSlot, deliberately not a second growth policy.
        uint32_t target = slot.visibleCapacity != 0 ? slot.visibleCapacity : kInitialVisibleClusters;
        while (target < required) {
            target = (target > kVisibleClusterCeiling / 2u) ? kVisibleClusterCeiling : target * 2u;
        }

        RhiBufferHandle grownHandle{};
        Resource* grownCompat = nullptr;
        if (!CreateVisibleBuffer(target, grownHandle, grownCompat)) {
            char buffer[224] = {};
            std::snprintf(buffer, sizeof(buffer),
                          "[ClusterGpuCulling] GrowVisibleBuffer: allocation for %u entries (%.1f MB) failed; keeping the %u-entry list "
                          "(the excess is reported as kOverflowVisibleList).\n",
                          target, VisibleMegabytesFor(target), slot.visibleCapacity);
            DebugLog(buffer);
            return;
        }

        if (slot.visibleHandle.IsValid()) {
            ViewSlotResources::RetiredVisibleBuffer retired{};
            retired.handle = slot.visibleHandle;
            retired.updatesRemaining = m_frameCount;
            slot.retiredVisible.push_back(retired);
        }

        char buffer[192] = {};
        std::snprintf(buffer, sizeof(buffer),
                      "[ClusterGpuCulling] GrowVisibleBuffer: %u -> %u entries (%.1f MB) for this view slot; demand was %u.\n",
                      slot.visibleCapacity, target, VisibleMegabytesFor(target), required);
        DebugLog(buffer);

        slot.visibleHandle = grownHandle;
        slot.visibleCompat = grownCompat;
        slot.visibleCapacity = target;
        slot.visibleState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    void ClusterGpuCulling::TickRetiredVisible(ViewSlotResources& slot)
    {
        for (size_t i = slot.retiredVisible.size(); i-- > 0;) {
            if (slot.retiredVisible[i].updatesRemaining > 0) {
                --slot.retiredVisible[i].updatesRemaining;
            }
            if (slot.retiredVisible[i].updatesRemaining == 0) {
                if (m_device && slot.retiredVisible[i].handle.IsValid()) {
                    m_device->DestroyRhiResource(slot.retiredVisible[i].handle);
                }
                slot.retiredVisible.erase(slot.retiredVisible.begin() + static_cast<ptrdiff_t>(i));
            }
        }
    }

    void ClusterGpuCulling::ReleaseSlot(ViewSlotResources& slot)
    {
        if (slot.cbMapped && slot.cbCompat) {
            slot.cbCompat->Unmap(0, nullptr);
        }
        if (m_device) {
            // Buffers still inside the retirement window go first: Release tears the whole object
            // down, so there is no later tick that would free them.
            for (const ViewSlotResources::RetiredVisibleBuffer& retired : slot.retiredVisible) {
                if (retired.handle.IsValid()) {
                    m_device->DestroyRhiResource(retired.handle);
                }
            }
            if (slot.visibleHandle.IsValid())  m_device->DestroyRhiResource(slot.visibleHandle);
            if (slot.counterHandle.IsValid())  m_device->DestroyRhiResource(slot.counterHandle);
            if (slot.argsHandle.IsValid())     m_device->DestroyRhiResource(slot.argsHandle);
            if (slot.cbHandle.IsValid())       m_device->DestroyRhiResource(slot.cbHandle);
            if (slot.wantedHandle.IsValid())   m_device->DestroyRhiResource(slot.wantedHandle);
            if (slot.readbackHandle.IsValid()) m_device->DestroyRhiResource(slot.readbackHandle);
        }
        slot = ViewSlotResources{};
    }

    ClusterGpuCulling::ViewSlotResources* ClusterGpuCulling::FindSlot(uint32_t frameIndex, uint32_t viewSlot)
    {
        if (frameIndex >= m_frameCount || viewSlot >= kViewSlotsPerFrame) return nullptr;
        return &m_slots[static_cast<size_t>(frameIndex) * kViewSlotsPerFrame + viewSlot];
    }

    const ClusterGpuCulling::ViewSlotResources* ClusterGpuCulling::FindSlot(uint32_t frameIndex, uint32_t viewSlot) const
    {
        if (frameIndex >= m_frameCount || viewSlot >= kViewSlotsPerFrame) return nullptr;
        return &m_slots[static_cast<size_t>(frameIndex) * kViewSlotsPerFrame + viewSlot];
    }

    bool ClusterGpuCulling::RecordSelect(CommandList& cmdList, uint32_t frameIndex, uint32_t viewSlot, const ClusterSelectParams& params,
                                         D3D12_GPU_VIRTUAL_ADDRESS clustersVA, D3D12_GPU_VIRTUAL_ADDRESS groupsVA,
                                         D3D12_GPU_VIRTUAL_ADDRESS nodesVA, D3D12_GPU_VIRTUAL_ADDRESS instancesVA)
    {
        if (!m_valid || !params.viewProjection) return false;
        if (params.pageOffsetTableVA == 0 || params.pagePoolVA == 0) return false;

        ViewSlotResources* slot = FindSlot(frameIndex, viewSlot);
        if (!slot || !slot->visibleCompat || !slot->visibleCompat->IsValid() ||
            !slot->counterCompat || !slot->counterCompat->IsValid() ||
            !slot->argsCompat || !slot->argsCompat->IsValid() ||
            !slot->cbCompat || !slot->cbCompat->IsValid() || !slot->cbMapped ||
            !slot->wantedCompat || !slot->wantedCompat->IsValid() ||
            !slot->readbackCompat || !slot->readbackCompat->IsValid()) {
            return false;
        }

        ID3D12GraphicsCommandList* cl = cmdList.Get();
        if (!cl) return false;

        // Capacity first, because everything below (the CB's maxVisible, the root UAV, the barriers)
        // has to see the buffer this view will actually write into. Ticking the retirement list here
        // makes it tick once per frame for this slot: a (frameIndex, viewSlot) is recorded at most
        // once per frame.
        TickRetiredVisible(*slot);
        GrowVisibleBuffer(*slot, ReadRequestedVisibleCount(*slot));
        slot->recordedVisibleCapacity = slot->visibleCapacity;

        ClusterSelectCBData cb{};
        std::memcpy(cb.viewProj, params.viewProjection, sizeof(cb.viewProj));
        cb.cameraPos[0]   = params.cameraPos[0];
        cb.cameraPos[1]   = params.cameraPos[1];
        cb.cameraPos[2]   = params.cameraPos[2];
        cb.lodThresholdPx = params.lodThresholdPx;
        cb.projScale      = params.projScale;
        cb.zNear          = params.zNear;
        cb.orthographic   = params.orthographic ? 1.0f : 0.0f;
        cb.instanceCount  = params.instanceCount;
        cb.maxVisible     = slot->visibleCapacity;
        cb.collectStats   = params.collectStats ? 1u : 0u;
        std::memcpy(slot->cbMapped, &cb, sizeof(cb));

        // Reset counter, args and the wanted bitset to zero: UAV -> COPY_DEST, copy from the shared zero buffer, COPY_DEST -> UAV.
        D3D12_RESOURCE_BARRIER toCopyDest[3] = {
            CD3DX12_RESOURCE_BARRIER::Transition(slot->counterCompat->Get(), slot->counterState, D3D12_RESOURCE_STATE_COPY_DEST),
            CD3DX12_RESOURCE_BARRIER::Transition(slot->argsCompat->Get(), slot->argsState, D3D12_RESOURCE_STATE_COPY_DEST),
            CD3DX12_RESOURCE_BARRIER::Transition(slot->wantedCompat->Get(), slot->wantedState, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        cl->ResourceBarrier(3, toCopyDest);
        slot->counterState = D3D12_RESOURCE_STATE_COPY_DEST;
        slot->argsState    = D3D12_RESOURCE_STATE_COPY_DEST;
        slot->wantedState  = D3D12_RESOURCE_STATE_COPY_DEST;

        cl->CopyBufferRegion(slot->counterCompat->Get(), 0, m_zeroBufferCompat->Get(), 0, kCounterBufferSize);
        cl->CopyBufferRegion(slot->argsCompat->Get(), 0, m_zeroBufferCompat->Get(), 0, kArgsBytesToReset);
        // Only the words that can hold a group are reset/read back: Bistro has ~8k groups (1 KB),
        // so copying the whole 128 KB bitset per view was pure overhead.
        const uint32_t wantedWords = (params.groupCount == 0u) ? kWantedWordCount
            : (std::min)(kWantedWordCount, (params.groupCount + 31u) / 32u);
        slot->wantedBytes = ((wantedWords * 4u) + 15u) & ~15u;
        cl->CopyBufferRegion(slot->wantedCompat->Get(), 0, m_zeroBufferCompat->Get(), 0, slot->wantedBytes);

        D3D12_RESOURCE_BARRIER toUav[3] = {
            CD3DX12_RESOURCE_BARRIER::Transition(slot->counterCompat->Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(slot->argsCompat->Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(slot->wantedCompat->Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        cl->ResourceBarrier(3, toUav);
        slot->counterState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        slot->argsState    = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        slot->wantedState  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        cl->SetComputeRootSignature(m_rootSignature.Get());
        cl->SetPipelineState(m_selectPso.Get());
        cl->SetComputeRootConstantBufferView(0, slot->cbCompat->GetGPUVirtualAddress());
        cl->SetComputeRootShaderResourceView(1, clustersVA);
        cl->SetComputeRootShaderResourceView(2, groupsVA);
        cl->SetComputeRootShaderResourceView(3, nodesVA);
        cl->SetComputeRootShaderResourceView(4, instancesVA);
        cl->SetComputeRootUnorderedAccessView(5, slot->visibleCompat->GetGPUVirtualAddress());
        cl->SetComputeRootUnorderedAccessView(6, slot->counterCompat->GetGPUVirtualAddress());
        cl->SetComputeRootUnorderedAccessView(7, slot->argsCompat->GetGPUVirtualAddress());
        cl->SetComputeRootUnorderedAccessView(8, slot->wantedCompat->GetGPUVirtualAddress());
        cl->SetComputeRootShaderResourceView(9, params.pageOffsetTableVA);
        cl->SetComputeRootShaderResourceView(10, params.pagePoolVA);

        // One thread group per instance, packed into 2D the same way CS_ClusterSelectArgs packs the
        // indirect DispatchMesh: a 1D Dispatch would exceed D3D12's 65535 thread groups per
        // dimension past 65535 instances. The round-up leaves surplus groups, which the shader
        // early-returns on. y is clamped to 1 so an empty view still dispatches nothing (x == 0)
        // rather than Dispatch(0, 0, 1), matching the pre-2D behaviour exactly.
        const uint32_t selectGroupsX = (std::min)(params.instanceCount, kSelectGroupsX);
        const uint32_t selectGroupsY = (std::max)((params.instanceCount + kSelectGroupsX - 1u) / kSelectGroupsX, 1u);
        cl->Dispatch(selectGroupsX, selectGroupsY, 1);

        // CS_ClusterSelectArgs reads the counter this shader just wrote via atomics.
        const D3D12_RESOURCE_BARRIER uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(nullptr);
        cl->ResourceBarrier(1, &uavBarrier);

        cl->SetPipelineState(m_argsPso.Get());
        cl->SetComputeRootUnorderedAccessView(8, slot->wantedCompat->GetGPUVirtualAddress());
        cl->SetComputeRootShaderResourceView(9, params.pageOffsetTableVA);
        cl->SetComputeRootShaderResourceView(10, params.pagePoolVA);
        cl->Dispatch(1, 1, 1);

        D3D12_RESOURCE_BARRIER toConsumed[4] = {
            CD3DX12_RESOURCE_BARRIER::Transition(slot->argsCompat->Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
            CD3DX12_RESOURCE_BARRIER::Transition(slot->visibleCompat->Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(slot->counterCompat->Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(slot->wantedCompat->Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        cl->ResourceBarrier(4, toConsumed);
        slot->argsState    = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        slot->visibleState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        slot->counterState = D3D12_RESOURCE_STATE_COPY_SOURCE;
        slot->wantedState  = D3D12_RESOURCE_STATE_COPY_SOURCE;

        cl->CopyBufferRegion(slot->readbackCompat->Get(), 0, slot->counterCompat->Get(), 0, kCounterBufferSize);
        cl->CopyBufferRegion(slot->readbackCompat->Get(), kCounterBufferSize, slot->wantedCompat->Get(), 0, slot->wantedBytes);

        D3D12_RESOURCE_BARRIER toFinal[2] = {
            CD3DX12_RESOURCE_BARRIER::Transition(slot->counterCompat->Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(slot->wantedCompat->Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        cl->ResourceBarrier(2, toFinal);
        slot->counterState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        slot->wantedState  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        slot->recorded = true;
        return true;
    }

    void ClusterGpuCulling::RecordRelease(CommandList& cmdList, uint32_t frameIndex, uint32_t viewSlot)
    {
        if (!m_valid) return;

        ViewSlotResources* slot = FindSlot(frameIndex, viewSlot);
        if (!slot || !slot->visibleCompat || !slot->counterCompat || !slot->argsCompat) return;

        ID3D12GraphicsCommandList* cl = cmdList.Get();
        if (!cl) return;

        D3D12_RESOURCE_BARRIER toUav[3] = {
            CD3DX12_RESOURCE_BARRIER::Transition(slot->argsCompat->Get(), slot->argsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(slot->visibleCompat->Get(), slot->visibleState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            CD3DX12_RESOURCE_BARRIER::Transition(slot->counterCompat->Get(), slot->counterState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        cl->ResourceBarrier(3, toUav);
        slot->argsState    = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        slot->visibleState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        slot->counterState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    D3D12_GPU_VIRTUAL_ADDRESS ClusterGpuCulling::GetVisibleListVA(uint32_t frameIndex, uint32_t viewSlot) const
    {
        const ViewSlotResources* slot = FindSlot(frameIndex, viewSlot);
        return (slot && slot->visibleCompat && slot->visibleCompat->IsValid()) ? slot->visibleCompat->GetGPUVirtualAddress() : 0u;
    }

    D3D12_GPU_VIRTUAL_ADDRESS ClusterGpuCulling::GetCounterVA(uint32_t frameIndex, uint32_t viewSlot) const
    {
        const ViewSlotResources* slot = FindSlot(frameIndex, viewSlot);
        return (slot && slot->counterCompat && slot->counterCompat->IsValid()) ? slot->counterCompat->GetGPUVirtualAddress() : 0u;
    }

    ID3D12Resource* ClusterGpuCulling::GetArgsResource(uint32_t frameIndex, uint32_t viewSlot) const
    {
        const ViewSlotResources* slot = FindSlot(frameIndex, viewSlot);
        return (slot && slot->argsCompat) ? slot->argsCompat->Get() : nullptr;
    }

    ID3D12CommandSignature* ClusterGpuCulling::GetDispatchMeshCommandSignature() const
    {
        return m_commandSignature.Get();
    }

    bool ClusterGpuCulling::ReadFeedback(uint32_t frameIndex, uint32_t viewSlot, ClusterFeedbackReadout& out) const
    {
        const ViewSlotResources* slot = FindSlot(frameIndex, viewSlot);
        if (!slot || !slot->recorded || !slot->readbackCompat || !slot->readbackCompat->IsValid()) {
            return false;
        }

        void* mapped = nullptr;
        const D3D12_RANGE readRange{ 0, kCounterBufferSize + slot->wantedBytes };
        if (FAILED(slot->readbackCompat->Map(0, &readRange, &mapped)) || !mapped) {
            return false;
        }

        const uint8_t* bytes = static_cast<const uint8_t*>(mapped);
        uint32_t counterValues[7] = {};
        std::memcpy(counterValues, bytes, sizeof(counterValues));
        out.visibleCount        = counterValues[0];
        out.overflow            = counterValues[1];
        out.instancesProcessed  = counterValues[2];
        out.nodesVisited        = counterValues[3];
        out.maxTraversalLevels  = counterValues[4];
        out.clustersExamined    = counterValues[5];
        out.requestedCount      = counterValues[6];
        // The capacity that was in force when this readback was produced, not the slot's current
        // one: RecordSelect may already have grown the buffer in response to this very readout.
        out.visibleCapacity     = slot->recordedVisibleCapacity;

        out.wantedBits.resize(slot->wantedBytes / 4u);
        std::memcpy(out.wantedBits.data(), bytes + kCounterBufferSize, slot->wantedBytes);

        const D3D12_RANGE writtenRange{ 0, 0 };
        slot->readbackCompat->Unmap(0, &writtenRange);
        return true;
    }
}
