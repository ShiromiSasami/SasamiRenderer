#include "ImGuiCoordinator.h"

namespace SasamiRenderer
{
    namespace
    {
        constexpr UINT IMGUI_SRV_DESCRIPTOR_COUNT = 64;
    }

    ImGuiCoordinator& ImGuiCoordinator::Instance()
    {
        static ImGuiCoordinator s_instance;
        return s_instance;
    }

    bool ImGuiCoordinator::Initialize(HWND hWnd,
                                  ID3D12Device* device,
                                  ID3D12CommandQueue* queue,
                                  DXGI_FORMAT rtvFormat,
                                  DXGI_FORMAT dsvFormat,
                                  int numFramesInFlight)
    {
        if (m_initialized)
            return true;
        if (!device || !queue) {
            return InitializePlatformOnly(hWnd);
        }

        // Descriptor heap for ImGui textures
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = IMGUI_SRV_DESCRIPTOR_COUNT;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_srvHeap))))
            return false;
        m_srvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ResetSrvDescriptorAllocator(desc.NumDescriptors);

        IMGUI_CHECKVERSION();
        // Enable per-monitor DPI awareness for proper mouse scaling on DPI changes
        ImGui_ImplWin32_EnableDpiAwareness();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        if (!ImGui_ImplWin32_Init(hWnd))
            return false;

        ImGui_ImplDX12_InitInfo info{};
        info.Device = device;
        info.CommandQueue = queue;
        info.NumFramesInFlight = numFramesInFlight;
        info.RTVFormat = rtvFormat;
        info.DSVFormat = dsvFormat;
        info.SrvDescriptorHeap = m_srvHeap.Get();
        info.UserData = this;
        info.SrvDescriptorAllocFn = &ImGuiCoordinator::AllocateSrvDescriptorForImGui;
        info.SrvDescriptorFreeFn = &ImGuiCoordinator::FreeSrvDescriptorForImGui;
        if (!ImGui_ImplDX12_Init(&info))
            return false;

        m_initialized = true;
        m_dx12BackendInitialized = true;
        return true;
    }

    bool ImGuiCoordinator::InitializePlatformOnly(HWND hWnd)
    {
        if (m_initialized)
            return true;

        IMGUI_CHECKVERSION();
        ImGui_ImplWin32_EnableDpiAwareness();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        if (!ImGui_ImplWin32_Init(hWnd))
            return false;

        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        m_initialized = true;
        m_dx12BackendInitialized = false;
        return true;
    }

    void ImGuiCoordinator::Shutdown()
    {
        if (!m_initialized) return;
        if (m_dx12BackendInitialized) {
            ImGui_ImplDX12_Shutdown();
        }
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_srvHeap.Reset();
        m_srvDescriptorSize = 0;
        m_freeSrvDescriptorIndices.clear();
        m_windows.clear();
        m_dx12BackendInitialized = false;
        m_initialized = false;
    }

    void ImGuiCoordinator::NewFrame()
    {
        if (!m_initialized) return;
        if (m_dx12BackendInitialized) {
            ImGui_ImplDX12_NewFrame();
        }
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        for (auto& w : m_windows)
        {
            if (ImGui::Begin(w.first.c_str()))
            {
                if (w.second) w.second();
            }
            ImGui::End();
        }
    }

    void ImGuiCoordinator::Render(ID3D12GraphicsCommandList* cmdList, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle)
    {
        if (!m_initialized) return;
        ImGui::Render();
        if (!m_dx12BackendInitialized || !cmdList) return;
        // Bind RTV and heap
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList);
    }

    void ImGuiCoordinator::RegisterWindow(const char* name, std::function<void()> drawFn)
    {
        m_windows.emplace_back(name ? name : "Window", std::move(drawFn));
    }

    bool ImGuiCoordinator::HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (!m_initialized) return false;
        return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
    }

    bool ImGuiCoordinator::WantsMouseCapture() const
    {
        return m_initialized ? ImGui::GetIO().WantCaptureMouse : false;
    }

    bool ImGuiCoordinator::WantsKeyboardCapture() const
    {
        return m_initialized ? ImGui::GetIO().WantCaptureKeyboard : false;
    }

    void ImGuiCoordinator::AllocateSrvDescriptorForImGui(ImGui_ImplDX12_InitInfo* info,
                                                         D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
                                                         D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
    {
        auto* coordinator = info ? static_cast<ImGuiCoordinator*>(info->UserData) : nullptr;
        if (!coordinator) {
            *outCpuHandle = {};
            *outGpuHandle = {};
            return;
        }
        coordinator->AllocateSrvDescriptor(outCpuHandle, outGpuHandle);
    }

    void ImGuiCoordinator::FreeSrvDescriptorForImGui(ImGui_ImplDX12_InitInfo* info,
                                                     D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
                                                     D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle)
    {
        auto* coordinator = info ? static_cast<ImGuiCoordinator*>(info->UserData) : nullptr;
        if (!coordinator) {
            return;
        }
        coordinator->FreeSrvDescriptor(cpuHandle, gpuHandle);
    }

    void ImGuiCoordinator::ResetSrvDescriptorAllocator(UINT descriptorCount)
    {
        m_freeSrvDescriptorIndices.clear();
        m_freeSrvDescriptorIndices.reserve(descriptorCount);
        for (UINT i = descriptorCount; i > 0; --i) {
            m_freeSrvDescriptorIndices.push_back(i - 1);
        }
    }

    void ImGuiCoordinator::AllocateSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
                                                 D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
    {
        if (!outCpuHandle || !outGpuHandle) {
            return;
        }
        if (!m_srvHeap || m_freeSrvDescriptorIndices.empty()) {
            *outCpuHandle = {};
            *outGpuHandle = {};
            OutputDebugStringA("ImGuiCoordinator: SRV descriptor heap exhausted.\n");
            return;
        }

        const UINT index = m_freeSrvDescriptorIndices.back();
        m_freeSrvDescriptorIndices.pop_back();

        const D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        outCpuHandle->ptr = cpuStart.ptr + static_cast<SIZE_T>(index) * m_srvDescriptorSize;
        outGpuHandle->ptr = gpuStart.ptr + static_cast<UINT64>(index) * m_srvDescriptorSize;
    }

    void ImGuiCoordinator::FreeSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
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
