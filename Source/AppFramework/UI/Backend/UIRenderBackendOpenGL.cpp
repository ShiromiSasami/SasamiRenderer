#include "UIRenderBackendOpenGL.h"

#if RHI_OPENGL

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

namespace SasamiEngine::App::UI
{
    bool UIRenderBackendOpenGL::Initialize(const UIRenderBackendInitDesc& desc)
    {
        // This backend resolves its own GL entry points, so it needs none of `desc`.
        (void)desc;

        // "#version 130" targets the compatibility-profile GL context this renderer creates
        // (wglCreateContext in OpenGLGraphicsDevice.cpp:1340, no core-profile attribute, no VAOs,
        // and the scene shaders themselves are "#version 120"). Requesting a higher, core-profile
        // GLSL version here would be incompatible with that context.
        m_initialized = ImGui_ImplOpenGL3_Init("#version 130");
        return m_initialized;
    }

    void UIRenderBackendOpenGL::Shutdown()
    {
        // ImGui asserts when its backend entry points run without backend data, so every one of
        // these is gated on a successful Initialize rather than on the caller remembering.
        if (!m_initialized) return;
        ImGui_ImplOpenGL3_Shutdown();
        m_initialized = false;
    }

    void UIRenderBackendOpenGL::NewFrame()
    {
        if (!m_initialized) return;
        ImGui_ImplOpenGL3_NewFrame();
    }

    void UIRenderBackendOpenGL::RenderDrawData(const Render::RhiOverlayFrameContext& context)
    {
        if (!m_initialized) return;
        // The caller has already made its GL context current for this thread this frame, so
        // nothing in `context` is needed here.
        (void)context;
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    bool UIRenderBackendOpenGL::IsInitialized() const
    {
        return m_initialized;
    }
}

#endif // RHI_OPENGL
