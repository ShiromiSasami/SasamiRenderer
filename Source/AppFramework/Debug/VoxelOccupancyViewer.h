#pragma once

#include "Renderer/Voxel/OccupancyImage.h"

#include <cstdint>
#include <string>

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        // "Voxel Occupancy" ImGui window: runs the scene voxelization (ApplicationCore::
        // GetSceneVoxelization) and shows the latest grid as a slice or a projection along one
        // axis. The picture is drawn as filled rectangles through the window's ImDrawList, so it
        // needs no texture upload and works on every UI render backend. To keep one window's
        // vertex count under the 16-bit index limit the image is shrunk to at most
        // MAX_DISPLAY_SIDE per side (BuildOccupancyImage keeps the block maximum) and
        // horizontal runs of equal value are merged into one rectangle.
        //
        // Draw is meant to be registered as an ImGuiCoordinator overlay, so the window's own
        // visibility flag (voxel.view on|off) is the only thing that hides it. PBRApp shows
        // DrawContents in its "Voxel" tab instead. Main thread only.
        class VoxelOccupancyViewer
        {
        public:
            static constexpr uint32_t MAX_DISPLAY_SIDE = 128;

            bool IsVisible() const { return m_visible; }
            void SetVisible(bool visible) { m_visible = visible; }

            // Selects what the window shows; sliceIndex is clamped when the image is built.
            void SetView(Render::Voxel::VoxelAxis axis, Render::Voxel::OccupancyImageMode mode, uint32_t sliceIndex);

            // Draws the window when visible. Its "Voxelize scene" button runs the voxelization
            // synchronously (blocks until the GPU is idle).
            void Draw(ApplicationCore& app);

            // Draws the same controls and picture into the current ImGui window, so a host app
            // can show them inside one of its own tabs instead of the separate window.
            void DrawContents(ApplicationCore& app);

        private:
            // Rebuilds m_image when the grid generation or the view selection changed.
            void RefreshImage(ApplicationCore& app);

            bool m_visible = false;
            int m_maxDim = 128;
            // Solid fill mode for the next "Voxelize scene" run (surface OR parity interior).
            bool m_fillInterior = false;
            Render::Voxel::VoxelAxis m_axis = Render::Voxel::VoxelAxis::Y;
            Render::Voxel::OccupancyImageMode m_mode = Render::Voxel::OccupancyImageMode::Projection;
            int m_sliceIndex = 0;
            std::string m_lastError;

            Render::Voxel::OccupancyImage m_image;
            // Key of m_image; generation 0 means it was never built.
            uint64_t m_imageGeneration = 0;
            Render::Voxel::VoxelAxis m_imageAxis = Render::Voxel::VoxelAxis::Y;
            Render::Voxel::OccupancyImageMode m_imageMode = Render::Voxel::OccupancyImageMode::Projection;
            int m_imageSlice = -1;
        };
    }
}
namespace SasamiEngine { using namespace App; }
