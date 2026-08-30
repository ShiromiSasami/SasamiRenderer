#pragma once

namespace SasamiRenderer::UI
{
    // RAII scope guard around a top-level ImGui window (Begin/End).
    // ImGui::End() must always be called after ImGui::Begin(), regardless
    // of its return value; wrapping it here means the caller can never
    // forget to close the window or mismatch a Begin/End pair.
    //
    //   UI::Window window("My Panel");
    //   if (window) {
    //       UI::Text("Hello");
    //   }
    //
    class Window
    {
    public:
        explicit Window(const char* title, bool* isOpen = nullptr);
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;
        Window(Window&&) = delete;
        Window& operator=(Window&&) = delete;

        // True while the window is open/visible and its content should be drawn.
        explicit operator bool() const { return m_visible; }

    private:
        bool m_visible = false;
    };
}
