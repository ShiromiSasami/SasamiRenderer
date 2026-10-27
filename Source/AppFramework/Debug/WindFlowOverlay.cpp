#include "AppFramework/Debug/WindFlowOverlay.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Object/Camera.h"
#include "AppFramework/Debug/WindOverlayProjection.h"
#include "AppFramework/Debug/VoxelStreamOverlay.h"
#include "Renderer/Voxel/ChunkSignedDistance.h"
#include "Renderer/Voxel/SceneVoxelization.h"
#include "Renderer/Voxel/WindowPressureShift.h"
#include "Foundation/Jobs/ParallelFor.h"
#include "Renderer/Voxel/VoxelDistanceTransform.h"
#include "imgui.h"
#include "Foundation/Math/MathUtil.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        constexpr float MAX_TRAIL_FADE = 0.85f;
        // Integer widths take ImGui's baked-texture AA path (2 vertices per point); a fractional
        // width such as 1.5 takes the geometric AA path, 4 vertices per point.
        constexpr float LINE_THICKNESS = 2.0f;
        constexpr float DASH_ON = 4.0f;       // px
        constexpr float DASH_PERIOD = 7.0f;   // px
        constexpr uint32_t PROJECT_CHUNK = 256;

        using Foundation::Math::Dot3;

        // Draws a -> b as dashes of DASH_ON px every DASH_PERIOD px, starting arcLength px into the pattern.
        // Returns the arc length at b.
        float AddDashedLine(ImDrawList* drawList, const ImVec2& a, const ImVec2& b, float arcLength, ImU32 color)
        {
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            if (length <= 0.0f) {
                return arcLength;
            }
            float s = 0.0f;
            while (s < length) {
                const float phase = std::fmod(arcLength + s, DASH_PERIOD);
                if (phase < DASH_ON) {
                    const float e = (std::min)(length, s + (DASH_ON - phase));
                    drawList->AddLine(ImVec2(a.x + dx * (s / length), a.y + dy * (s / length)),
                                      ImVec2(a.x + dx * (e / length), a.y + dy * (e / length)), color, LINE_THICKNESS);
                    s = e;
                } else {
                    s += DASH_PERIOD - phase;
                }
            }
            return arcLength + length;
        }

        // Blue -> white -> yellow with speed / referenceSpeed in [0, 2], as DrawWindParticleTrails.
        void SpeedColor(float speed, float referenceSpeed, float outRgb[3])
        {
            const float s = referenceSpeed > 0.0f ? std::clamp(speed / referenceSpeed, 0.0f, 2.0f) : 1.0f;
            static const float BLUE[3] = { 40.0f, 110.0f, 255.0f };
            static const float WHITE[3] = { 255.0f, 255.0f, 255.0f };
            static const float YELLOW[3] = { 255.0f, 230.0f, 40.0f };
            const float* from = s <= 1.0f ? BLUE : WHITE;
            const float* to = s <= 1.0f ? WHITE : YELLOW;
            const float t = s <= 1.0f ? s : s - 1.0f;
            for (int c = 0; c < 3; ++c) {
                outRgb[c] = from[c] + (to[c] - from[c]) * t;
            }
        }
    }

    std::string WindFlowOverlay::Enable(ApplicationCore& app, const WindFlowOverlaySettings& settings)
    {
        const Render::Voxel::SceneVoxelization& vox = app.GetSceneVoxelization();
        // source=stream: the window is assembled before Disable so a failure keeps the old overlay.
        std::unique_ptr<Render::Voxel::StreamedVoxelWindow> window;
        int32_t centre[3] = { 0, 0, 0 };
        double assembleMs = 0.0;
        const Render::Voxel::VoxelChunkStreamer* streamer = app.GetVoxelStreamOverlay().Streamer();
        if (settings.streamWindow) {
            if (!settings.projectMode) {
                return "ERR source=stream needs mode=project";
            }
            if (streamer == nullptr) {
                return "ERR stream is off (voxel.stream on sdf=A first)";
            }
            streamer->WorldToChunk(app.GetMainCameraProxy().cameraPosition, centre);
            window = std::make_unique<Render::Voxel::StreamedVoxelWindow>();
            uint32_t missing = 0;
            std::string error;
            const auto assembleBegin = std::chrono::steady_clock::now();
            if (!Render::Voxel::AssembleStreamedVoxelWindow(*streamer, centre, streamer->Settings().loadRadius, *window, missing,
                                                            error)) {
                return "ERR " + error + " missing=" + std::to_string(missing);
            }
            assembleMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - assembleBegin).count();
        } else if (!vox.HasResult()) {
            return "ERR no voxel.scene result (run voxel.scene first)";
        }
        Disable();
        m_settings = settings;
        m_desc = window ? window->desc : vox.Desc();
        const float extent = m_desc.voxelSize * static_cast<float>((std::max)({ m_desc.dimX, m_desc.dimY, m_desc.dimZ }));
        float wind[3] = { settings.wind[0], settings.wind[1], settings.wind[2] };
        m_windSpeed = std::sqrt(Dot3(wind, wind));
        if (m_windSpeed <= 0.0f) {
            wind[0] = extent / 10.0f;
            wind[1] = 0.0f;
            wind[2] = 0.0f;
            m_windSpeed = wind[0];
        }
        for (int i = 0; i < 3; ++i) {
            m_settings.wind[i] = wind[i];
        }
        if (m_settings.turbulence < 0.0f) {
            m_settings.turbulence = settings.projectMode ? 0.0f : 0.3f;
        }

        const auto start = std::chrono::steady_clock::now();
        if (!window) {
            m_field = std::make_unique<Render::Voxel::SignedDistanceGrid>();
            Render::Voxel::BuildSignedDistanceField(vox.Grid(), *m_field);
        }

        // Same field setup as `voxel.wind run`.
        Render::Voxel::SdfWindSettings curl;
        for (int i = 0; i < 3; ++i) {
            curl.wind[i] = wind[i];
        }
        curl.turbulence = m_settings.turbulence;
        curl.rampWidth = 4.0f * m_desc.voxelSize;
        if (window) {
            // The window SDF is clamped to +-(A - 0.5) voxels; a wider ramp would never reach full speed.
            curl.rampWidth = (std::min)(4.0f, Render::Voxel::ChunkSdfLimit(streamer->Settings().sdfApronVoxels)) * m_desc.voxelSize;
        }
        curl.noiseScale = 16.0f * m_desc.voxelSize;
        m_gustSettings = curl;
        m_gustSettings.laminar = false;
        if (settings.projectMode) {
            for (int i = 0; i < 3; ++i) {
                m_projectSettings.wind[i] = wind[i];
            }
            m_projectSettings.projection.micSlabs = m_settings.micSlabs;
            if (window) {
                InstallStreamWindow(Render::Voxel::BuildProjectedWindowField(std::move(window), centre, m_projectSettings, nullptr, 0));
                m_streamAssembleMs = assembleMs;
                m_streamRebuilds = 0;
            } else {
                m_projected = std::make_unique<Render::Voxel::ProjectedWindField>(vox.Grid(), *m_field, m_desc, m_projectSettings);
                BuildGusts();
            }
        } else {
            m_curl = std::make_unique<Render::Voxel::SdfWindField>(*m_field, m_desc, curl);
        }
        const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        // One step of `voxel.wind run` at vps=2 bounds the per-frame step, so a hitch cannot tunnel through walls.
        m_maxStep = 2.0f * m_desc.voxelSize / m_windSpeed;
        m_particles.Reset(ActiveField(), m_settings.count, 1u, 40.0f * m_maxStep, 120.0f * m_maxStep, m_desc.voxelSize);
        // Per-frame steps are tiny; one trail entry per voxel of travel keeps streaks ~TRAIL_LENGTH voxels long.
        m_particles.SetTrailSpacing(m_desc.voxelSize);
        m_time = 0.0f;
        m_lastDt = 0.0f;
        m_frames = 0;
        ++m_generation;
        m_enabled = true;

        std::ostringstream out;
        out << "OK overlay=1 mode=" << (settings.projectMode ? "project" : "curl")
            << " source=" << (m_streamWindow ? "stream" : "scene") << " count=" << m_settings.count
            << " wind=" << wind[0] << "," << wind[1] << "," << wind[2] << " turb=" << m_settings.turbulence
            << " speed=" << m_settings.timeScale << " occlude=" << (m_settings.occlude ? 1 : 0)
            << " dash=" << (m_settings.dashed ? 1 : 0) << " tracers=" << (m_settings.tracers ? 1 : 0) << " buildMs=" << buildMs;
        if (m_projected) {
            const Render::Voxel::PressureProjectionStats& ps = m_projected->Stats();
            out << " iters=" << ps.iterations << " converged=" << (ps.converged ? 1 : 0) << " slabs=" << ps.micSlabs
                << " projMs=" << ps.solveMs << " preconMs=" << ps.preconditionerMs;
        }
        if (m_streamWindow) {
            out << " centre=" << m_streamCentre[0] << "," << m_streamCentre[1] << "," << m_streamCentre[2]
                << " dims=" << m_desc.dimX << "," << m_desc.dimY << "," << m_desc.dimZ << " assembleMs=" << m_streamAssembleMs;
        }
        return out.str();
    }

    void WindFlowOverlay::BuildGusts()
    {
        m_gusted.reset();
        m_gustNoise.reset();
        if (!m_projected || m_settings.turbulence <= 0.0f) {
            return;
        }
        m_gustNoise = std::make_unique<Render::Voxel::SdfWindField>(ActiveSdf(), m_desc, m_gustSettings);
        m_gustNoise->SetTime(m_time);
        m_gusted = std::make_unique<Render::Voxel::GustedWindField>(*m_projected, *m_gustNoise);
    }

    void WindFlowOverlay::InstallStreamWindow(Render::Voxel::ProjectedWindowField&& result)
    {
        // Gusts reference the old field and SDF, so they go first; the field goes before its window.
        m_gusted.reset();
        m_gustNoise.reset();
        m_projected = std::move(result.field);
        m_streamWindow = std::move(result.window);
        m_desc = m_streamWindow->desc;
        for (int a = 0; a < 3; ++a) {
            m_streamCentre[a] = result.centreChunk[a];
        }
        m_streamWarmCells = result.warmCells;
        m_streamProjectMs = result.projectMs;
        BuildGusts();
        ++m_generation;
    }

    void WindFlowOverlay::UpdateStreamWindow(ApplicationCore& app)
    {
        Render::Voxel::ProjectedWindowField result;
        if (m_async.TryTake(result)) {
            InstallStreamWindow(std::move(result));
            ++m_streamRebuilds;
            return;
        }
        const Render::Voxel::VoxelChunkStreamer* streamer = app.GetVoxelStreamOverlay().Streamer();
        if (m_async.IsBusy() || streamer == nullptr || !m_projected) {
            return;
        }
        int32_t centre[3] = { 0, 0, 0 };
        streamer->WorldToChunk(app.GetMainCameraProxy().cameraPosition, centre);
        if (centre[0] == m_streamCentre[0] && centre[1] == m_streamCentre[1] && centre[2] == m_streamCentre[2]) {
            m_streamMissing = 0;
            return;
        }
        // Chunks still loading make the assembly fail; it is retried next frame.
        auto window = std::make_unique<Render::Voxel::StreamedVoxelWindow>();
        uint32_t missing = 0;
        std::string error;
        const auto begin = std::chrono::steady_clock::now();
        if (!Render::Voxel::AssembleStreamedVoxelWindow(*streamer, centre, streamer->Settings().loadRadius, *window, missing, error)) {
            m_streamMissing = missing;
            return;
        }
        m_streamMissing = 0;
        std::vector<float> warm;
        const uint64_t warmCells =
            Render::Voxel::ShiftWindowPressure(m_projected->Pressure(), m_streamWindow->region, window->region, warm);
        m_streamAssembleMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        m_async.Kick(std::move(window), centre, m_projectSettings, std::move(warm), warmCells);
    }

    void WindFlowOverlay::Disable()
    {
        if (m_enabled) {
            ++m_generation;
        }
        m_enabled = false;
        m_lastDt = 0.0f;
        m_async.Cancel();
        m_gusted.reset();
        m_gustNoise.reset();
        m_projected.reset();
        m_curl.reset();
        m_field.reset();
        m_streamWindow.reset();
        m_streamMissing = 0;
        m_streamWarmCells = 0;
        m_particles = Render::Voxel::WindParticleSystem();
    }

    std::string WindFlowOverlay::Status() const
    {
        std::ostringstream out;
        out << "OK overlay=" << (m_enabled ? 1 : 0);
        if (m_enabled) {
            out << " mode=" << (m_projected ? "project" : "curl") << " count=" << m_particles.Count()
                << " frames=" << m_frames << " simTime=" << m_time << " visible=" << m_lastVisible
                << " stepMs=" << m_lastStepMs << " drawMs=" << m_lastDrawMs << " vertices=" << m_lastVertices;
            if (m_streamWindow) {
                const Render::Voxel::PressureProjectionStats& ps = m_projected->Stats();
                out << " source=stream centre=" << m_streamCentre[0] << "," << m_streamCentre[1] << "," << m_streamCentre[2]
                    << " dims=" << m_desc.dimX << "," << m_desc.dimY << "," << m_desc.dimZ << " rebuilds=" << m_streamRebuilds
                    << " pending=" << (m_async.IsBusy() ? 1 : 0) << " missing=" << m_streamMissing
                    << " assembleMs=" << m_streamAssembleMs << " projectMs=" << m_streamProjectMs << " iters=" << ps.iterations
                    << " converged=" << (ps.converged ? 1 : 0) << " warmCells=" << m_streamWarmCells;
            }
        }
        return out.str();
    }

    const Render::Voxel::SignedDistanceGrid& WindFlowOverlay::ActiveSdf() const
    {
        return m_streamWindow ? m_streamWindow->sdf : *m_field;
    }

    const Render::Voxel::IWindVelocityField& WindFlowOverlay::ActiveField() const
    {
        if (m_gusted) {
            return *m_gusted;
        }
        if (m_projected) {
            return *m_projected;
        }
        return *m_curl;
    }

    void WindFlowOverlay::Draw(ApplicationCore& app)
    {
        if (!m_enabled) {
            return;
        }
        Camera* camera = app.GetMainCamera();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        if (!camera || viewport->Size.x <= 0.0f || viewport->Size.y <= 0.0f) {
            return;
        }

        if (m_streamWindow) {
            UpdateStreamWindow(app);
        }
        const auto stepStart = std::chrono::steady_clock::now();
        const float dt = (std::min)(ImGui::GetIO().DeltaTime * m_settings.timeScale, m_maxStep);
        if (dt > 0.0f) {
            m_time += dt;
            if (m_gustNoise) {
                m_gustNoise->SetTime(m_time);
            } else if (m_curl) {
                m_curl->SetTime(m_time);
            }
            if (m_settings.tracers) {
                m_particles.Step(ActiveField(), dt);
            }
        }
        m_lastDt = (std::max)(dt, 0.0f);
        const auto drawStart = std::chrono::steady_clock::now();

        const Render::RenderCameraProxy proxy = camera->BuildRenderCameraProxy(viewport->Size.x, viewport->Size.y);
        if (!m_settings.tracers) {
            m_lastVisible = 0;
            m_lastStepMs = std::chrono::duration<double, std::milli>(drawStart - stepStart).count();
            m_lastDrawMs = 0.0;
            ++m_frames;
            return;
        }
        // Projection and the occlusion rays (the expensive part) run in parallel; ImGui only takes
        // draw calls from one thread, so the lines are emitted afterwards in particle order.
        constexpr uint32_t trailLength = Render::Voxel::WindParticleSystem::TRAIL_LENGTH;
        const uint32_t count = m_particles.Count();
        m_screenXy.resize(static_cast<size_t>(count) * trailLength * 2);
        m_screenPoints.assign(count, 0);
        const ImVec2 viewPos = viewport->Pos;
        const ImVec2 viewSize = viewport->Size;
        Foundation::ParallelFor((count + PROJECT_CHUNK - 1) / PROJECT_CHUNK, [&](uint32_t chunk) {
            const uint32_t begin = chunk * PROJECT_CHUNK;
            const uint32_t chunkEnd = (std::min)(begin + PROJECT_CHUNK, count);
            for (uint32_t i = begin; i < chunkEnd; ++i) {
                const uint32_t trailCount = m_particles.TrailCount(i);
                if (trailCount < 2) {
                    continue;
                }
                float* xy = &m_screenXy[static_cast<size_t>(i) * trailLength * 2];
                const float* head = m_particles.TrailPosition(i, 0);
                ImVec2 a;
                if (!ProjectToOverlay(proxy, viewPos, viewSize, head, a)) {
                    continue;
                }
                if (m_settings.occlude && IsOccludedBySdf(ActiveSdf(), m_desc, proxy.cameraPosition, head)) {
                    continue;
                }
                xy[0] = a.x;
                xy[1] = a.y;
                uint32_t points = 1;
                for (; points < trailCount; ++points) {
                    ImVec2 b;
                    if (!ProjectToOverlay(proxy, viewPos, viewSize, m_particles.TrailPosition(i, points), b)) {
                        break;
                    }
                    xy[points * 2] = b.x;
                    xy[points * 2 + 1] = b.y;
                }
                m_screenPoints[i] = static_cast<uint8_t>(points);
            }
        });

        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        const int vertexStart = drawList->VtxBuffer.Size;
        uint32_t visible = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t points = m_screenPoints[i];
            if (points == 0) {
                continue;
            }
            ++visible;
            const float* xy = &m_screenXy[static_cast<size_t>(i) * trailLength * 2];
            float rgb[3];
            SpeedColor(m_particles.Speed(i), m_windSpeed, rgb);
            // Screen-space distance from the head; the dash pattern is anchored to the particle, so it moves with it.
            float arcLength = 0.0f;
            ImVec2 a(xy[0], xy[1]);
            for (uint32_t k = 0; k + 1 < points; ++k) {
                const ImVec2 b(xy[(k + 1) * 2], xy[(k + 1) * 2 + 1]);
                const float alpha = 1.0f - (static_cast<float>(k) / static_cast<float>(trailLength)) * MAX_TRAIL_FADE;
                const ImU32 color = IM_COL32(static_cast<int>(rgb[0]), static_cast<int>(rgb[1]), static_cast<int>(rgb[2]),
                                             static_cast<int>(alpha * 255.0f));
                if (m_settings.dashed) {
                    arcLength = AddDashedLine(drawList, a, b, arcLength, color);
                } else {
                    drawList->AddLine(a, b, color, LINE_THICKNESS);
                }
                a = b;
            }
        }
        m_lastVertices = static_cast<uint32_t>(drawList->VtxBuffer.Size - vertexStart);
        const auto end = std::chrono::steady_clock::now();
        m_lastVisible = visible;
        m_lastStepMs = std::chrono::duration<double, std::milli>(drawStart - stepStart).count();
        m_lastDrawMs = std::chrono::duration<double, std::milli>(end - drawStart).count();
        ++m_frames;
    }
}
