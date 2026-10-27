#include "AppFramework/Debug/VoxelOccupancyViewer.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Debug/WindEffectsOverlay.h"
#include "AppFramework/Debug/WindFlowOverlay.h"
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Voxel/SceneVoxelization.h"
#include "UI/UIButton.h"
#include "UI/UIIntVolume.h"
#include "UI/UIText.h"
#include "imgui.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <exception>
#include <string>

namespace SasamiEngine::App::Debug
{
    void VoxelOccupancyViewer::SetView(Render::Voxel::VoxelAxis axis,
                                       Render::Voxel::OccupancyImageMode mode,
                                       uint32_t sliceIndex)
    {
        m_axis = axis;
        m_mode = mode;
        m_sliceIndex = static_cast<int>(std::min<uint32_t>(sliceIndex, static_cast<uint32_t>(INT_MAX)));
    }

    void VoxelOccupancyViewer::RefreshImage(ApplicationCore& app)
    {
        const Render::Voxel::SceneVoxelization& vox = app.GetSceneVoxelization();
        if (!vox.HasResult()) {
            return;
        }
        if (m_imageGeneration == vox.Generation() && m_imageAxis == m_axis && m_imageMode == m_mode &&
            (m_mode == Render::Voxel::OccupancyImageMode::Projection || m_imageSlice == m_sliceIndex)) {
            return;
        }
        m_image = Render::Voxel::BuildOccupancyImage(
            vox.Grid(), m_axis, m_mode, static_cast<uint32_t>((std::max)(0, m_sliceIndex)), MAX_DISPLAY_SIDE);
        m_imageGeneration = vox.Generation();
        m_imageAxis = m_axis;
        m_imageMode = m_mode;
        m_imageSlice = m_sliceIndex;
    }

    void VoxelOccupancyViewer::Draw(ApplicationCore& app)
    {
        if (!m_visible) {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(420, 560), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Voxel Occupancy", &m_visible)) {
            DrawContents(app);
        }
        ImGui::End();
    }

