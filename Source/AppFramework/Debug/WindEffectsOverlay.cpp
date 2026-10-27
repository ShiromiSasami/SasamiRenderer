#include "AppFramework/Debug/WindEffectsOverlay.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Debug/IsoSurfaceOverlayDraw.h"
#include "AppFramework/Debug/WindFlowOverlay.h"
#include "AppFramework/Debug/WindOverlayProjection.h"
#include "AppFramework/Object/Camera.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Voxel/FlameColor.h"
#include "Renderer/Voxel/GpuWindParticleStepper.h"
#include "Renderer/Voxel/GpuWindParticleStream.h"
#include "Renderer/Voxel/ProjectedWindField.h"
#include "Renderer/Voxel/SceneVoxelization.h"
#include "Renderer/Voxel/SdfSphereTracer.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"
#include "UI/UIButton.h"
#include "UI/UIText.h"
#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        using Render::Voxel::WindDrivenParticles;
        using Render::Voxel::WindDrivenParticleSettings;
        using Render::Voxel::WindParticleKind;

        constexpr float SMOKE_MAX_ALPHA = 0.35f;
        constexpr float MIN_PETAL_SIZE = 0.08f;
        constexpr uint32_t FLAME_MAX_PARTICLES = 6000;
        constexpr uint32_t EMBER_MAX_PARTICLES = 800;
        constexpr float FLAMES_PER_CELL_PER_SECOND = 12.0f;
        constexpr float EMBERS_PER_CELL_PER_SECOND = 0.4f;

        // Set at the start of Draw, read by the draw helpers.
        struct FrameView
        {
            Render::RenderCameraProxy proxy;
            ImVec2 origin;
            ImVec2 size;
            ImDrawList* drawList = nullptr;
            const Render::Voxel::SignedDistanceGrid* sdf = nullptr;
            Render::Voxel::VoxelGridDesc desc;
            bool valid = false;
        };
        FrameView g_view;

        uint32_t g_fireRng = 2463534242u;

        uint32_t NextRandom()
        {
            g_fireRng ^= g_fireRng << 13;
            g_fireRng ^= g_fireRng >> 17;
            g_fireRng ^= g_fireRng << 5;
            return g_fireRng;
        }

        const char* EffectName(WindEffect effect)
        {
            switch (effect) {
            case WindEffect::Smoke: return "smoke";
            case WindEffect::Fire: return "fire";
            default: return "petals";
            }
        }

        bool Occluded(const float point[3])
        {
            return g_view.sdf && IsOccludedBySdf(*g_view.sdf, g_view.desc, g_view.proxy.cameraPosition, point);
        }

        ImU32 Color(float r, float g, float b, float alpha)
        {
            return IM_COL32(static_cast<int>(r), static_cast<int>(g), static_cast<int>(b),
                            static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
        }

        // Projects a world point; radiusWorld becomes a pixel radius clamped to [minPx, maxPx].
        bool ProjectDisc(const float point[3], float radiusWorld, float minPx, float maxPx, ImVec2& center, float& radiusPx)
        {
            float depth = 0.0f;
            if (!ProjectToOverlay(g_view.proxy, g_view.origin, g_view.size, point, center, &depth)) {
                return false;
            }
            radiusPx = std::clamp(radiusWorld * OverlayPixelsPerUnit(g_view.proxy, g_view.size, depth), minPx, maxPx);
            return true;
        }

        uint32_t DrawSmokeParticles(const WindDrivenParticles& system, float r, float g, float b, float maxAlpha)
        {
            uint32_t drawn = 0;
            for (uint32_t i = 0; i < system.Count(); ++i) {
                const float* p = system.Position(i);
                ImVec2 center;
                float radius;
                if (!ProjectDisc(p, system.Size(i), 1.0f, 80.0f, center, radius) || Occluded(p)) {
                    continue;
                }
                const float age = system.Age01(i);
                const float fade = (std::min)(age / 0.1f, 1.0f) * (1.0f - age);
                g_view.drawList->AddCircleFilled(center, radius, Color(r, g, b, maxAlpha * fade), 12);
                ++drawn;
            }
            return drawn;
        }

        // Flame tongues: short-lived buoyant puffs, strongly coupled to the air (small drag time), shrinking
        // towards the tip. Emitted by EmitFire from burning surface cells, never by the system's own spawner.
        WindDrivenParticleSettings MakeFlameSettings(const float pos[3], float voxelSize)
        {
            WindDrivenParticleSettings s;
            s.kind = WindParticleKind::Smoke;
            s.maxParticles = FLAME_MAX_PARTICLES;
            s.emitterPosition[0] = pos[0];
            s.emitterPosition[1] = pos[1];
            s.emitterPosition[2] = pos[2];
            s.spawnRate = 0.0f;
            s.minLifetime = 0.35f;
            s.maxLifetime = 0.8f;
            s.dragTime = 0.2f;
            s.buoyancy = 25.0f;
            s.initialVelocity[1] = 2.0f;
            s.velocityJitter = 0.6f;
            s.startSize = 0.45f * voxelSize;
            s.endSize = 0.1f * voxelSize;
            s.wallFriction = 0.5f;
            return s;
        }

        // Embers: tiny sparks that keep rising longer than the flames and follow the wind further.
        WindDrivenParticleSettings MakeEmberSettings(const float pos[3], float voxelSize)
        {
            WindDrivenParticleSettings s = MakeFlameSettings(pos, voxelSize);
            s.maxParticles = EMBER_MAX_PARTICLES;
            s.minLifetime = 1.0f;
            s.maxLifetime = 2.5f;
            s.dragTime = 0.6f;
            s.buoyancy = 6.0f;
            s.initialVelocity[1] = 3.0f;
            s.velocityJitter = 1.5f;
            s.startSize = 0.04f * voxelSize;
            s.endSize = 0.02f * voxelSize;
            return s;
        }

        void FlameColor(float age01, float kelvinOffset, float rgb[3])
        {
            Render::Voxel::BlackbodySrgb(Render::Voxel::FlameTemperature(age01) + kelvinOffset, rgb);
        }

        // Teardrop in screen space: a half disc of radius r at `base`, closed by an apex `length` along `dir`
        // (unit), pushed sideways by `sway` (in units of r). Convex, so one AddConvexPolyFilled.
        void AddTeardrop(ImDrawList* dl, ImVec2 base, ImVec2 dir, float r, float length, float sway, ImU32 color)
        {
            constexpr int ARC_POINTS = 9;
            constexpr float PI = 3.14159265f;
            const ImVec2 n(-dir.y, dir.x);
            ImVec2 points[ARC_POINTS + 1];
            for (int k = 0; k < ARC_POINTS; ++k) {
                const float theta = -0.5f * PI + PI * static_cast<float>(k) / static_cast<float>(ARC_POINTS - 1);
                const float c = std::cos(theta);
                const float sn = std::sin(theta);
                points[k] = ImVec2(base.x + r * (-c * dir.x + sn * n.x), base.y + r * (-c * dir.y + sn * n.y));
            }
            points[ARC_POINTS] = ImVec2(base.x + dir.x * length + n.x * sway * r, base.y + dir.y * length + n.y * sway * r);
            dl->AddConvexPolyFilled(points, ARC_POINTS + 1, color);
        }

        WindDrivenParticleSettings MakeSmokeSettings(const float pos[3], float voxelSize, uint32_t count, float rate)
        {
            WindDrivenParticleSettings s;
            s.kind = WindParticleKind::Smoke;
            s.maxParticles = count ? count : 3000;
            s.emitterPosition[0] = pos[0];
            s.emitterPosition[1] = pos[1] + 0.5f * voxelSize;
            s.emitterPosition[2] = pos[2];
            s.emitterRadius = 0.5f * voxelSize;
            s.spawnRate = rate >= 0.0f ? rate : 300.0f;
            s.minLifetime = 3.0f;
            s.maxLifetime = 6.0f;
            s.dragTime = 0.4f;
            s.buoyancy = 4.0f;
            s.initialVelocity[1] = 1.0f;
            s.velocityJitter = 0.5f;
            s.startSize = 0.25f * voxelSize;
            s.endSize = 2.0f * voxelSize;
            s.wallFriction = 0.8f;
            return s;
        }
    }

    namespace
    {
        constexpr uint32_t GPU_CAPACITY = 32768;
        constexpr uint32_t GPU_MAX_SPAWN_PER_RECORD = 4096;
    }

    WindEffectsOverlay::WindEffectsOverlay() = default;

    WindEffectsOverlay::~WindEffectsOverlay()
    {
        WaitGpuBeforeRelease();
        RemoveGpuHook();
    }

    // DX12 DestroyRhiResource frees at once, and the previous frames may still read these buffers.
    void WindEffectsOverlay::WaitGpuBeforeRelease()
    {
        if (m_hookApp && (m_gpuStepper || m_gpuSmoke || m_gpuPetals)) {
            if (Render::IRHIDevice* device = m_hookApp->GetRenderer().GetRhiDevice()) {
                device->WaitForRhiIdle();
            }
        }
    }

    void WindEffectsOverlay::RemoveGpuHook()
    {
        if (m_hookApp) {
            m_hookApp->GetRenderer().SetFrameComputeHook(nullptr);
            m_hookApp = nullptr;
        }
    }

    std::string WindEffectsOverlay::SetGpu(ApplicationCore& app, bool on)
    {
        if (!on) {
            WaitGpuBeforeRelease();
            RemoveGpuHook();
            m_gpu = false;
            m_gpuSmoke.reset();
            m_gpuPetals.reset();
            m_gpuStepper.reset();
            return "OK gpu=0";
        }
        WindFlowOverlay& wind = app.GetWindFlowOverlay();
        const Render::Voxel::ProjectedWindField* projected = wind.ProjectedField();
        if (!projected || !wind.Sdf()) {
            return "ERR gpu needs voxel.wind.overlay on mode=project";
        }
        Render::IRHIDevice* device = app.GetRenderer().GetRhiDevice();
        if (!device) {
            return "ERR no RHI device";
        }
        WaitGpuBeforeRelease();
        RemoveGpuHook();
        m_gpuSmoke.reset();
        m_gpuPetals.reset();
        auto stepper = std::make_unique<Render::Voxel::GpuWindParticleStepper>();
        std::string error;
        if (!stepper->Initialize(*device, error) || !stepper->UploadField(*wind.Sdf(), projected->Grid(), wind.Desc(), error)) {
            m_gpuError = error;
            m_gpu = false;
            return "ERR " + error;
        }
        m_gpuStepper = std::move(stepper);
        m_gpuError.clear();
        m_hookApp = &app;
        app.GetRenderer().SetFrameComputeHook([this](Render::IRhiCommandEncoder& encoder) { RecordGpu(encoder); });
        m_gpu = true;
        return "OK gpu=1 capacity=" + std::to_string(GPU_CAPACITY) + " maxSpawnPerRecord=" + std::to_string(GPU_MAX_SPAWN_PER_RECORD);
    }

    void WindEffectsOverlay::RecordGpu(Render::IRhiCommandEncoder& encoder)
    {
        if (!m_gpu || !m_gpuStepper) {
            return;
        }
        std::string error;
        for (const auto* flagAndStream : { &m_gpuSmoke, &m_gpuPetals }) {
            Render::Voxel::GpuWindParticleStream* stream = flagAndStream->get();
            if (stream && stream->HasParams() && !stream->Record(encoder, *m_gpuStepper, error)) {
                m_gpuError = error;
            }
        }
    }

    void WindEffectsOverlay::StepGpu(ApplicationCore& app, Render::Voxel::WindDrivenParticles& particles,
                                     Render::Voxel::WindDrivenParticles& spawner, Render::Voxel::WindEmitterSet& sources,
                                     std::unique_ptr<Render::Voxel::GpuWindParticleStream>& stream,
                                     float rate, float radius, const float offset[3], float dt)
    {
        if (!stream) {
            auto created = std::make_unique<Render::Voxel::GpuWindParticleStream>();
            Render::IRHIDevice* device = app.GetRenderer().GetRhiDevice();
            std::string error;
            if (!device || !created->Initialize(*device, GPU_CAPACITY, GPU_MAX_SPAWN_PER_RECORD, error)) {
                m_gpuError = device ? error : "no RHI device";
                SetGpu(app, false);
                return;
            }
            stream = std::move(created);
        }
        spawner.ClearParticles();
        sources.Emit(spawner, rate, dt, offset, radius);
        spawner.ExportStates(m_gpuSpawnStates);
        stream->QueueSpawn(m_gpuSpawnStates.data(), static_cast<uint32_t>(m_gpuSpawnStates.size()));
        stream->SetParams(m_gpuStepper->MakeGpuParams(
            Render::Voxel::MakeWindParticleStepParams(particles.Settings(), m_voxelSize, dt), stream->Capacity()));
        if (stream->ReadLatest(m_gpuReadback)) {
            particles.ImportStates(m_gpuReadback);
        }
    }

    std::string WindEffectsOverlay::Start(ApplicationCore& app, WindEffect effect, const WindEffectSettings& settings)
    {
        Stop(effect);
        return Add(app, effect, settings);
    }

    std::string WindEffectsOverlay::Add(ApplicationCore& app, WindEffect effect, const WindEffectSettings& settings)
    {
        Render::Voxel::SceneVoxelization& vox = app.GetSceneVoxelization();
        WindFlowOverlay& wind = app.GetWindFlowOverlay();
        if (!wind.Field()) {
            return "ERR wind overlay is off (voxel.wind.overlay on first)";
        }
        if (m_windGeneration != wind.Generation()) {
            StopAll();
            m_windGeneration = wind.Generation();
            if (m_gpu) {
                SetGpu(app, false);
            }
        }
        float pos[3];
        std::string error;
        if (!ResolvePosition(app, settings, pos, error)) {
            return error;
        }
        const float vs = wind.Desc().voxelSize;
        m_voxelSize = vs;
        m_windSpeed = wind.WindSpeed();
        std::ostringstream out;
        out << "OK fx=" << EffectName(effect) << " at=" << pos[0] << "," << pos[1] << "," << pos[2];

        if (effect == WindEffect::Smoke) {
            if (!m_smokeOn) {
                // The system's own emitter stays off; every source emits through m_smokeSources.
                const WindDrivenParticleSettings s = MakeSmokeSettings(pos, vs, settings.count, 0.0f);
                m_smoke.Reset(s, 11);
                m_smokeSpawn.Reset(s, 11);
                m_smokeRate = settings.rate >= 0.0f ? settings.rate : 300.0f;
                m_smokeSources.Clear();
                m_smokeOn = true;
            }
            m_smokeSources.Add(pos);
            out << " sources=" << m_smokeSources.Count() << " count=" << m_smoke.Settings().maxParticles << " rate=" << m_smokeRate;
        } else if (effect == WindEffect::Petals) {
            if (!m_petalsOn) {
                WindDrivenParticleSettings s;
                s.kind = WindParticleKind::Petal;
                s.maxParticles = settings.count ? settings.count : 2500;
                s.emitterPosition[0] = pos[0];
                s.emitterPosition[1] = pos[1] + 6.0f * vs;
                s.emitterPosition[2] = pos[2];
                s.emitterRadius = 4.0f * vs;
                s.spawnRate = 0.0f;
                s.minLifetime = 6.0f;
                s.maxLifetime = 12.0f;
                s.dragTime = 0.12f;
                s.acceleration[1] = -9.8f;
                s.velocityJitter = 0.3f;
                s.startSize = (std::max)(0.12f * vs, MIN_PETAL_SIZE);
                s.endSize = s.startSize;
                s.flutterAcceleration = 12.0f;
                s.flutterFrequency = 1.2f;
                s.spinRate = 5.0f;
                s.wallFriction = 0.3f;
                m_petals.Reset(s, 23);
                m_petalSpawn.Reset(s, 23);
                m_petalRate = settings.rate >= 0.0f ? settings.rate : 250.0f;
                m_petalSources.Clear();
                m_petalsOn = true;
            }
            m_petalSources.Add(pos);
            out << " sources=" << m_petalSources.Count() << " count=" << m_petals.Settings().maxParticles << " rate=" << m_petalRate;
        } else {
            if (!vox.HasResult()) {
                return "ERR no voxel.scene result (run voxel.scene first)";
            }
            if (!m_fireOn) {
                // Unlimited spread covered all of Bistro (~40k burnt cells, drawMs ~17, 55 fps); 12 voxels keeps
                // the fire, and the per-cell draw and occlusion work, around its sources.
                Render::Voxel::WindFireSettings fireSettings{};
                fireSettings.maxSpreadRadius = settings.radius >= 0.0f ? settings.radius : 12.0f;
                m_fire.Build(vox.Grid(), vox.Desc(), fireSettings, 5);
            }
            if (!m_fire.Ignite(pos, 6)) {
                if (!m_fireOn) {
                    m_fire = Render::Voxel::WindFireSpread();
                }
                return "ERR no surface voxel near the target";
            }
            if (!m_fireOn) {
                m_fireSmokeRate = settings.rate >= 0.0f ? settings.rate : 1.5f;
                m_fireSmokeCarry = 0.0f;
                WindDrivenParticleSettings s = MakeSmokeSettings(pos, vs, settings.count ? settings.count : 4000, 0.0f);
                s.minLifetime = 2.0f;
                s.maxLifetime = 4.0f;
                s.endSize = 1.5f * vs;
                m_fireSmoke.Reset(s, 31);
                m_flames.Reset(MakeFlameSettings(pos, vs), 37);
                m_embers.Reset(MakeEmberSettings(pos, vs), 41);
                m_flameCarry = 0.0f;
                m_emberCarry = 0.0f;
                m_flameTime = 0.0f;
                m_fireSources.Clear();
                m_fireOn = true;
            }
            m_fireSources.Add(pos);
            out << " sources=" << m_fireSources.Count() << " count=" << m_fireSmoke.Settings().maxParticles << " rate=" << m_fireSmokeRate
                << " radius=" << m_fire.Settings().maxSpreadRadius << " surfaceCells=" << m_fire.Stats().surfaceCells;
        }
        out << " wind=" << wind.WindSpeed();
        return out.str();
    }

    std::string WindEffectsOverlay::UndoLast(WindEffect effect)
    {
        Render::Voxel::WindEmitterSet& sources = Sources(effect);
        if (!IsActive(effect) || !sources.RemoveLast()) {
            return std::string("ERR fx=") + EffectName(effect) + " has no source";
        }
        if (sources.Count() == 0) {
            Stop(effect);
        }
        std::ostringstream out;
        out << "OK fx=" << EffectName(effect) << " sources=" << sources.Count();
        return out.str();
    }

    Render::Voxel::WindEmitterSet& WindEffectsOverlay::Sources(WindEffect effect)
    {
        switch (effect) {
        case WindEffect::Smoke: return m_smokeSources;
        case WindEffect::Fire: return m_fireSources;
        default: return m_petalSources;
        }
    }

    const Render::Voxel::WindEmitterSet& WindEffectsOverlay::Sources(WindEffect effect) const
    {
        return const_cast<WindEffectsOverlay*>(this)->Sources(effect);
    }

    void WindEffectsOverlay::EmitOffset(WindEffect effect, float outOffset[3]) const
    {
        const float vs = m_voxelSize;
        outOffset[0] = 0.0f;
        outOffset[1] = effect == WindEffect::Petals ? 6.0f * vs : effect == WindEffect::Smoke ? 0.5f * vs : 0.0f;
        outOffset[2] = 0.0f;
    }

    void WindEffectsOverlay::Stop(WindEffect effect)
    {
        switch (effect) {
        case WindEffect::Smoke:
            m_smokeOn = false;
            m_smoke.Reset(WindDrivenParticleSettings{}, 1);
            m_smokeSources.Clear();
            WaitGpuBeforeRelease();
            m_gpuSmoke.reset();
            break;
        case WindEffect::Fire:
            m_fireOn = false;
            m_fire = Render::Voxel::WindFireSpread();
            m_fireSmoke.Reset(WindDrivenParticleSettings{}, 1);
            m_fireSmokeCarry = 0.0f;
            m_flames.Reset(WindDrivenParticleSettings{}, 1);
            m_embers.Reset(WindDrivenParticleSettings{}, 1);
            m_flameCarry = 0.0f;
            m_emberCarry = 0.0f;
            m_flameTime = 0.0f;
            m_fireSources.Clear();
            break;
        case WindEffect::Petals:
            m_petalsOn = false;
            m_petals.Reset(WindDrivenParticleSettings{}, 1);
            m_petalSources.Clear();
            WaitGpuBeforeRelease();
            m_gpuPetals.reset();
            break;
        }
    }

    void WindEffectsOverlay::StopAll()
    {
        Stop(WindEffect::Smoke);
        Stop(WindEffect::Fire);
        Stop(WindEffect::Petals);
    }

    bool WindEffectsOverlay::IsActive(WindEffect effect) const
    {
        switch (effect) {
        case WindEffect::Smoke: return m_smokeOn;
        case WindEffect::Fire: return m_fireOn;
        default: return m_petalsOn;
        }
    }

    std::string WindEffectsOverlay::Status(ApplicationCore& app) const
    {
        std::ostringstream out;
        out << "OK smoke=" << (m_smokeOn ? 1 : 0) << " fire=" << (m_fireOn ? 1 : 0) << " petals=" << (m_petalsOn ? 1 : 0);
        if (m_smokeOn) {
            out << " smokeAlive=" << m_smoke.Count() << " smokeSources=" << m_smokeSources.Count();
        }
        if (m_petalsOn) {
            out << " petalsAlive=" << m_petals.Count() << " petalSources=" << m_petalSources.Count();
        }
        if (m_fireOn) {
            out << " fireBurning=" << m_fire.Stats().burning << " fireBurnt=" << m_fire.Stats().burnt
                << " fireTicks=" << m_fire.Stats().ticks << " fireSmokeAlive=" << m_fireSmoke.Count()
                << " flames=" << m_flames.Count() << " embers=" << m_embers.Count()
                << " fireSources=" << m_fireSources.Count();
        }
        out << " gpu=" << (m_gpu ? 1 : 0);
        if (m_gpu) {
            const uint64_t smokeRecords = m_gpuSmoke ? m_gpuSmoke->RecordCount() : 0;
            const uint64_t petalRecords = m_gpuPetals ? m_gpuPetals->RecordCount() : 0;
            const uint64_t dropped = (m_gpuSmoke ? m_gpuSmoke->DroppedSpawns() : 0) + (m_gpuPetals ? m_gpuPetals->DroppedSpawns() : 0);
            out << " gpuRecords=" << smokeRecords << "," << petalRecords << " gpuDropped=" << dropped;
            for (const auto* stream : { m_gpuSmoke.get(), m_gpuPetals.get() }) {
                if (stream) {
                    out << " [queued=" << stream->QueuedSpawns() << " reads=" << stream->ReadCount()
                        << " readAlive=" << stream->LastReadAlive() << " dt=" << stream->Params().dt
                        << " n=" << stream->Params().particleCount << "]";
                }
            }
        }
        if (!m_gpuError.empty()) {
            out << " gpuErr=" << m_gpuError;
        }
        if (m_smokeOn || m_fireOn || m_petalsOn) {
            out << " frames=" << m_frames << " stepMs=" << m_lastStepMs << " drawMs=" << m_lastDrawMs << " drawn=" << m_lastDrawn;
            if (m_volume) {
                out << " volume=1 dims=" << m_lastVolumeDims[0] << "x" << m_lastVolumeDims[1] << "x"
                    << m_lastVolumeDims[2] << " volumeMs=" << m_lastVolumeMs
                    << " maxSmoke=" << m_lastVolumeMaxSmoke << " maxFlame=" << m_lastVolumeMaxFlame;
            }
            if (m_surface) {
                // splatMs = gather + fit + splat (isoMs minus the extraction).
                out << " surface=1 tris=" << m_lastSurfaceTris << " isoMs=" << m_lastSurfaceMs
                    << " splatMs=" << (m_lastSurfaceMs - m_lastExtractMs) << " mtMs=" << m_lastExtractMs
                    << " flameGrid=" << m_flameGrid.Desc().dims[0] << "x" << m_flameGrid.Desc().dims[1] << "x" << m_flameGrid.Desc().dims[2]
                    << " smokeGrid=" << m_smokeGrid.Desc().dims[0] << "x" << m_smokeGrid.Desc().dims[1] << "x" << m_smokeGrid.Desc().dims[2];
                if (m_surfaceScene) {
                    out << " scene=1 flameTris=" << m_sceneSurfaces.FlameTriangles()
                        << " smokeTris=" << m_sceneSurfaces.SmokeTriangles()
                        << " dropped=" << m_sceneSurfaces.DroppedTriangles()
                        << " pushMs=" << m_sceneSurfaces.LastPushMs()
                    << " expandMs=" << (m_sceneSurfaces.LastPushMs() - m_sceneSurfaces.LastUploadMs())
                        << " uploadMs=" << m_sceneSurfaces.LastUploadMs()
                        << " uploadKB=" << m_sceneSurfaces.LastUploadBytes() / 1024
                        << " pendingFrames=" << m_sceneSurfaces.PendingFrames();
                }
            }
        }
        const WindFlowOverlay& wind = app.GetWindFlowOverlay();
        if (wind.Field() && wind.Generation() == m_windGeneration) {
            if (m_smokeOn) {
                AppendSourceReport(out, WindEffect::Smoke, *wind.Field());
            }
            if (m_fireOn) {
                AppendSourceReport(out, WindEffect::Fire, *wind.Field());
            }
            if (m_petalsOn) {
                AppendSourceReport(out, WindEffect::Petals, *wind.Field());
            }
        }
        return out.str();
    }

    void WindEffectsOverlay::AppendSourceReport(std::ostringstream& out, WindEffect effect,
                                                const Render::Voxel::IWindVelocityField& field) const
    {
        std::vector<float> points;
        if (effect == WindEffect::Smoke) {
            Render::Voxel::CollectParticlePositions(m_smoke, points);
        } else if (effect == WindEffect::Petals) {
            Render::Voxel::CollectParticlePositions(m_petals, points);
        } else {
            // Burnt and burning cells: where the fire front has been, not its smoke.
            for (const std::vector<uint32_t>* cells : { &m_fire.BurntCells(), &m_fire.BurningCells() }) {
                for (uint32_t cell : *cells) {
                    float p[3];
                    m_fire.SurfacePoint(cell, p);
                    points.insert(points.end(), p, p + 3);
                }
            }
        }
        float offset[3];
        EmitOffset(effect, offset);
        const std::vector<Render::Voxel::WindSourceDrift> drift =
            Render::Voxel::MeasureWindSourceDrift(Sources(effect), offset, points, field);
        for (size_t i = 0; i < drift.size(); ++i) {
            const Render::Voxel::WindSourceDrift& d = drift[i];
            out << " " << EffectName(effect) << i << "=(points=" << d.points << " drift=" << d.meanOffset[0] << ","
                << d.meanOffset[2] << " pathWind=" << d.pathWind[0] << "," << d.pathWind[2] << " cos=" << d.cosine
                << " sourceWind=" << d.sourceWind[0] << "," << d.sourceWind[2] << " sourceCos=" << d.sourceCosine << ")";
        }
    }

    bool WindEffectsOverlay::ResolvePosition(ApplicationCore& app, const WindEffectSettings& settings, float outPosition[3],
                                             std::string& error) const
    {
        if (settings.hasPosition) {
            for (int i = 0; i < 3; ++i) {
                outPosition[i] = settings.position[i];
            }
            return true;
        }
        Camera* camera = app.GetMainCamera();
        if (!camera) {
            error = "ERR no main camera";
            return false;
        }
        if (settings.hasScreen) {
            if (!PickSurface(app, settings.screen[0], settings.screen[1], outPosition)) {
                error = "ERR no surface under that pixel";
                return false;
            }
            return true;
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const Render::RenderCameraProxy proxy = camera->BuildRenderCameraProxy(viewport->Size.x, viewport->Size.y);
        WindFlowOverlay& wind = app.GetWindFlowOverlay();
        float distance = 20.0f * wind.Desc().voxelSize;
        if (const Render::Voxel::SignedDistanceGrid* sdf = wind.Sdf()) {
            const Render::Voxel::SdfRayHit hit =
                Render::Voxel::TraceSdfRay(*sdf, wind.Desc(), proxy.cameraPosition, proxy.cameraForward, Render::Voxel::SdfTraceSettings{});
            if (hit.hit) {
                distance = hit.t;
            }
        }
        for (int i = 0; i < 3; ++i) {
            outPosition[i] = proxy.cameraPosition[i] + proxy.cameraForward[i] * distance;
        }
        return true;
    }

    bool WindEffectsOverlay::PickSurface(ApplicationCore& app, float screenX, float screenY, float outPosition[3]) const
    {
        Camera* camera = app.GetMainCamera();
        WindFlowOverlay& wind = app.GetWindFlowOverlay();
        const Render::Voxel::SignedDistanceGrid* sdf = wind.Sdf();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        if (!camera || !sdf || viewport->Size.x <= 0.0f || viewport->Size.y <= 0.0f) {
            return false;
        }
        const Render::RenderCameraProxy proxy = camera->BuildRenderCameraProxy(viewport->Size.x, viewport->Size.y);
        // Inverse of ProjectToOverlay.
        const float sx = 2.0f * screenX / viewport->Size.x - 1.0f;
        const float sy = 1.0f - 2.0f * screenY / viewport->Size.y;
        float dir[3];
        for (int i = 0; i < 3; ++i) {
            dir[i] = proxy.cameraForward[i] + sx * proxy.tanHalfFovY * proxy.aspectRatio * proxy.cameraRight[i] +
                     sy * proxy.tanHalfFovY * proxy.cameraUp[i];
        }
        const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (int i = 0; i < 3; ++i) {
            dir[i] /= len;
        }
        // Generous step budget for rays that graze rooftops.
        Render::Voxel::SdfTraceSettings trace;
        trace.maxSteps = 2048;
        const Render::Voxel::SdfRayHit hit = Render::Voxel::TraceSdfRay(*sdf, wind.Desc(), proxy.cameraPosition, dir, trace);
        if (hit.hit) {
            for (int i = 0; i < 3; ++i) {
                outPosition[i] = proxy.cameraPosition[i] + dir[i] * hit.t;
            }
            return true;
        }
        // Thin ground (Bistro's streets) is often missing from the occupancy grid; the wind solver
        // treats the grid bottom as the floor, so a downward ray that misses lands there instead.
        const Render::Voxel::VoxelGridDesc& desc = wind.Desc();
        if (dir[1] >= -1e-4f) {
            return false;
        }
        const float t = (desc.origin[1] - proxy.cameraPosition[1]) / dir[1];
        if (t <= 0.0f) {
            return false;
        }
        const float floorPoint[3] = { proxy.cameraPosition[0] + dir[0] * t, desc.origin[1],
                                      proxy.cameraPosition[2] + dir[2] * t };
        const float extentX = static_cast<float>(desc.dimX) * desc.voxelSize;
        const float extentZ = static_cast<float>(desc.dimZ) * desc.voxelSize;
        if (floorPoint[0] < desc.origin[0] || floorPoint[0] > desc.origin[0] + extentX ||
            floorPoint[2] < desc.origin[2] || floorPoint[2] > desc.origin[2] + extentZ) {
            return false;
        }
        for (int i = 0; i < 3; ++i) {
            outPosition[i] = floorPoint[i];
        }
        return true;
    }

    void WindEffectsOverlay::HandlePlacementClick(ApplicationCore& app)
    {
        if (!m_placing || ImGui::GetIO().WantCaptureMouse || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            return;
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        WindEffectSettings settings;
        settings.hasScreen = true;
        settings.screen[0] = mouse.x - viewport->Pos.x;
        settings.screen[1] = mouse.y - viewport->Pos.y;
        m_lastMessage = Add(app, m_placeEffect, settings);
    }

    void WindEffectsOverlay::Draw(ApplicationCore& app)
    {
        HandlePlacementClick(app);
        if (!m_smokeOn && !m_fireOn && !m_petalsOn) {
            ClearSceneSurfaces(app);
            if (m_volume) {
                app.GetRenderer().SetParticleVolumeEnabled(false);
            }
            return;
        }
        WindFlowOverlay& wind = app.GetWindFlowOverlay();
        if (!wind.Field() || wind.Generation() != m_windGeneration) {
            StopAll();
            m_windGeneration = wind.Generation();
            ClearSceneSurfaces(app);
            if (m_gpu) {
                SetGpu(app, false);
            }
            if (m_volume) {
                app.GetRenderer().SetParticleVolumeEnabled(false);
            }
            return;
        }
        Camera* camera = app.GetMainCamera();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        if (!camera || viewport->Size.x <= 0.0f || viewport->Size.y <= 0.0f) {
            return;
        }

        const auto stepStart = std::chrono::steady_clock::now();
        const Render::Voxel::IWindVelocityField& field = *wind.Field();
        const float dt = wind.FrameDt();
        const float vs = wind.Desc().voxelSize;
        m_voxelSize = vs;
        m_windSpeed = wind.WindSpeed();
        if (dt > 0.0f) {
            if (m_smokeOn) {
                float offset[3];
                EmitOffset(WindEffect::Smoke, offset);
                if (m_gpu) {
                    StepGpu(app, m_smoke, m_smokeSpawn, m_smokeSources, m_gpuSmoke, m_smokeRate, 0.5f * vs, offset, dt);
                } else {
                    m_smokeSources.Emit(m_smoke, m_smokeRate, dt, offset, 0.5f * vs);
                    m_smoke.Step(field, dt);
                }
            }
            if (m_petalsOn) {
                float offset[3];
                EmitOffset(WindEffect::Petals, offset);
                if (m_gpu) {
                    StepGpu(app, m_petals, m_petalSpawn, m_petalSources, m_gpuPetals, m_petalRate, 4.0f * vs, offset, dt);
                } else {
                    m_petalSources.Emit(m_petals, m_petalRate, dt, offset, 4.0f * vs);
                    m_petals.Step(field, dt);
                }
            }
            if (m_fireOn) {
                m_fire.Step(field, dt);
                EmitFire(dt);
                m_fireSmoke.Step(field, dt);
                m_flames.Step(field, dt);
                m_embers.Step(field, dt);
            }
        }
        const auto drawStart = std::chrono::steady_clock::now();

        g_view.proxy = camera->BuildRenderCameraProxy(viewport->Size.x, viewport->Size.y);
        g_view.origin = viewport->Pos;
        g_view.size = viewport->Size;
        g_view.drawList = ImGui::GetBackgroundDrawList();
        g_view.sdf = wind.Sdf();
        g_view.desc = wind.Desc();
        g_view.valid = true;

        m_lastDrawn = 0;
        const bool volumeDrawn = m_volume && (m_fireOn || m_smokeOn) && BuildVolume(app);
        if (m_volume) {
            app.GetRenderer().SetParticleVolumeEnabled(volumeDrawn);
        }
        if (m_smokeOn && !m_surface && !volumeDrawn) {
            DrawSmoke(app, m_smoke, 1.0f);
        }
        if (m_surface && !volumeDrawn && (m_fireOn || m_smokeOn)) {
            DrawSurfaces(app);
        } else {
            ClearSceneSurfaces(app);
        }
        if (m_fireOn) {
            m_volumeDrawn = volumeDrawn;
            DrawFire(app);
        }
        if (m_petalsOn) {
            DrawPetals(app);
        }
        DrawSourceMarkers(field);
        g_view.valid = false;

        const auto end = std::chrono::steady_clock::now();
        m_lastStepMs = std::chrono::duration<double, std::milli>(drawStart - stepStart).count();
        m_lastDrawMs = std::chrono::duration<double, std::milli>(end - drawStart).count();
        ++m_frames;
    }

    void WindEffectsOverlay::DrawSmoke(ApplicationCore&, const Render::Voxel::WindDrivenParticles& smoke, float alphaScale)
    {
        m_lastDrawn += DrawSmokeParticles(smoke, 170.0f, 170.0f, 175.0f, SMOKE_MAX_ALPHA * alphaScale);
    }

    // Spreads `rate * dt * burning` emissions of `system` over random burning cells, carrying the fraction.
    // Smoke leaves from a voxel above the flames so it reads as rising out of them.
    void WindEffectsOverlay::EmitFire(float dt)
    {
        const std::vector<uint32_t>& burning = m_fire.BurningCells();
        if (burning.empty()) {
            return;
        }
        m_flameTime += dt;
        const float vs = m_voxelSize > 0.0f ? m_voxelSize : 1.0f;
        const float cells = static_cast<float>(burning.size());
        struct Emission
        {
            Render::Voxel::WindDrivenParticles* system;
            float* carry;
            float rate;
            float lift;
            float radius;
        };
        const Emission emissions[] = {
            { &m_flames, &m_flameCarry, FLAMES_PER_CELL_PER_SECOND, 0.1f * vs, 0.4f * vs },
            { &m_embers, &m_emberCarry, EMBERS_PER_CELL_PER_SECOND, 0.3f * vs, 0.4f * vs },
            { &m_fireSmoke, &m_fireSmokeCarry, m_fireSmokeRate, 1.0f * vs, 0.3f * vs },
        };
        for (const Emission& e : emissions) {
            *e.carry += e.rate * dt * cells;
            const uint32_t count = static_cast<uint32_t>(*e.carry);
            *e.carry -= static_cast<float>(count);
            for (uint32_t k = 0; k < count; ++k) {
                float point[3];
                m_fire.SurfacePoint(burning[NextRandom() % burning.size()], point);
                point[1] += e.lift;
                e.system->Emit(point, e.radius, 1);
            }
        }
    }

    // Stylised flames: each flame particle is a teardrop stretched along its screen-space velocity, in three
    // translucent layers (wide red glow, blackbody body, small hot core) since ImGui has no additive blend.
    // Burnt cells are scorch marks, burning cells a low ember glow under the flames, embers short streaks.
    void WindEffectsOverlay::DrawFire(ApplicationCore&)
    {
        const float vs = g_view.desc.voxelSize;
        ImDrawList* dl = g_view.drawList;
        // Volume mode: the ray-marched flames carry the glow; ImGui scorch marks, cell discs and sprites would sit
        // on top of the volume (no depth test against it), so only embers stay.
        static const std::vector<uint32_t> NO_CELLS;
        for (uint32_t cell : (m_volumeDrawn ? NO_CELLS : m_fire.BurntCells())) {
            float p[3];
            m_fire.SurfacePoint(cell, p);
            ImVec2 center;
            float radius;
            if (!ProjectDisc(p, 0.4f * vs, 1.0f, 40.0f, center, radius) || Occluded(p)) {
                continue;
            }
            dl->AddCircleFilled(center, radius, Color(25.0f, 18.0f, 15.0f, 0.3f), 8);
            ++m_lastDrawn;
        }
        for (uint32_t cell : (m_volumeDrawn ? NO_CELLS : m_fire.BurningCells())) {
            float p[3];
            m_fire.SurfacePoint(cell, p);
            ImVec2 center;
            float radius;
            if (!ProjectDisc(p, 0.55f * vs, 1.0f, 40.0f, center, radius) || Occluded(p)) {
                continue;
            }
            const float pulse = 0.5f + 0.5f * Render::Voxel::FlameNoise(m_flameTime * 3.0f, static_cast<float>(cell) * 0.37f);
            dl->AddCircleFilled(center, radius, Color(170.0f, 45.0f, 10.0f, 0.25f + 0.25f * pulse), 10);
            ++m_lastDrawn;
        }

        // Iso-surface mode draws the flames and the fire smoke as surfaces instead (DrawSurfaces).
        if (!m_surface && !m_volumeDrawn) {
            m_lastDrawn += DrawSmokeParticles(m_fireSmoke, 70.0f, 66.0f, 64.0f, SMOKE_MAX_ALPHA * 1.4f);

            // Project every visible flame once, then draw layer by layer so cores sit on top of all glows.
            struct FlameSprite
            {
                ImVec2 base;
                ImVec2 dir;
                float radius;
                float length;
                float sway;
                float age;
                float fade;
            };
            std::vector<FlameSprite> sprites;
            sprites.reserve(m_flames.Count());
            for (uint32_t i = 0; i < m_flames.Count(); ++i) {
                const float* p = m_flames.Position(i);
                ImVec2 base;
                float radius;
                if (!ProjectDisc(p, m_flames.Size(i), 1.5f, 60.0f, base, radius) || Occluded(p)) {
                    continue;
                }
                // Screen direction of travel; an almost head-on velocity falls back to screen up.
                const float* v = m_flames.Velocity(i);
                const float ahead[3] = { p[0] + v[0] * 0.05f, p[1] + v[1] * 0.05f, p[2] + v[2] * 0.05f };
                ImVec2 tip;
                ImVec2 dir(0.0f, -1.0f);
                if (ProjectToOverlay(g_view.proxy, g_view.origin, g_view.size, ahead, tip, nullptr)) {
                    const float dx = tip.x - base.x;
                    const float dy = tip.y - base.y;
                    const float len = std::sqrt(dx * dx + dy * dy);
                    if (len > 0.5f) {
                        dir = ImVec2(dx / len, dy / len);
                    }
                }
                const float age = m_flames.Age01(i);
                FlameSprite sprite;
                sprite.base = base;
                sprite.dir = dir;
                // Per-particle size variation so neighbouring tongues do not read as a stamped pattern.
                const float scale = 0.6f + 0.8f * Render::Voxel::ValueNoise2(static_cast<float>(i % 211) * 0.53f, 7.1f);
                sprite.radius = radius * 0.7f * scale;
                sprite.length = sprite.radius * (3.0f + 2.5f * age);
                sprite.sway = 0.9f * Render::Voxel::FlameNoise(m_flameTime * 6.0f + static_cast<float>(i % 97) * 0.61f,
                                                               static_cast<float>(i % 89) * 1.3f);
                sprite.age = age;
                sprite.fade = (std::min)(age / 0.08f, 1.0f) * (1.0f - age * age);
                sprites.push_back(sprite);
            }
            for (const FlameSprite& f : sprites) {
                float rgb[3];
                FlameColor(f.age, -300.0f, rgb);
                AddTeardrop(dl, f.base, f.dir, f.radius * 1.8f, f.length * 1.3f, f.sway, Color(rgb[0], rgb[1], rgb[2], 0.07f * f.fade));
            }
            for (const FlameSprite& f : sprites) {
                float rgb[3];
                FlameColor(f.age, 0.0f, rgb);
                AddTeardrop(dl, f.base, f.dir, f.radius, f.length, f.sway, Color(rgb[0], rgb[1], rgb[2], 0.38f * f.fade));
            }
            for (const FlameSprite& f : sprites) {
                if (f.age > 0.5f) {
                    continue;
                }
                float rgb[3];
                FlameColor(f.age, 1500.0f, rgb);
                AddTeardrop(dl, f.base, f.dir, f.radius * 0.45f, f.length * 0.5f, f.sway * 0.5f,
                            Color(rgb[0], rgb[1], rgb[2], 0.55f * (1.0f - 2.0f * f.age)));
            }
            m_lastDrawn += static_cast<uint32_t>(sprites.size()) * 3;
        }

        for (uint32_t i = 0; i < m_embers.Count(); ++i) {
            const float* p = m_embers.Position(i);
            const float* v = m_embers.Velocity(i);
            ImVec2 head;
            ImVec2 tail;
            const float back[3] = { p[0] - v[0] * 0.04f, p[1] - v[1] * 0.04f, p[2] - v[2] * 0.04f };
            if (!ProjectToOverlay(g_view.proxy, g_view.origin, g_view.size, p, head, nullptr)
                || !ProjectToOverlay(g_view.proxy, g_view.origin, g_view.size, back, tail, nullptr) || Occluded(p)) {
                continue;
            }
            const float age = m_embers.Age01(i);
            const float twinkle = 0.6f + 0.4f * Render::Voxel::FlameNoise(m_flameTime * 12.0f, static_cast<float>(i) * 0.71f);
            float rgb[3];
            Render::Voxel::BlackbodySrgb(2200.0f - 1000.0f * age, rgb);
            dl->AddLine(tail, head, Color(rgb[0], rgb[1], rgb[2], twinkle * (1.0f - age)), 1.5f);
            ++m_lastDrawn;
        }
    }

    namespace
    {
        // A lone particle of weight 1 crosses ISO_LEVEL where (1 - s^2)^3 = 0.5, s = 0.454, so radius 2.2 * Size
        // gives it about the sprite's size; 2.5 lets neighbouring flames merge into tongues (3.0 fused the roof fire into one sheet).
        constexpr float ISO_LEVEL = 0.5f;
        constexpr float SPLAT_RADIUS_PER_SIZE = 2.5f;
        constexpr float SURFACE_CELL_PER_VOXEL = 0.45f;
        constexpr uint32_t SURFACE_MAX_DIM = 72;

        void FlameSurfaceColor(float age, float outRgb[3], float& outAlpha)
        {
            Render::Voxel::BlackbodySrgb(Render::Voxel::FlameTemperature((std::min)((std::max)(age, 0.0f), 1.0f)), outRgb);
            outAlpha = 0.85f * (1.0f - 0.6f * age * age);
        }

        // attribute 0 = plain smoke (light grey), 1 = smoke off the fire (dark).
        void SmokeSurfaceColor(float attribute, float outRgb[3], float& outAlpha)
        {
            const float t = (std::min)((std::max)(attribute, 0.0f), 1.0f);
            outRgb[0] = 175.0f + (70.0f - 175.0f) * t;
            outRgb[1] = 175.0f + (66.0f - 175.0f) * t;
            outRgb[2] = 180.0f + (64.0f - 180.0f) * t;
            outAlpha = 0.45f - 0.15f * t;
        }

        float SrgbByteToLinear(float v)
        {
            const float c = v / 255.0f;
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        // Scene-mesh mode: the same colours as linear vertex colours (they multiply the albedo); alpha is unused.
        void FlameVertexColor(float age, float outRgba[4])
        {
            float rgb[3];
            float alpha;
            FlameSurfaceColor(age, rgb, alpha);
            for (int c = 0; c < 3; ++c) {
                outRgba[c] = SrgbByteToLinear(rgb[c]);
            }
            outRgba[3] = 1.0f;
        }

        void SmokeVertexColor(float attribute, float outRgba[4])
        {
            float rgb[3];
            float alpha;
            SmokeSurfaceColor(attribute, rgb, alpha);
            for (int c = 0; c < 3; ++c) {
                outRgba[c] = SrgbByteToLinear(rgb[c]);
            }
            outRgba[3] = 1.0f;
        }
    }

    // useAge: the attribute is each particle's Age01 (flames), else `attribute`. Old particles weigh less, so
    // a puff shrinks away instead of popping out when it dies.
    void WindEffectsOverlay::GatherSplat(const Render::Voxel::WindDrivenParticles& particles, float attribute, bool useAge)
    {
        for (uint32_t i = 0; i < particles.Count(); ++i) {
            const float* p = particles.Position(i);
            const float age = particles.Age01(i);
            m_splatPositions.insert(m_splatPositions.end(), p, p + 3);
            m_splatRadii.push_back(SPLAT_RADIUS_PER_SIZE * particles.Size(i));
            m_splatWeights.push_back(1.0f - age * age);
            m_splatAttributes.push_back(useAge ? age : attribute);
        }
    }

    uint32_t WindEffectsOverlay::BuildSurface(Render::Fluid::ParticleSplatGrid& grid, Render::Fluid::IsoSurfaceMesh& mesh)
    {
        const uint32_t count = static_cast<uint32_t>(m_splatRadii.size());
        float maxRadius = 0.0f;
        for (float r : m_splatRadii) {
            maxRadius = (std::max)(maxRadius, r);
        }
        grid.Fit(m_splatPositions.data(), count, 3, maxRadius, SURFACE_CELL_PER_VOXEL * m_voxelSize, SURFACE_MAX_DIM);
        grid.SplatAll(m_splatPositions.data(), count, 3, m_splatRadii.data(), 0.0f, m_splatWeights.data(), m_splatAttributes.data());
        const auto extractStart = std::chrono::steady_clock::now();
        Render::Fluid::ExtractIsoSurface(grid, ISO_LEVEL, mesh);
        m_lastExtractMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - extractStart).count();
        m_splatPositions.clear();
        m_splatRadii.clear();
        m_splatWeights.clear();
        m_splatAttributes.clear();
        return mesh.TriangleCount();
    }

    // Volume cells are finer than the iso-surface ones: the ray march interpolates them, so they do not
    // need to resolve a level set, but blocky density shows as banding in thin smoke.
    constexpr float VOLUME_CELL_PER_VOXEL = 0.4f;
    constexpr uint32_t VOLUME_MAX_DIM = 96;
    constexpr float VOLUME_SMOKE_DENSITY_CAP = 2.0f;  // soft cap of the packed smoke density
    constexpr float VOLUME_MIN_KERNEL_CELLS = 1.5f;   // narrower kernels are widened (mass kept) to avoid blocks

    void WindEffectsOverlay::SetVolume(ApplicationCore& app, bool on)
    {
        m_volume = on;
        if (!on) {
            app.GetRenderer().SetParticleVolumeEnabled(false);
            m_lastVolumeDims[0] = m_lastVolumeDims[1] = m_lastVolumeDims[2] = 0;
        }
    }

    bool WindEffectsOverlay::BuildVolume(ApplicationCore& app)
    {
        const auto start = std::chrono::steady_clock::now();
        // Flames first (attribute = age), then smoke (attribute = soot: 0 plain, 1 off the fire).
        if (m_fireOn) {
            GatherSplat(m_flames, 0.0f, true);
        }
        const uint32_t flameCount = static_cast<uint32_t>(m_splatRadii.size());
        if (m_smokeOn) {
            GatherSplat(m_smoke, 0.0f, false);
        }
        if (m_fireOn) {
            GatherSplat(m_fireSmoke, 1.0f, false);
        }
        const uint32_t count = static_cast<uint32_t>(m_splatRadii.size());
        if (count == 0) {
            return false;
        }
        float maxRadius = 0.0f;
        for (float r : m_splatRadii) {
            maxRadius = (std::max)(maxRadius, r);
        }
        // Margin for the kernels widened below (at the requested cell size; a dims clamp only coarsens the cells,
        // and SplatAll clips whatever then reaches past the edge).
        maxRadius = (std::max)(maxRadius, VOLUME_MIN_KERNEL_CELLS * VOLUME_CELL_PER_VOXEL * m_voxelSize);
        // One shared grid for both fields, so the shader reads them from the same cell.
        m_volumeSmokeGrid.Fit(m_splatPositions.data(), count, 3, maxRadius, VOLUME_CELL_PER_VOXEL * m_voxelSize,
                              VOLUME_MAX_DIM);
        const Render::Fluid::SplatGridDesc& desc = m_volumeSmokeGrid.Desc();
        m_volumeFlameGrid.Reset(desc);
        // A kernel narrower than ~1.5 cells lands in one or two cells, and the trilinear fetch then draws it as a
        // square blob (fresh flames on Bistro looked blocky). Widen such kernels and scale the weight by
        // (r / rMin)^3, which keeps each particle's integrated mass for the (1-s^2)^3 kernel.
        const float minRadius = VOLUME_MIN_KERNEL_CELLS * desc.cellSize;
        for (size_t p = 0; p < m_splatRadii.size(); ++p) {
            const float r = m_splatRadii[p];
            if (r < minRadius) {
                const float ratio = r / minRadius;
                m_splatWeights[p] *= ratio * ratio * ratio;
                m_splatRadii[p] = minRadius;
            }
        }
        if (flameCount > 0) {
            m_volumeFlameGrid.SplatAll(m_splatPositions.data(), flameCount, 3, m_splatRadii.data(), 0.0f,
                                       m_splatWeights.data(), m_splatAttributes.data());
        }
        if (count > flameCount) {
            m_volumeSmokeGrid.SplatAll(m_splatPositions.data() + size_t(flameCount) * 3, count - flameCount, 3,
                                       m_splatRadii.data() + flameCount, 0.0f, m_splatWeights.data() + flameCount,
                                       m_splatAttributes.data() + flameCount);
        }
        m_splatPositions.clear();
        m_splatRadii.clear();
        m_splatWeights.clear();
        m_splatAttributes.clear();

        m_volumeCells.resize(size_t(m_volumeSmokeGrid.CellCount()) * 4);
        const std::vector<float>& smokeDensity = m_volumeSmokeGrid.DensityData();
        const std::vector<float>& flameDensity = m_volumeFlameGrid.DensityData();
        float maxSmoke = 0.0f;
        float maxFlame = 0.0f;
        size_t i = 0;
        for (uint32_t z = 0; z < desc.dims[2]; ++z) {
            for (uint32_t y = 0; y < desc.dims[1]; ++y) {
                for (uint32_t x = 0; x < desc.dims[0]; ++x, ++i) {
                    // Soft cap cap*(1-e^(-d/cap)): many particles pile up near the source (raw sum ~35 on
                    // Bistro vs <1 at the plume edge), which made the core opaque and self-shadowed while only
                    // the thin rim was lit. Linear for small d, so thin smoke is unchanged.
                    m_volumeCells[i * 4 + 0] = VOLUME_SMOKE_DENSITY_CAP
                                             * (1.0f - std::exp(-smokeDensity[i] / VOLUME_SMOKE_DENSITY_CAP));
                    m_volumeCells[i * 4 + 1] = m_volumeSmokeGrid.Attribute(x, y, z);
                    m_volumeCells[i * 4 + 2] = flameDensity[i];
                    m_volumeCells[i * 4 + 3] = m_volumeFlameGrid.Attribute(x, y, z);
                    maxSmoke = (std::max)(maxSmoke, smokeDensity[i]);
                    maxFlame = (std::max)(maxFlame, flameDensity[i]);
                }
            }
        }
        m_lastVolumeMaxSmoke = maxSmoke;
        m_lastVolumeMaxFlame = maxFlame;
        for (int k = 0; k < 3; ++k) {
            m_lastVolumeDims[k] = desc.dims[k];
        }
        const bool uploaded = app.GetRenderer().SetParticleVolumeData(desc.origin, desc.cellSize, desc.dims,
                                                                      m_volumeCells.data(), ++m_volumeVersion);
        m_lastVolumeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return uploaded;
    }

    void WindEffectsOverlay::DrawSurfaces(ApplicationCore& app)
    {
        const auto start = std::chrono::steady_clock::now();
        m_flameMesh.Clear();
        m_smokeMesh.Clear();
        m_lastExtractMs = 0.0;
        uint32_t tris = 0;
        if (m_fireOn) {
            GatherSplat(m_flames, 0.0f, true);
            tris += BuildSurface(m_flameGrid, m_flameMesh);
        }
        if (m_smokeOn) {
            GatherSplat(m_smoke, 0.0f, false);
        }
        if (m_fireOn) {
            GatherSplat(m_fireSmoke, 1.0f, false);
        }
        tris += BuildSurface(m_smokeGrid, m_smokeMesh);
        m_lastSurfaceTris = tris;
        m_lastSurfaceMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

        if (m_surfaceScene) {
            if (m_sceneSurfaces.Update(app, m_flameMesh, FlameVertexColor, m_smokeMesh, SmokeVertexColor)) {
                m_sceneSurfacesEmpty = tris == 0;
                return;
            }
            // The models were deleted (scene load, editor); fall back to the ImGui surface.
            m_surfaceScene = false;
        }

        // DirectionFromYawPitch gives the direction the light travels; the shading wants the one toward it.
        const DirectionalLight light = app.GetDirectionalLight();
        float travel[3];
        Math::DirectionFromYawPitch(light.yaw, light.pitch, travel);
        IsoSurfaceDrawStyle flameStyle;
        flameStyle.color = FlameSurfaceColor;
        flameStyle.emissive = 0.85f;
        flameStyle.rim = 0.3f;
        IsoSurfaceDrawStyle smokeStyle;
        smokeStyle.color = SmokeSurfaceColor;
        smokeStyle.ambient = 0.55f;
        smokeStyle.rim = 0.35f;
        for (int k = 0; k < 3; ++k) {
            flameStyle.lightDirection[k] = -travel[k];
            smokeStyle.lightDirection[k] = -travel[k];
        }
        // Smoke first, flames over it: the meshes are sorted only within themselves, and with smoke on top the
        // dark fire smoke hid the flames it envelops.
        m_lastDrawn += DrawIsoSurfaceOverlay(m_smokeMesh, smokeStyle, g_view.proxy, g_view.origin, g_view.size,
                                             g_view.drawList, g_view.sdf, g_view.desc);
        m_lastDrawn += DrawIsoSurfaceOverlay(m_flameMesh, flameStyle, g_view.proxy, g_view.origin, g_view.size,
                                             g_view.drawList, g_view.sdf, g_view.desc);
    }

    void WindEffectsOverlay::ClearSceneSurfaces(ApplicationCore& app)
    {
        if (!m_surfaceScene || m_sceneSurfacesEmpty) {
            return;
        }
        const Render::Fluid::IsoSurfaceMesh empty;
        m_sceneSurfaces.Update(app, empty, FlameVertexColor, empty, SmokeVertexColor);
        m_sceneSurfacesEmpty = true;
    }

    std::string WindEffectsOverlay::SetSurfaceScene(ApplicationCore& app, bool on)
    {
        if (!on) {
            m_sceneSurfaces.Disable(app);
            m_surfaceScene = false;
            return "OK surface scene=0";
        }
        const WindFlowOverlay& wind = app.GetWindFlowOverlay();
        if (!wind.Field()) {
            return "ERR needs voxel.wind.overlay on";
        }
        // The voxel grid plus 8 voxels round it, and half its height again above it for rising smoke.
        const Render::Voxel::VoxelGridDesc& desc = wind.Desc();
        const float dims[3] = { float(desc.dimX), float(desc.dimY), float(desc.dimZ) };
        const float margin = 8.0f * desc.voxelSize;
        float boundsMin[3];
        float boundsMax[3];
        for (int c = 0; c < 3; ++c) {
            boundsMin[c] = desc.origin[c] - margin;
            boundsMax[c] = desc.origin[c] + dims[c] * desc.voxelSize + margin;
        }
        boundsMax[1] += 0.5f * dims[1] * desc.voxelSize;
        const std::string result = m_sceneSurfaces.Enable(app, boundsMin, boundsMax);
        if (result.rfind("OK", 0) == 0) {
            m_surface = true;
            m_surfaceScene = true;
            m_sceneSurfacesEmpty = true;
        }
        return result;
    }

    void WindEffectsOverlay::DrawPetals(ApplicationCore&)
    {
        ImDrawList* dl = g_view.drawList;
        for (uint32_t i = 0; i < m_petals.Count(); ++i) {
            const float* p = m_petals.Position(i);
            ImVec2 center;
            float s;
            if (!ProjectDisc(p, m_petals.Size(i), 1.5f, 12.0f, center, s) || Occluded(p)) {
                continue;
            }
            const float facing = (std::max)(m_petals.Facing(i), 0.15f);
            const float a = m_petals.Angle(i);
            const float c = std::cos(a);
            const float sn = std::sin(a);
            const ImVec2 ax(c * s, sn * s);
            const ImVec2 ay(-sn * s * 0.55f * facing, c * s * 0.55f * facing);
            const ImU32 color = Color(255.0f, 150.0f + static_cast<float>((i * 37u) % 41u), 200.0f, 0.95f);
            dl->AddQuadFilled(ImVec2(center.x - ax.x - ay.x, center.y - ax.y - ay.y),
                              ImVec2(center.x + ax.x - ay.x, center.y + ax.y - ay.y),
                              ImVec2(center.x + ax.x + ay.x, center.y + ax.y + ay.y),
                              ImVec2(center.x - ax.x + ay.x, center.y - ax.y + ay.y), color);
            ++m_lastDrawn;
        }
    }

    void WindEffectsOverlay::DrawSourceMarkers(const Render::Voxel::IWindVelocityField& field)
    {
        ImDrawList* dl = g_view.drawList;
        const float vs = g_view.desc.voxelSize;
        for (WindEffect effect : { WindEffect::Smoke, WindEffect::Fire, WindEffect::Petals }) {
            if (!IsActive(effect)) {
                continue;
            }
            const ImU32 color = effect == WindEffect::Smoke ? Color(200.0f, 200.0f, 210.0f, 1.0f)
                              : effect == WindEffect::Fire  ? Color(255.0f, 140.0f, 40.0f, 1.0f)
                                                            : Color(255.0f, 150.0f, 200.0f, 1.0f);
            float offset[3];
            EmitOffset(effect, offset);
            const Render::Voxel::WindEmitterSet& sources = Sources(effect);
            for (uint32_t i = 0; i < sources.Count(); ++i) {
                const float* p = sources.Position(i);
                ImVec2 base;
                if (!ProjectToOverlay(g_view.proxy, g_view.origin, g_view.size, p, base)) {
                    continue;
                }
                dl->AddCircle(base, 7.0f, Color(0.0f, 0.0f, 0.0f, 0.8f), 16, 4.0f);
                dl->AddCircle(base, 7.0f, color, 16, 2.0f);
                // Arrow along the horizontal-plus-vertical wind at the emission point, 6 voxels at the overlay's wind speed.
                const float at[3] = { p[0] + offset[0], p[1] + offset[1], p[2] + offset[2] };
                float u[3];
                field.Velocity(at, u);
                const float speed = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
                if (speed < 1e-4f || m_windSpeed <= 0.0f) {
                    continue;
                }
                const float len = 6.0f * vs * (std::min)(speed / m_windSpeed, 2.0f);
                const float tip[3] = { at[0] + u[0] / speed * len, at[1] + u[1] / speed * len, at[2] + u[2] / speed * len };
                ImVec2 a;
                ImVec2 b;
                if (!ProjectToOverlay(g_view.proxy, g_view.origin, g_view.size, at, a) ||
                    !ProjectToOverlay(g_view.proxy, g_view.origin, g_view.size, tip, b)) {
                    continue;
                }
                const float dx = b.x - a.x;
                const float dy = b.y - a.y;
                const float l = std::sqrt(dx * dx + dy * dy);
                dl->AddLine(base, a, color, 1.0f);
                dl->AddLine(a, b, Color(0.0f, 0.0f, 0.0f, 0.8f), 5.0f);
                dl->AddLine(a, b, color, 3.0f);
                if (l > 4.0f) {
                    const float hx = dx / l * 10.0f;
                    const float hy = dy / l * 10.0f;
                    dl->AddTriangleFilled(b, ImVec2(b.x - hx - 0.5f * hy, b.y - hy + 0.5f * hx),
                                          ImVec2(b.x - hx + 0.5f * hy, b.y - hy - 0.5f * hx), color);
                }
            }
        }
    }

    void WindEffectsOverlay::DrawControls(ApplicationCore& app)
    {
        ImGui::SeparatorText("Wind effects");
        WindFlowOverlay& wind = app.GetWindFlowOverlay();
        if (!wind.IsEnabled()) {
            UI::TextDisabled("Turn on the wind overlay first.");
            return;
        }
        bool tracers = wind.TracersVisible();
        if (ImGui::Checkbox("Show tracers", &tracers)) {
            wind.SetTracersVisible(tracers);
        }
        bool gpu = m_gpu;
        ImGui::SameLine();
        if (ImGui::Checkbox("GPU step", &gpu)) {
            m_lastMessage = SetGpu(app, gpu);
        }
        ImGui::SameLine();
        ImGui::Checkbox("Iso-surface", &m_surface);
        ImGui::SameLine();
        bool surfaceScene = m_surfaceScene;
        if (ImGui::Checkbox("Scene mesh", &surfaceScene)) {
            m_lastMessage = SetSurfaceScene(app, surfaceScene);
        }
        ImGui::SameLine();
        bool volume = m_volume;
        if (ImGui::Checkbox("Volume", &volume)) {
            SetVolume(app, volume);
        }
        if (UI::Button("Smoke here")) {
            m_lastMessage = Add(app, WindEffect::Smoke, WindEffectSettings{});
        }
        ImGui::SameLine();
        if (UI::Button("Fire here")) {
            m_lastMessage = Add(app, WindEffect::Fire, WindEffectSettings{});
        }
        ImGui::SameLine();
        if (UI::Button("Petals here")) {
            m_lastMessage = Add(app, WindEffect::Petals, WindEffectSettings{});
        }
        ImGui::SameLine();
        if (UI::Button("Clear")) {
            StopAll();
            m_lastMessage.clear();
        }
        UI::TextDisabled("Buttons add a source where the camera looks.");

        int placeEffect = static_cast<int>(m_placeEffect);
        ImGui::Checkbox("Click to place", &m_placing);
        ImGui::SameLine();
        ImGui::RadioButton("Smoke##place", &placeEffect, static_cast<int>(WindEffect::Smoke));
        ImGui::SameLine();
        ImGui::RadioButton("Fire##place", &placeEffect, static_cast<int>(WindEffect::Fire));
        ImGui::SameLine();
        ImGui::RadioButton("Petals##place", &placeEffect, static_cast<int>(WindEffect::Petals));
        m_placeEffect = static_cast<WindEffect>(placeEffect);
        ImGui::SameLine();
        if (UI::Button("Undo")) {
            m_lastMessage = UndoLast(m_placeEffect);
        }
        if (m_placing) {
            UI::TextDisabled("Left-click the scene to add a source (right drag still moves the camera).");
        }
        if (m_lastMessage.rfind("ERR", 0) == 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", m_lastMessage.c_str());
        } else if (!m_lastMessage.empty()) {
            UI::TextDisabled("%s", m_lastMessage.c_str());
        }
        UI::TextDisabled("smoke %u  petals %u  burning %u  burnt %u", m_smoke.Count(), m_petals.Count(),
                         m_fire.Stats().burning, m_fire.Stats().burnt);
    }
}
