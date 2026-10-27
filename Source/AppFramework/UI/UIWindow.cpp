#include "UIWindow.h"

#include "imgui.h"

namespace SasamiEngine::App::UI
{
    Window::Window(const char* title, bool* isOpen)
    {
        m_visible = ImGui::Begin(title ? title : "Window", isOpen);
    }

    Window::~Window()
    {
        // ImGui::End() is unconditional: it must be called even if Begin() returned false.
        ImGui::End();
    }
}