    void VoxelOccupancyViewer::DrawContents(ApplicationCore& app)
    {
        UI::IntVolume("Max dim", &m_maxDim, 8, 512);
        ImGui::Checkbox("Fill interior (parity)", &m_fillInterior);
        if (UI::Button("Voxelize scene")) {
            IRHIDevice* device = app.GetRenderer().GetRhiDevice();
            if (!device) {
                m_lastError = "no RHI device";
            } else {
                try {
                    std::string error;
                    if (!app.GetSceneVoxelization().Run(*device,
                                                        app.GetRenderer().GetSceneSubmitter(),
                                                        static_cast<uint32_t>(m_maxDim),
                                                        m_fillInterior ? Render::Voxel::VoxelFillMode::Solid
                                                                       : Render::Voxel::VoxelFillMode::Surface,
                                                        false,
                                                        error)) {
                        m_lastError = error;
                    } else {
                        m_lastError.clear();
                    }
                } catch (std::exception& e) {
                    m_lastError = e.what();
                }
            }
        }
        UI::TextDisabled("Opaque static meshes only. GPU voxelizer is DX12-only; blocks until GPU idle.");
        WindFlowOverlay& windOverlay = app.GetWindFlowOverlay();
        bool windOn = windOverlay.IsEnabled();
        if (ImGui::Checkbox("Wind overlay (voxel.wind.overlay)", &windOn)) {
            if (windOn) {
                const std::string result = windOverlay.Enable(app, WindFlowOverlaySettings{});
                if (result.rfind("ERR", 0) == 0) {
                    m_lastError = result;
                }
            } else {
                windOverlay.Disable();
            }
        }
        app.GetWindEffectsOverlay().DrawControls(app);

        if (!m_lastError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error: %s", m_lastError.c_str());
        }

        const Render::Voxel::SceneVoxelization& vox = app.GetSceneVoxelization();
        if (!vox.HasResult()) {
            UI::TextDisabled("No voxelization yet.");
            return;
        }

        const Render::Voxel::OccupancyBitGrid& grid = vox.Grid();
        const Render::Voxel::SceneVoxelizationStats& stats = vox.Stats();
        const double voxelCount = static_cast<double>(grid.VoxelCount());
        const double occupiedPercent = voxelCount > 0.0 ? 100.0 * static_cast<double>(stats.occupiedVoxels) / voxelCount : 0.0;

        UI::Text("Grid %u x %u x %u  voxel %.4f", grid.DimX(), grid.DimY(), grid.DimZ(), vox.Desc().voxelSize);
        UI::Text("Occupied %llu (%.2f%%)", static_cast<unsigned long long>(stats.occupiedVoxels), occupiedPercent);
        UI::Text("Memory %.1f KB (1 bit/voxel)", static_cast<double>(grid.SizeInBytes()) / 1024.0);
        UI::Text("Triangles %llu  items %u  skipped %u",
                 static_cast<unsigned long long>(stats.triangleCount),
                 stats.drawItemsUsed,
                 stats.drawItemsSkipped);
        UI::Text("Gather %.2f ms  GPU %.2f ms", stats.gatherMs, stats.gpuMs);
        if (stats.fillMode == Render::Voxel::VoxelFillMode::Solid) {
            UI::Text("Surface %llu  interior %llu  solid GPU %.2f ms",
                     static_cast<unsigned long long>(stats.surfaceVoxels),
                     static_cast<unsigned long long>(stats.interiorVoxels),
                     stats.solidGpuMs);
            UI::Text("Leak rows %llu (odd parity at +X end; holes in meshes)",
                     static_cast<unsigned long long>(stats.leakRows));
        }

        ImGui::Separator();

        int axis = static_cast<int>(m_axis);
        ImGui::RadioButton("X", &axis, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Y", &axis, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Z", &axis, 2);
        m_axis = static_cast<Render::Voxel::VoxelAxis>(axis);

        int mode = static_cast<int>(m_mode);
        ImGui::RadioButton("Projection", &mode, static_cast<int>(Render::Voxel::OccupancyImageMode::Projection));
        ImGui::SameLine();
        ImGui::RadioButton("Slice", &mode, static_cast<int>(Render::Voxel::OccupancyImageMode::Slice));
        m_mode = static_cast<Render::Voxel::OccupancyImageMode>(mode);

        if (m_mode == Render::Voxel::OccupancyImageMode::Slice) {
            const int maxSlice = (std::max)(0, static_cast<int>(Render::Voxel::AxisDim(grid, m_axis)) - 1);
            m_sliceIndex = std::clamp(m_sliceIndex, 0, maxSlice);
            ImGui::SliderInt("Slice", &m_sliceIndex, 0, maxSlice);
        }

        RefreshImage(app);

        UI::Text("Image %ux%u (source %ux%u, 1/%u downsample)",
                 m_image.width,
                 m_image.height,
                 m_image.sourceWidth,
                 m_image.sourceHeight,
                 m_image.downsample);

        if (m_image.width == 0 || m_image.height == 0) {
            return;
        }

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float maxH = (std::max)(256.0f, (std::min)(avail.y, 512.0f));
        float cell = (std::max)(1.0f, std::floor(avail.x / static_cast<float>(m_image.width)));
        cell = (std::min)(cell, (std::max)(1.0f, std::floor(maxH / static_cast<float>(m_image.height))));

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 canvasSize(static_cast<float>(m_image.width) * cell, static_cast<float>(m_image.height) * cell);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, ImVec2(origin.x + canvasSize.x, origin.y + canvasSize.y), IM_COL32(20, 20, 24, 255));
        drawList->AddRect(origin, ImVec2(origin.x + canvasSize.x, origin.y + canvasSize.y), IM_COL32(90, 90, 90, 255));

        for (uint32_t row = 0; row < m_image.height; ++row) {
            const uint8_t* line = m_image.values.data() + static_cast<size_t>(row) * m_image.width;
            const float y0 = origin.y + static_cast<float>(row) * cell;
            uint32_t col = 0;
            while (col < m_image.width) {
                const uint8_t value = line[col];
                if (value == 0) {
                    ++col;
                    continue;
                }
                uint32_t end = col + 1;
                while (end < m_image.width && line[end] == value) {
                    ++end;
                }
                const int g = 60 + static_cast<int>(value) * 195 / 255;
                const ImU32 color = IM_COL32(g, static_cast<int>(g * 0.85f), static_cast<int>(g * 0.55f), 255);
                drawList->AddRectFilled(ImVec2(origin.x + static_cast<float>(col) * cell, y0),
                                        ImVec2(origin.x + static_cast<float>(end) * cell, y0 + cell),
                                        color);
                col = end;
            }
        }
        ImGui::Dummy(canvasSize);
    }
}
