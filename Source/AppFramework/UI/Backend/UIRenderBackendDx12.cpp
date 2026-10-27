#include "UIRenderBackendDx12.h"

#if RHI_DIRECTX12

#include "Foundation/Tools/DebugOutput.h"

namespace SasamiEngine::App::UI
{
    namespace
    {
        constexpr UINT IMGUI_SRV_DESCRIPTOR_COUNT = 64;
    }

    bool UIRenderBackendDx12::Initialize(const UIRenderBackendInitDesc& desc)
    {
        auto* device = static_cast<ID3D12Device*>(desc.d3d12Device);
        auto* queue = static_cast<ID3D12CommandQueue*>(desc.d3d12CommandQueue);
        if (!device || !queue) {
            return false;
        }

        // Descriptor heap for ImGui textures
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = IMGUI_SRV_DESCRIPTOR_COUNT;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap))))
            return false;
        m_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ResetSrvDescriptorAllocator(heapDesc.NumDescriptors);

        ImGui_ImplDX12_InitInfo info{};
        info.Device = device;
        info.CommandQueue = queue;
        info.NumFramesInFlight = static_cast<int>(desc.framesInFlight);
        info.RTVFormat = static_cast<DXGI_FORMAT>(desc.rtvFormat);
        info.DSVFormat = static_cast<DXGI_FORMAT>(desc.dsvFormat);
        info.SrvDescriptorHeap = m_srvHeap.Get();
        info.UserData = this;
        info.SrvDescriptorAllocFn = &UIRenderBackendDx12::AllocateSrvDescriptorForImGui;
        info.SrvDescriptorFreeFn = &UIRenderBackendDx12::FreeSrvDescriptorForImGui;
        if (!ImGui_ImplDX12_Init(&info))
            return false;

        m_initialized = true;
        return true;
    }

    void UIRenderBackendDx12::Shutdown()
    {
        if (!m_initialized) return;
        ImGui_ImplDX12_Shutdown();
        m_srvHeap.Reset();
        m_srvDescriptorSize = 0;
        m_freeSrvDescriptorIndices.clear();
        m_initialized = false;
    }

    void UIRenderBackendDx12::NewFrame()
    {
        if (!m_initialized) return;
        ImGui_ImplDX12_NewFrame();
    }

    void UIRenderBackendDx12::RenderDrawData(const Render::RhiOverlayFrameContext& context)
    {
        if (!m_initialized) return;
        auto* cmdList = static_cast<ID3D12GraphicsCommandList*>(context.commandBuffer);
        auto* rtvHandle = static_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(context.renderTarget);
        if (!cmdList || !rtvHandle) return;

        // Bind RTV and heap
        cmdList->OMSetRenderTargets(1, rtvHandle, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);
    }

    void UIRenderBackendDx12::AllocateSrvDescriptorForImGui(ImGui_ImplDX12_InitInfo* info,
                                                             D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
                                                             D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
    {
        auto* backend = info ? static_cast<UIRenderBackendDx12*>(info->UserData) : nullptr;
        if (!backend) {
            *outCpuHandle = {};
            *outGpuHandle = {};
            return;
        }
        backend->AllocateSrvDescriptor(outCpuHandle, outGpuHandle);
    }

    void UIRenderBackendDx12::FreeSrvDescriptorForImGui(ImGui_ImplDX12_InitInfo* info,
                                                        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
                                                        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
    {
        auto* backend = info ? static_cast<UIRenderBackendDx12*>(info->UserData) : nullptr;
        if (!backend) {
            return;
        }
        backend->FreeSrvDescriptor(cpuHandle, gpuHandle);
    }

    void UIRenderBackendDx12::ResetSrvDescriptorAllocator(UINT descriptorCount)
    {
        m_freeSrvDescriptorIndices.clear();
        m_freeSrvDescriptorIndices.reserve(descriptorCount);
        for (UINT i = descriptorCount; i > 0; --i) {
            m_freeSrvDescriptorIndices.push_back(i - 1);
        }
    }

    void UIRenderBackendDx12::AllocateSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
                                                     D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
    {
        if (!outCpuHandle || !outGpuHandle) {
            return;
        }
        if (!m_srvHeap || m_freeSrvDescriptorIndices.empty()) {
            *outCpuHandle = {};
            *outGpuHandle = {};
            DebugLog("UIRenderBackendDx12: SRV descriptor heap exhausted.\n");
            return;
        }

        const UINT index = m_freeSrvDescriptorIndices.back();
        m_freeSrvDescriptorIndices.pop_back();

        const D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        outCpuHandle->ptr = cpuStart.ptr + static_cast<SIZE_T>(index) * m_srvDescriptorSize;
        outGpuHandle->ptr = gpuStart.ptr + static_cast<UINT64>(index) * m_srvDescriptorSize;
    }

    void UIRenderBackendDx12::FreeSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
                                                D3D12_GPU_DESCRIPTOR_HANDLE)
    {
        if (!m_srvHeap || m_srvDescriptorSize == 0 || cpuHandle.ptr == 0) {
            return;
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        if (cpuHandle.ptr < cpuStart.ptr) {
            return;
        }

        const SIZE_T offset = cpuHandle.ptr - cpuStart.ptr;
        if ((offset % m_srvDescriptorSize) != 0) {
            return;
        }

        const UINT index = static_cast<UINT>(offset / m_srvDescriptorSize);
        if (index >= IMGUI_SRV_DESCRIPTOR_COUNT) {
            return;
        }

        m_freeSrvDescriptorIndices.push_back(index);
    }
}

#endif // RHI_DIRECTX12
