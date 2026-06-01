#pragma once
#include <functional>
#include <vector>
#include <string>
#include <wrl.h>
#include <d3d12.h>
#include <windows.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"

using Microsoft::WRL::ComPtr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace SasamiRenderer
{
    // Minimal singleton wrapper around Dear ImGui (Win32 + DX12)
    class ImGuiCoordinator
    {
    public:
        static ImGuiCoordinator& Instance();

        bool InitializePlatformOnly(HWND hWnd);
        bool Initialize(HWND hWnd,
            ID3D12Device* device,
            ID3D12CommandQueue* queue,
            DXGI_FORMAT rtvFormat,
            DXGI_FORMAT dsvFormat,
            int numFramesInFlight = 2);
        void Shutdown();
        void NewFrame();
        void Render(ID3D12GraphicsCommandList* cmdList, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle);

        // Register UI drawers (thread-unsafe, call during init)
        void RegisterWindow(const char* name, std::function<void()> drawFn);

        // Feed a Win32 message to ImGui. Returns true if ImGui handled it.
        bool HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
        bool WantsMouseCapture() const;
        bool WantsKeyboardCapture() const;

    private:
        ImGuiCoordinator() = default;
        ~ImGuiCoordinator() = default;
        ImGuiCoordinator(const ImGuiCoordinator&) = delete;
        ImGuiCoordinator& operator=(const ImGuiCoordinator&) = delete;

        static void AllocateSrvDescriptorForImGui(ImGui_ImplDX12_InitInfo* info,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
                                                  D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle);
        static void FreeSrvDescriptorForImGui(ImGui_ImplDX12_InitInfo* info,
                                              D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
                                              D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);
        void ResetSrvDescriptorAllocator(UINT descriptorCount);
        void AllocateSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
                                   D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle);
        void FreeSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
                               D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);

        // Data
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
        UINT m_srvDescriptorSize = 0;
        std::vector<UINT> m_freeSrvDescriptorIndices;
        std::vector<std::pair<std::string, std::function<void()>>> m_windows;
        bool m_initialized = false;
        bool m_dx12BackendInitialized = false;
    };
}
