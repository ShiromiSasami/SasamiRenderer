#pragma once

namespace SasamiEngine::App::UI
{
    // Button part. Returns true on the frame the button is clicked,
    // matching ImGui::Button.
    bool Button(const char* label);
}
namespace SasamiEngine { using namespace App; }
