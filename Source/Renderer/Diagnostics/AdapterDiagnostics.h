#pragma once

#include <dxgi1_4.h>

namespace SasamiRenderer
{
    // Logs the adapter's name/VRAM budget and warns if another process is
    // hogging VRAM. Diagnostic only: failures never propagate to the caller.
    void LogAdapterDiagnostics(IDXGIAdapter1* adapter);
}
