// When and in what order a session is written and restored. Three stores are involved and
// their order matters: the framework's scene file (camera + lights), this sample's settings
// file, and the GI probe cache, which is fingerprinted from the first two and so has to be
// written last and read only once the scene bounds and the renderer's GI/SWRT features are up.
// The contents of each store live elsewhere (RenderingAppSettingsFile.cpp,
// RenderingAppGiProbeCache.cpp); this file owns only the sequencing.

#include "RenderingApp.h"
#include "RenderingAppSessionFiles.h"
#include "ApplicationCore.h"
#include "ApplicationResourcePaths.h"

#include <string>

namespace SasamiEngine::Sample::PBR
{
    void RenderingApp::SaveSessionState(ApplicationCore& app) const
    {
        // Camera + lights via the framework scene writer.
        const std::string scenePath =
            ApplicationResourcePaths::ResolveConfigPathString(kSceneStateFile);
        app.SaveScene(scenePath);

        // Render settings + GI toggles + app UI settings.
        const std::string settingsPath =
            ApplicationResourcePaths::ResolveConfigPathString(kSettingsStateFile);
        WriteSettingsFile(app, settingsPath);

        // Last: its validity key is the content of the two files written above.
        PersistGiProbeCache(app);
    }

    void RenderingApp::LoadSessionState(ApplicationCore& app)
    {
        const std::string settingsPath =
            ApplicationResourcePaths::ResolveConfigPathString(kSettingsStateFile);
        const std::string scenePath =
            ApplicationResourcePaths::ResolveConfigPathString(kSceneStateFile);

        ReadSettingsFile(app, settingsPath);

        // Camera + lights (non-destructive: applied onto existing objects; overrides
        // the default view/lights when a saved scene file is present).
        app.ApplyCameraAndLights(scenePath);

        // GI probe grid fit + probe cache load / bake request need final scene bounds
        // (Sponza/bunny async loads settled) and renderer GI/SWRT readiness. Apply now
        // if those conditions already hold (e.g. a manual/UI-triggered reload after the
        // app is fully up), otherwise defer to OnUpdate via m_pendingGiSessionRestore.
        if (IsGiSessionRestoreReady(app)) {
            m_pendingGiSessionRestore = !ApplyDeferredGiSessionState(app);
        } else {
            m_pendingGiSessionRestore = true;
        }
    }
}
