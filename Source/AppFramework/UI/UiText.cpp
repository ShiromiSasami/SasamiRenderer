#include "UiText.h"

#include "imgui.h"

#include <cstdarg>

namespace SasamiRenderer::UI
{
    void Text(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        ImGui::TextV(fmt, args);
        va_end(args);
    }

    void TextDisabled(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        ImGui::TextDisabledV(fmt, args);
        va_end(args);
    }
}
