#pragma once

#include <dxgi1_4.h>

namespace SasamiEngine::Render
{
    // Logs the adapter's name/VRAM budget and warns if another process is
    // hogging VRAM. Diagnostic only: failures never propagate to the caller.
    void LogAdapterDiagnostics(IDXGIAdapter1* adapter);
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
