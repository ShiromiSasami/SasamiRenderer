#include "ImGuiCoordinator.h"

namespace SasamiRenderer
{
   
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

        // Descriptor heap for ImGui textures
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 64;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_srvHeap))))
            return false;

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
        info.LegacySingleSrvCpuDescriptor = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        info.LegacySingleSrvGpuDescriptor = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        if (!ImGui_ImplDX12_Init(&info))
            return false;

        m_initialized = true;
        return true;
    }

    void ImGuiCoordinator::Shutdown()
    {
        if (!m_initialized) return;
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_srvHeap.Reset();
        m_windows.clear();
        m_initialized = false;
    }

    void ImGuiCoordinator::NewFrame()
    {
        if (!m_initialized) return;
        ImGui_ImplDX12_NewFrame();
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
}
