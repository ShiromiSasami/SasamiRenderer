#pragma once

#include "AppFramework/Debug/IsoSurfaceSceneMeshes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        // Live iso-surface of the GPU smoke sim (smoke.mesh live): every frame it takes the newest density
        // copy SmokeVolumeSim streams back (SetDensityStreaming, ~3 frames late), meshes `iso` with
        // ExtractIsoSurface on the CPU and writes it into the smoke slot of an IsoSurfaceSceneMeshes model
        // (fixed capacity, vertex range update, lit and shadowed). The model's bounds are the smoke box at
        // Enable; after smoke.volume moves the box, turn it off and on again. DX12-only like the sim.
        class SmokeSurfaceOverlay
        {
        public:
            std::string Enable(ApplicationCore& app, float iso);
            void Disable(ApplicationCore& app);
            bool IsEnabled() const { return m_enabled; }
            void Tick(ApplicationCore& app);
            std::string Status() const;

        private:
            IsoSurfaceSceneMeshes m_meshes;
            std::vector<float> m_density;
            bool m_enabled = false;
            float m_iso = 0.5f;
            uint64_t m_lastRecord = UINT64_MAX;
            uint64_t m_meshedFrames = 0;
            uint32_t m_triangles = 0;
            uint32_t m_openEdges = 0;
            double m_meshMs = 0.0;
        };
    }
}

namespace SasamiEngine { using namespace App; }
