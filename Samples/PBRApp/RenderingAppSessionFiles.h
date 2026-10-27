#pragma once

// The on-disk names of the three files this sample persists a session in. They are shared
// rather than repeated because the settings writer, the GI probe cache and the restore
// sequence each resolve a different subset of them, and a name that drifted between those
// would silently strand a file that the other side still expects to find.

namespace SasamiEngine::Sample::PBR
{
    // Camera + lights, written and read through ApplicationCore's scene serializer.
    constexpr const char* kSceneStateFile = "PBRApp.scene";
    // Render settings + GI toggles + app UI toggles (schema in RenderingAppSettingsFile.cpp).
    constexpr const char* kSettingsStateFile = "PBRApp.settings.ini";
    // Baked GI probe irradiance, keyed by a fingerprint of the two files above.
    constexpr const char* kGIProbeCacheFile = "PBRApp.gi_probe_cache.bin";
}
