#pragma once

namespace SasamiEngine::App { class ApplicationCore; }

namespace SasamiEngine::Sample::PBR
{
    // Fits the GI probe grid to one of this sample's coverage presets
    // (0=Interior, 1=Wide, 2=Very Wide, 3=Scene Auto).
    //
    // Returns false only for Scene Auto while the scene still has no bounds to fit, so the
    // caller can retry instead of settling for the fallback box. The fixed-box presets always
    // succeed. Called by the UI's GI tab preset combo (RenderingAppUI.cpp) and by the deferred
    // session restore (RenderingAppGiProbeCache.cpp), which is why it keeps external linkage.
    // Qualified: this header is included first by RenderingAppProbeGrid.cpp, before anything
    // pulls in IApplication.h and with it the "using namespace App" that makes the short name
    // visible from inside SasamiEngine::Sample::PBR.
    bool ApplyProbeGridPreset(App::ApplicationCore& app, int presetIndex);
}
