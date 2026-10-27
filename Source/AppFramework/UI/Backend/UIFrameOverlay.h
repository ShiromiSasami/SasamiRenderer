#pragma once

#include "Renderer/RHI/RhiUIOverlay.h"

namespace SasamiEngine::App::UI
{
    class IUIRenderBackend;

    // The renderer's hook into the UI layer.
    //
    // Backends that own their whole frame (Vulkan / DX11 / OpenGL) cannot be handed a command list
    // from the outside, so they call this back from inside ExecuteBackendFrame instead. All it does
    // is forward to the active ImGui renderer backend -- the draw data was already built by
    // ImGuiCoordinator::EndFrame earlier in the frame.
    class UIFrameOverlay final : public Render::IRhiFrameOverlay
    {
    public:
        // The backend is owned by ImGuiCoordinator; this only borrows it. Pass null to detach.
        void SetBackend(IUIRenderBackend* backend) { m_backend = backend; }
        // Mirrors ImGuiCoordinator::SetDrawVisible for backends that record the UI themselves.
        void SetVisible(bool visible) { m_visible = visible; }

        void RecordOverlay(const Render::RhiOverlayFrameContext& context) override;

    private:
        IUIRenderBackend* m_backend = nullptr;
        bool m_visible = true;
    };
}

namespace SasamiEngine
{
    using namespace App;
}
