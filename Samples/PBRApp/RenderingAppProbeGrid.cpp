// The GI probe grid's coverage presets for this sample. Separate from the scene geometry it
// encloses and from the session restore that selects it: the boxes below are a GI sampling
// decision (how much of the world gets probes, at what spacing), and both callers - the UI
// combo and the deferred session restore - only need that one decision expressed.

#include "RenderingAppProbeGrid.h"

#include "ApplicationCore.h"

namespace SasamiEngine::Sample::PBR
{
    bool ApplyProbeGridPreset(App::ApplicationCore& app, int presetIndex)
    {
        switch (presetIndex) {
        case 0:
            // Sponza interior coverage.
            app.FitProbeGridToScene(-7.0f, -0.5f, -13.0f, 7.0f, 8.0f, 15.0f, 1.0f);
            break;
        case 2:
            // Wide interior plus large exterior buffer and more sky coverage.
            app.FitProbeGridToScene(-18.0f, -2.0f, -24.0f, 18.0f, 18.0f, 26.0f, 3.0f);
            break;
        case 1:
        default:
            // Default: wider than the old Sponza fit so probe debug is easier to inspect.
            app.FitProbeGridToScene(-12.0f, -1.0f, -18.0f, 12.0f, 12.0f, 20.0f, 2.0f);
            break;
        case 3:
            // Scene auto-fit: covers every loaded model (Sponza and the Bistro block at
            // x = 60 alike) instead of a hardcoded Sponza-sized box, coarsening probe
            // spacing as needed to stay inside the budget. Falls back to the default
            // interior box while the scene has no geometry yet.
            // Budget sized so covering the whole ~196x60x193m scene still lands near the 2.0m
            // spacing the old Sponza-sized box used; 16384 over that volume would coarsen to
            // roughly 5.2m and make the interpolation banding worse than before widening.
            if (!app.FitProbeGridToSceneAuto(2.0f, 131072u)) {
                app.FitProbeGridToScene(-12.0f, -1.0f, -18.0f, 12.0f, 12.0f, 20.0f, 2.0f);
                return false;
            }
            break;
        }
        return true;
    }
}
