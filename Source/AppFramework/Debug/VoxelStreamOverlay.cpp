#include "AppFramework/Debug/VoxelStreamOverlay.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Debug/WindOverlayProjection.h"
#include "AppFramework/Object/Camera.h"
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Voxel/ProjectedWindField.h"
#include "Renderer/Voxel/StreamedVoxelWindow.h"
#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        constexpr float BOX_THICKNESS = 1.0f;
        constexpr ImU32 LOADED_COLOR = IM_COL32(60, 220, 90, 110);
        constexpr ImU32 LOADING_COLOR = IM_COL32(240, 210, 40, 200);

        // Corner i: bit 0 = x, bit 1 = y, bit 2 = z (set = max side).
        constexpr int BOX_EDGES[12][2] = {
            { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },
            { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
            { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
        };
    }

    VoxelStreamOverlay::VoxelStreamOverlay() = default;
    VoxelStreamOverlay::~VoxelStreamOverlay() = default;

    bool VoxelStreamOverlay::IsRunning() const
    {
        return m_streamer && m_streamer->IsRunning();
    }

    std::string VoxelStreamOverlay::Start(ApplicationCore& app, const Render::Voxel::VoxelChunkStreamerSettings& settings)
    {
        auto& submitter = app.GetRenderer().GetSceneSubmitter();
        if (!m_streamer) {
            m_streamer = std::make_unique<Render::Voxel::VoxelChunkStreamer>();
        }
        std::string error;
        if (!m_streamer->Start(submitter.GetDrawItems(), submitter.GetMeshes(), settings, error)) {
            return "ERR " + error;
        }
        const Render::Voxel::VoxelChunkStreamerStats& stats = m_streamer->Stats();
        const Render::Voxel::VoxelGridDesc& desc = m_streamer->AnchorDesc();
        std::ostringstream out;
        out << "OK stream=1 chunks=" << stats.chunkCountX << "," << stats.chunkCountY << "," << stats.chunkCountZ
            << " dims=" << desc.dimX << "," << desc.dimY << "," << desc.dimZ
            << " voxel=" << desc.voxelSize << " chunk=" << settings.chunkVoxels
            << " radius=" << settings.loadRadius << " sdf=" << settings.sdfApronVoxels << " triangles=" << stats.triangles;
        return out.str();
    }

    std::string VoxelStreamOverlay::Stop()
    {
        if (m_streamer) {
            m_streamer->Stop();
        }
        return "OK stream=0";
    }

    std::string VoxelStreamOverlay::Status() const
    {
        if (!IsRunning()) {
            return "OK stream=0";
        }
        const Render::Voxel::VoxelChunkStreamerStats& s = m_streamer->Stats();
        std::ostringstream out;
        out << std::fixed << std::setprecision(2);
        out << "OK stream=1 chunks=" << s.chunkCountX << "," << s.chunkCountY << "," << s.chunkCountZ
            << " cam=" << s.cameraChunk[0] << "," << s.cameraChunk[1] << "," << s.cameraChunk[2]
            << " wanted=" << s.wanted << " loaded=" << s.loaded << " loading=" << s.loading
            << " loads=" << s.loadsCompleted << " unloads=" << s.unloadsCompleted
            << " discarded=" << s.discardedResults << " residentKB=" << (s.residentBytes / 1024)
            << " sdfKB=" << (s.residentSdfBytes / 1024)
            << " lastJobMs=" << s.lastJobMs << " maxJobMs=" << s.maxJobMs << " tickMs=" << m_lastTickMs;
        return out.str();
    }

    std::string VoxelStreamOverlay::ProjectWindow(ApplicationCore& app, const float wind[3], uint32_t micSlabs) const
    {
        if (!IsRunning()) {
            return "ERR stream is off (voxel.stream on sdf=A first)";
        }
        if (app.GetMainCamera() == nullptr) {
            return "ERR no main camera";
        }
        int32_t centre[3] = { 0, 0, 0 };
        m_streamer->WorldToChunk(app.GetMainCameraProxy().cameraPosition, centre);

        using Clock = std::chrono::steady_clock;
        const auto assembleBegin = Clock::now();
        Render::Voxel::StreamedVoxelWindow window;
        uint32_t missing = 0;
        std::string error;
        if (!Render::Voxel::AssembleStreamedVoxelWindow(*m_streamer, centre, m_streamer->Settings().loadRadius, window,
                                                        missing, error)) {
            return "ERR " + error + " missing=" + std::to_string(missing);
        }
        const std::chrono::duration<double, std::milli> assembleMs = Clock::now() - assembleBegin;

        Render::Voxel::ProjectedWindSettings projected;
        for (int a = 0; a < 3; ++a) {
            projected.wind[a] = wind[a];
        }
        projected.projection.micSlabs = micSlabs;
        // Only the bottom chunk row holds the scene floor; elsewhere the lower face is open air (6a-4(d)).
        projected.projection.solidFloor = window.chunkLo[1] == 0;
        const auto projectBegin = Clock::now();
        const Render::Voxel::ProjectedWindField field(window.occupancy, window.sdf, window.desc, projected);
        const std::chrono::duration<double, std::milli> projectMs = Clock::now() - projectBegin;

        const Render::Voxel::PressureProjectionStats& ps = field.Stats();
        std::ostringstream out;
        out << std::fixed << std::setprecision(2);
        out << (ps.valid ? "OK" : "ERR") << " window cam=" << centre[0] << "," << centre[1] << "," << centre[2]
            << " chunks=" << window.chunkLo[0] << ".." << window.chunkHi[0] << "," << window.chunkLo[1] << ".."
            << window.chunkHi[1] << "," << window.chunkLo[2] << ".." << window.chunkHi[2]
            << " dims=" << window.desc.dimX << "," << window.desc.dimY << "," << window.desc.dimZ
            << " floor=" << (projected.projection.solidFloor ? 1 : 0)
            << " assembleMs=" << assembleMs.count() << " projectMs=" << projectMs.count()
            << " solveMs=" << ps.solveMs << " iters=" << ps.iterations << " converged=" << (ps.converged ? 1 : 0)
            << " slabs=" << ps.micSlabs << " fluid=" << ps.fluidCells << " solid=" << ps.solidCells
            << " enclosed=" << ps.enclosedCells;
        out << std::setprecision(4) << " divIn=" << ps.initialMaxDivergence << " divOut=" << ps.finalMaxDivergence;
        return out.str();
    }

    void VoxelStreamOverlay::Tick(ApplicationCore& app)
    {
        if (!IsRunning()) {
            return;
        }
        if (app.GetMainCamera() == nullptr) {
            return;
        }
        // The proxy is rebuilt after Tick, so the position lags one frame.
        const auto begin = std::chrono::steady_clock::now();
        m_streamer->Tick(app.GetMainCameraProxy().cameraPosition);
        const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - begin;
        m_lastTickMs = elapsed.count();
    }

    void VoxelStreamOverlay::Draw(ApplicationCore& app)
    {
        if (!IsRunning() || !m_drawBoxes) {
            return;
        }
        Camera* camera = app.GetMainCamera();
        if (camera == nullptr) {
            return;
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const Render::RenderCameraProxy proxy = camera->BuildRenderCameraProxy(viewport->Size.x, viewport->Size.y);
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();

        const Render::Voxel::VoxelGridDesc& desc = m_streamer->AnchorDesc();
        const Render::Voxel::VoxelChunkStreamerStats& stats = m_streamer->Stats();
        const int32_t radius = static_cast<int32_t>(m_streamer->Settings().loadRadius);
        const float vs = desc.voxelSize;

        m_streamer->ForEachChunk([&](const Render::Voxel::VoxelChunkStreamer::ChunkView& chunk) {
            for (int a = 0; a < 3; ++a) {
                if (std::abs(static_cast<int32_t>(chunk.coord[a]) - stats.cameraChunk[a]) > radius) {
                    return;
                }
            }
            const Render::Voxel::VoxelRegion region = m_streamer->ChunkRegion(chunk.coord[0], chunk.coord[1], chunk.coord[2]);
            if (region.IsEmpty()) {
                return;
            }
            float lo[3];
            float hi[3];
            for (int a = 0; a < 3; ++a) {
                lo[a] = desc.origin[a] + static_cast<float>(region.min[a]) * vs;
                hi[a] = desc.origin[a] + static_cast<float>(region.min[a] + region.size[a]) * vs;
            }
            ImVec2 screen[8];
            bool visible[8];
            for (int i = 0; i < 8; ++i) {
                const float corner[3] = { (i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2] };
                visible[i] = ProjectToOverlay(proxy, viewport->Pos, viewport->Size, corner, screen[i]);
            }
            const ImU32 color = chunk.state == Render::Voxel::VoxelChunkState::Loaded ? LOADED_COLOR : LOADING_COLOR;
            for (const auto& edge : BOX_EDGES) {
                if (visible[edge[0]] && visible[edge[1]]) {
                    drawList->AddLine(screen[edge[0]], screen[edge[1]], color, BOX_THICKNESS);
                }
            }
        });

        char text[96];
        std::snprintf(text, sizeof(text), "stream loaded=%u loading=%u", stats.loaded, stats.loading);
        drawList->AddText(ImVec2(viewport->Pos.x + 10.0f, viewport->Pos.y + viewport->Size.y - 24.0f),
                          IM_COL32(255, 255, 255, 220), text);
    }
}
