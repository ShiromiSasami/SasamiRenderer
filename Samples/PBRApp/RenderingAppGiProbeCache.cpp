// The GI probe cache's on-disk lifecycle: the fingerprint that decides whether a cached bake
// still matches the current scene and settings, the write of a fresh bake, and the restore that
// either loads the cache or asks for a new bake once the scene and the renderer are ready.
//
// Separate from the settings file because its validity key is derived from *both* saved files
// rather than stored in either, and separate from the session-restore sequence because that
// sequence only has to know whether the restore can run yet, not what makes a cache stale.

#include "RenderingApp.h"
#include "RenderingAppProbeGrid.h"
#include "RenderingAppSessionFiles.h"
#include "ApplicationCore.h"
#include "ApplicationResourcePaths.h"
#include "Object/StaticModel.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Runtime/Renderer.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

namespace SasamiEngine::Sample::PBR
{
    namespace
    {
        uint64_t Fnva1Mix(uint64_t hash, const void* data, size_t size)
        {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; ++i) {
                hash ^= static_cast<uint64_t>(bytes[i]);
                hash *= 1099511628211ull;
            }
            return hash;
        }

        uint64_t Fnva1MixString(uint64_t hash, const char* text)
        {
            return Fnva1Mix(hash, text, std::strlen(text));
        }

        uint64_t Fnva1MixFile(uint64_t hash, const std::string& path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) {
                return Fnva1MixString(hash, "<missing>");
            }

            std::array<char, 4096> buffer{};
            while (in.good()) {
                in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize readCount = in.gcount();
                if (readCount > 0) {
                    hash = Fnva1Mix(hash, buffer.data(), static_cast<size_t>(readCount));
                }
            }
            return hash;
        }

        // A cached bake is only valid for the exact scene and settings it was baked from, so the
        // key is the content of both saved files rather than a version number anyone has to bump.
        uint64_t ComputeSessionStateHash(const std::string& scenePath,
                                         const std::string& settingsPath)
        {
            uint64_t hash = 14695981039346656037ull;
            hash = Fnva1MixString(hash, "scene:");
            hash = Fnva1MixFile(hash, scenePath);
            hash = Fnva1MixString(hash, "|settings:");
            hash = Fnva1MixFile(hash, settingsPath);
            return hash;
        }
    }

    void RenderingApp::PersistGiProbeCache(ApplicationCore& app) const
    {
        const std::string scenePath =
            ApplicationResourcePaths::ResolveConfigPathString(kSceneStateFile);
        const std::string settingsPath =
            ApplicationResourcePaths::ResolveConfigPathString(kSettingsStateFile);
        const std::string cachePath =
            ApplicationResourcePaths::ResolveConfigPathString(kGIProbeCacheFile);

        const uint64_t stateHash = ComputeSessionStateHash(scenePath, settingsPath);
        Renderer& renderer = app.GetRenderer();
        if (renderer.HasGIProbeData() && !renderer.SaveGIProbeCache(cachePath, stateHash)) {
            DebugLog("PBRApp: failed to save GI probe cache.\n");
        }
    }

    bool RenderingApp::IsGiSessionRestoreReady(ApplicationCore& app) const
    {
        // A model that was never created reads as "settled": a failed load must not stall the
        // restore forever. That is also why ApplyDeferredGiSessionState can still report failure
        // below - this gate can go true while the scene is genuinely empty.
        const RendererReadyState& readyState = app.GetRenderer().GetReadyState();
        const bool sponzaSettled = !m_sponzaModel ||
            m_sponzaModel->GetLoadState() != MeshComponent::MeshLoadState::Loading;
        const bool bunnySettled = !m_bunnyModel ||
            m_bunnyModel->GetLoadState() != MeshComponent::MeshLoadState::Loading;
        const bool bistroSettled = !m_bistroModel ||
            m_bistroModel->GetLoadState() != MeshComponent::MeshLoadState::Loading;
        return readyState.IsFeatureReady(readyState.giReady) &&
               readyState.IsFeatureReady(readyState.swrtReady) &&
               sponzaSettled && bunnySettled && bistroSettled;
    }

    bool RenderingApp::ApplyDeferredGiSessionState(ApplicationCore& app)
    {
        // The model "settled" checks the callers use read true for a model that has not been
        // created yet, so they can fire before any geometry exists. When that happens Scene Auto
        // has no bounds to fit and falls back to the Sponza-sized box -- which is how the grid
        // ended up covering ~30x20x44m of a ~196x60x193m scene, leaving Bistro outside it.
        // Reporting failure lets the caller keep retrying until the bounds are real.
        if (!ApplyProbeGridPreset(app, m_probeGridPreset)) {
            return false;
        }

        if (m_sessionGiBaked) {
            Renderer& renderer = app.GetRenderer();
            const std::string scenePath =
                ApplicationResourcePaths::ResolveConfigPathString(kSceneStateFile);
            const std::string settingsPath =
                ApplicationResourcePaths::ResolveConfigPathString(kSettingsStateFile);
            const std::string cachePath =
                ApplicationResourcePaths::ResolveConfigPathString(kGIProbeCacheFile);
            const uint64_t stateHash = ComputeSessionStateHash(scenePath, settingsPath);
            if (!renderer.LoadGIProbeCache(cachePath, stateHash) && renderer.GetGIEnabled()) {
                renderer.RequestGIBake();
            }
        }
        return true;
    }
}
