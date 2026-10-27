#include "UIText.h"

#include "imgui.h"

#include <cstdarg>

namespace SasamiEngine::App::UI
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
