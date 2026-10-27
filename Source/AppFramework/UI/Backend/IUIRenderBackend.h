#pragma once

#include "Renderer/RHI/RhiUIOverlay.h"

#include <cstdint>
#include <memory>

namespace SasamiEngine::App::UI
{
    // What the UI layer needs to stand up an ImGui renderer backend.
    //
    // Two shapes of backend meet here. The D3D12 one is driven from the outside: the app owns the
    // device and hands a command list to the backend once per frame, so its init data is the D3D12
    // device, queue and target formats. Every other backend renders its own frame end to end and
    // calls us back from inside it, so its init data is whatever the device reports in `native`.
    // Native handles travel as void* / uint32_t to keep d3d12.h and vulkan.h out of this header.
    struct UIRenderBackendInitDesc
    {
        Render::RhiOverlayDeviceContext native{};

        void* d3d12Device = nullptr;         // ID3D12Device*
        void* d3d12CommandQueue = nullptr;   // ID3D12CommandQueue*
        uint32_t rtvFormat = 0;              // DXGI_FORMAT
        uint32_t dsvFormat = 0;              // DXGI_FORMAT
        uint32_t framesInFlight = 2;
    };

    // One ImGui renderer backend. Exactly one implementation is compiled into any given binary --
    // RhiBuild.props makes the graphics backends mutually exclusive -- so this is a seam for
    // separating responsibilities, not a runtime plug-in point.
    //
    // The platform half (ImGui_ImplWin32) is not part of this: it is the same on every backend and
    // stays in ImGuiCoordinator.
    class IUIRenderBackend
    {
    public:
        virtual ~IUIRenderBackend() = default;

        // Creates the ImGui renderer objects. An ImGui context must already exist.
        virtual bool Initialize(const UIRenderBackendInitDesc& desc) = 0;
        virtual void Shutdown() = 0;

        // ImGui_ImplXXX_NewFrame, before ImGui::NewFrame.
        virtual void NewFrame() = 0;

        // Submits ImGui::GetDrawData() into the frame described by `context`. Called after
        // ImGui::Render(), from wherever that backend's frame has a drawable target: the app's
        // overlay callback on D3D12, the device's own frame everywhere else.
        virtual void RenderDrawData(const Render::RhiOverlayFrameContext& context) = 0;

        virtual bool IsInitialized() const = 0;
    };

    // Creates the backend compiled into this binary. Null when the build has none.
    std::unique_ptr<IUIRenderBackend> CreateUIRenderBackend();
}

namespace SasamiEngine
{
    using namespace App;
}
