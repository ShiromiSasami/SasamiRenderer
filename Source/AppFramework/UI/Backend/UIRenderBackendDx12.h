#pragma once

#include "IUIRenderBackend.h"

#include <vector>
#include <wrl.h>
#include <d3d12.h>

#include "backends/imgui_impl_dx12.h"

#if RHI_DIRECTX12

namespace SasamiEngine::App::UI
{
    // D3D12 half of the ImGui renderer backend. Owns the shader-visible SRV heap ImGui needs for
    // font/texture bindings and forwards frame calls to ImGui_ImplDX12.
    class UIRenderBackendDx12 final : public IUIRenderBackend
    {
    public:
        bool Initialize(const UIRenderBackendInitDesc& desc) override;
        void Shutdown() override;
        void NewFrame() override;
        void RenderDrawData(const Render::RhiOverlayFrameContext& context) override;
        bool IsInitialized() const override { return m_initialized; }

    private:
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

        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
        UINT m_srvDescriptorSize = 0;
        std::vector<UINT> m_freeSrvDescriptorIndices;
        bool m_initialized = false;
    };
}

namespace SasamiEngine
{
    using namespace App;
}

#endif // RHI_DIRECTX12
