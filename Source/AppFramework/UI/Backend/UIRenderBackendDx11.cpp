#include "UIRenderBackendDx11.h"

#if RHI_DIRECTX11

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

#include <d3d11.h>

namespace SasamiEngine::App::UI
{
    bool UIRenderBackendDx11::Initialize(const UIRenderBackendInitDesc& desc)
    {
        auto* device = static_cast<ID3D11Device*>(desc.native.device);
        auto* context = static_cast<ID3D11DeviceContext*>(desc.native.queue);
        if (device == nullptr || context == nullptr) {
            return false;
        }

        m_initialized = ImGui_ImplDX11_Init(device, context);
        return m_initialized;
    }

    void UIRenderBackendDx11::Shutdown()
    {
        // ImGui asserts when its backend entry points run without backend data, so every one of
        // these is gated on a successful Initialize rather than on the caller remembering.
        if (!m_initialized) return;
        ImGui_ImplDX11_Shutdown();
        m_initialized = false;
    }

    void UIRenderBackendDx11::NewFrame()
    {
        if (!m_initialized) return;
        ImGui_ImplDX11_NewFrame();
    }

    void UIRenderBackendDx11::RenderDrawData(const Render::RhiOverlayFrameContext& context)
    {
        if (!m_initialized) return;
        // DX11 records into the immediate context, which the caller already has bound to the
        // current render target -- nothing in `context` is needed here.
        (void)context;
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    bool UIRenderBackendDx11::IsInitialized() const
    {
        return m_initialized;
    }
}

#endif // RHI_DIRECTX11
