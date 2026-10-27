#pragma once

#include "IUIRenderBackend.h"

#if RHI_OPENGL

namespace SasamiEngine::App::UI
{
    // ImGui renderer backend for the OpenGL build. Records through the current GL context,
    // which the caller has already made current for this thread this frame.
    class UIRenderBackendOpenGL final : public IUIRenderBackend
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

#endif // RHI_OPENGL
