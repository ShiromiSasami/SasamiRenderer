#pragma once

#include <atomic>

namespace SasamiRenderer
{
    // Per-feature readiness flags for progressive initialization. Init tasks publish
    // readiness by storing true AFTER the feature's GPU objects are fully assigned
    // (release ordering); the frame graph acquire-loads a flag before touching the
    // feature's pipelines, so a true flag guarantees the objects are visible.
    struct RendererReadyState
    {
        std::atomic<bool> coreReady{false};        // device, backbuffers, minimal descriptors
        std::atomic<bool> rasterReady{false};      // minimal raster shader/PSO set (opaque+deferred+sky+tonemap)
        std::atomic<bool> shadowReady{false};
        std::atomic<bool> transparencyReady{false};
        std::atomic<bool> ssaoReady{false};
        std::atomic<bool> ssrReady{false};
        std::atomic<bool> skyboxIblReady{false};   // HDR environment + IBL uploaded (procedural sky until then)
        std::atomic<bool> cloudReady{false};
        std::atomic<bool> tessellationReady{false};
        std::atomic<bool> meshShaderReady{false};
        std::atomic<bool> swrtReady{false};
        std::atomic<bool> dxrReady{false};
        std::atomic<bool> giReady{false};
        std::atomic<bool> fluidReady{false};
        std::atomic<bool> particlesReady{false};
        std::atomic<bool> allReady{false};         // every deferred init task completed

        bool IsFeatureReady(const std::atomic<bool>& flag) const
        {
            return flag.load(std::memory_order_acquire);
        }
    };
}
