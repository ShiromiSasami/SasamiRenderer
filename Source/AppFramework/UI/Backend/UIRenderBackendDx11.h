#pragma once

#include "IUIRenderBackend.h"

#if RHI_DIRECTX11

namespace SasamiEngine::App::UI
{
    // ImGui renderer backend for the DirectX 11 build. Records into the device's immediate
    // context, which the caller already has bound to the current render target.
    class UIRenderBackendDx11 final : public IUIRenderBackend
    {
    public:
        bool Initialize(const UIRenderBackendInitDesc& desc) override;
        void Shutdown() override;
        void NewFrame() override;
        void RenderDrawData(const Render::RhiOverlayFrameContext& context) override;
        bool IsInitialized() const override;

    private:
        bool m_initialized = false;
    };
}

#endif // RHI_DIRECTX11
