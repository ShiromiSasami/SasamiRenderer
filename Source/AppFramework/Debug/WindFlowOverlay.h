#pragma once

#include "Renderer/Voxel/AsyncWindowProjection.h"
#include "Renderer/Voxel/GustedWindField.h"
#include "Renderer/Voxel/ProjectedWindField.h"
#include "Renderer/Voxel/SdfWindField.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"
#include "Renderer/Voxel/WindParticleSystem.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        struct WindFlowOverlaySettings
        {
            uint32_t count = 2000;
            bool projectMode = true;
            float wind[3] = { 0.0f, 0.0f, 0.0f };   // m/s; zero picks extent / 10 along +x at Enable
            float turbulence = -1.0f;               // < 0 picks the mode default (curl 0.3, project 0)
            float timeScale = 1.0f;                 // simulated seconds per real second
            bool occlude = true;                    // hide particles behind voxel solids (one SDF ray each)
            bool dashed = true;                     // dashes anchored to each particle, so they flow with it
            uint32_t micSlabs = 8;                  // PressureProjectionSettings::micSlabs (Bistro 128: 973 -> 423 ms build)
            // false: keep the field and its clock running but neither step nor draw the tracers
            // (WindEffectsOverlay still uses the field).
            bool tracers = true;
            // true (source=stream, project mode only): the field covers the voxel.stream window around
            // the camera instead of the voxel.scene grid, and is re-projected on the JobSystem whenever
            // the camera chunk changes (6b stage 3b). Needs `voxel.stream on sdf=A`.
            bool streamWindow = false;
        };

        // Wind tracer particles drawn over the normal scene (Discord request, 2026-10-22). Builds the
        // SDF and the wind field of `voxel.wind run` once from the latest voxel.scene grid, then every
        // frame advects the particles by the frame time and draws their trails as lines into ImGui's
        // background draw list, projected with the main camera. That is backend independent but has no
        // depth buffer: occlusion is a CPU sphere trace against the voxel SDF from the camera to each
        // particle head, so it is as coarse as the voxel grid. Main thread only; Draw is registered as
        // an ImGuiCoordinator overlay.
        class WindFlowOverlay
        {
        public:
            // Builds the field and spawns the particles. Returns "OK ..." or "ERR ...".
            std::string Enable(ApplicationCore& app, const WindFlowOverlaySettings& settings);
            void Disable();
            bool IsEnabled() const { return m_enabled; }
            std::string Status() const;

            void Draw(ApplicationCore& app);

            // Borrowed by WindEffectsOverlay. Field()/Sdf() are null while disabled and change on every
            // Enable/Disable, which bumps Generation().
            const Render::Voxel::IWindVelocityField* Field() const { return m_enabled ? &ActiveField() : nullptr; }
            const Render::Voxel::SignedDistanceGrid* Sdf() const { return m_enabled ? &ActiveSdf() : nullptr; }
            // The steady projected field without gusts; null in curl mode or while disabled.
            const Render::Voxel::ProjectedWindField* ProjectedField() const { return m_enabled ? m_projected.get() : nullptr; }
            const Render::Voxel::VoxelGridDesc& Desc() const { return m_desc; }
            const float* WindVector() const { return m_settings.wind; }
            float WindSpeed() const { return m_windSpeed; }
            // Sim seconds Draw advanced this frame (0 when paused or disabled); overlays drawn after it step by the same dt.
            float FrameDt() const { return m_lastDt; }
            uint64_t Generation() const { return m_generation; }
            void SetTracersVisible(bool visible) { m_settings.tracers = visible; }
            bool TracersVisible() const { return m_settings.tracers; }

        private:
            const Render::Voxel::IWindVelocityField& ActiveField() const;
            // m_field, or the stream window's clamped SDF.
            const Render::Voxel::SignedDistanceGrid& ActiveSdf() const;
            // (Re)creates the gust noise on ActiveSdf() and m_projected when turbulence > 0.
            void BuildGusts();
            // Installs a projected window as the active field and bumps Generation().
            void InstallStreamWindow(Render::Voxel::ProjectedWindowField&& result);
            // Collects a finished background projection, or kicks one when the camera chunk changed.
            void UpdateStreamWindow(ApplicationCore& app);

            bool m_enabled = false;
            WindFlowOverlaySettings m_settings;
            Render::Voxel::VoxelGridDesc m_desc;
            float m_windSpeed = 0.0f;
            float m_maxStep = 0.0f;
            float m_time = 0.0f;
            float m_lastDt = 0.0f;
            uint64_t m_generation = 0;
            uint64_t m_frames = 0;
            double m_lastStepMs = 0.0;
            double m_lastDrawMs = 0.0;
            uint32_t m_lastVisible = 0;
            uint32_t m_lastVertices = 0;
            // Per-frame screen trail of every particle (TRAIL_LENGTH xy pairs each) and how many of
            // its points are drawable (0 = culled or occluded); filled in parallel, drawn serially.
            std::vector<float> m_screenXy;
            std::vector<uint8_t> m_screenPoints;
            // The fields keep references into m_field / m_streamWindow->sdf, so those are heap-held and destroyed last.
            std::unique_ptr<Render::Voxel::SignedDistanceGrid> m_field;
            std::unique_ptr<Render::Voxel::StreamedVoxelWindow> m_streamWindow;
            Render::Voxel::SdfWindSettings m_gustSettings;
            Render::Voxel::ProjectedWindSettings m_projectSettings;
            // Stream window state (streamWindow only).
            int32_t m_streamCentre[3] = { 0, 0, 0 };
            uint64_t m_streamRebuilds = 0;
            uint64_t m_streamWarmCells = 0;
            uint32_t m_streamMissing = 0;      // chunks not loaded at the last assembly attempt
            double m_streamAssembleMs = 0.0;   // main thread
            double m_streamProjectMs = 0.0;    // on the worker
            std::unique_ptr<Render::Voxel::SdfWindField> m_curl;
            std::unique_ptr<Render::Voxel::ProjectedWindField> m_projected;
            std::unique_ptr<Render::Voxel::SdfWindField> m_gustNoise;
            std::unique_ptr<Render::Voxel::GustedWindField> m_gusted;
            Render::Voxel::WindParticleSystem m_particles;
            // Declared last so its destructor waits for the job before the fields go away.
            Render::Voxel::AsyncWindowProjection m_async;
        };
    }
}
namespace SasamiEngine { using namespace App; }
