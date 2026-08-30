#pragma once

#include "Foundation/Boot/BootStatus.h"

namespace SasamiRenderer::BootProgressWindow
{
    // Draws the boot progress UI for the current ImGui frame. Call between
    // ImGui NewFrame and Render. fullScreen=true -> centered loading panel
    // (boot phase); false -> compact corner overlay (scene already visible).
    void Draw(const BootStatus& status, bool fullScreen);
}
