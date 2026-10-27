#pragma once

namespace SasamiEngine::App::UI
{
    // Text parts. Signatures mirror ImGui::Text/TextDisabled (printf-style
    // formatting) so migrating a call site is a drop-in rename.
    void Text(const char* fmt, ...);
    void TextDisabled(const char* fmt, ...);
}
namespace SasamiEngine { using namespace App; }
