#pragma once

#include "Renderer/Voxel/VoxelChunkStreamer.h"

#include <memory>
#include <string>

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        // voxel.stream (6b stage 1, Discord 2026-10-22): owns a VoxelChunkStreamer, ticks it from the
        // main camera every frame and draws the resident chunk boxes as ImGui background lines
        // (green = loaded, yellow = loading). Main thread only.
        class VoxelStreamOverlay
        {
        public:
            VoxelStreamOverlay();
            ~VoxelStreamOverlay();

            // Returns "OK ..." or "ERR ...".
            std::string Start(ApplicationCore& app, const Render::Voxel::VoxelChunkStreamerSettings& settings);
            std::string Stop();
            std::string Status() const;
            void SetDrawBoxes(bool on) { m_drawBoxes = on; }
            // voxel.stream window (6b stage 3a): assembles the loaded chunks within loadRadius of the
            // camera chunk into one grid (StreamedVoxelWindow) and runs a synchronous wind projection
            // on it (floor wall only when the window touches the bottom chunk row, other faces open).
            // Needs sdf=A. Reports the timings and the solver stats; the field is not kept.
            std::string ProjectWindow(ApplicationCore& app, const float wind[3], uint32_t micSlabs) const;
            bool IsRunning() const;
            // The running streamer, or null (WindFlowOverlay source=stream reads its chunks).
            const Render::Voxel::VoxelChunkStreamer* Streamer() const { return IsRunning() ? m_streamer.get() : nullptr; }

            // Once per frame after the debug commands: streams around the main camera.
            void Tick(ApplicationCore& app);
            // ImGui overlay callback.
            void Draw(ApplicationCore& app);

        private:
            std::unique_ptr<Render::Voxel::VoxelChunkStreamer> m_streamer;
            bool m_drawBoxes = true;
            double m_lastTickMs = 0.0;
        };
    }
}
namespace SasamiEngine { using namespace App; }
