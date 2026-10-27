#pragma once

#include "AppFramework/Debug/IsoSurfaceSceneMeshes.h"
#include "Renderer/Fluid/IsoSurfaceMesh.h"
#include "Renderer/Fluid/ParticleSplatGrid.h"
#include "Renderer/Voxel/WindDrivenParticles.h"
#include "Renderer/Voxel/WindEmitterSet.h"
#include "Renderer/Voxel/WindFireSpread.h"

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace SasamiEngine::Render
{
    class IRhiCommandEncoder;
}

namespace SasamiEngine::Render::Voxel
{
    class GpuWindParticleStepper;
    class GpuWindParticleStream;
}

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        enum class WindEffect : uint8_t
        {
            Smoke,
            Fire,
            Petals,
        };

        struct WindEffectSettings
        {
            // World position of the source. hasPosition=false: the surface point the main camera looks
            // at (an SDF ray along its forward axis), or 20 voxels ahead when the ray misses.
            bool hasPosition = false;
            float position[3] = { 0.0f, 0.0f, 0.0f };
            // Screen pixel (relative to the main viewport) whose camera ray picks the surface; used when
            // hasPosition is false. A miss is an error, unlike the camera-forward default.
            bool hasScreen = false;
            float screen[2] = { 0.0f, 0.0f };
            uint32_t count = 0;     // max particles; 0 = effect default
            float rate = -1.0f;     // smoke/petal spawn rate per source per second, fire smoke per burning cell; < 0 = default
                                    // (only the first source of an effect sets count and rate)
            float radius = -1.0f;   // fire only: max spread distance from any fire source, in voxels; 0 = unlimited,
                                    // < 0 = default (12); set by the first source like count and rate
        };

        // Smoke, fire spread and petal showers carried by the wind of WindFlowOverlay (which must be on).
        // Every frame, after WindFlowOverlay::Draw has advanced the wind clock, it steps the effects by the
        // same dt and draws them into ImGui's background draw list:
        //   smoke  - WindDrivenParticles with buoyancy, grey discs that grow and fade;
        //   fire   - WindFireSpread over the voxel surface. Each burning cell emits short-lived flame puffs
        //            (buoyant, wind-bent teardrop tongues coloured by a blackbody ramp, glow/body/core layers),
        //            embers (tiny bright streaks) and smoke above them; burnt cells stay as dark scorch marks;
        //   petals - WindDrivenParticles with gravity, flutter and spin, pink quads that flip.
        // Occlusion is the same per-point SDF ray as the tracers (voxel-accurate only). CPU only. When the
        // wind overlay is re-enabled or disabled (its Generation() changes) every effect is cleared.
        // Each effect can have several sources (Add, or a left click in the viewport while placement is
        // armed from the UI). Every source gets a marker and an arrow along the local wind, and Status()
        // reports, per source, how well its smoke/petals/burnt cells have drifted downwind.
        class WindEffectsOverlay
        {
        public:
            WindEffectsOverlay();
            ~WindEffectsOverlay();

            // 6c-2: steps smoke and petals on the GPU inside the frame's command list (needs
            // voxel.wind.overlay on mode=project); the CPU copy is refreshed from a readback READ_LAG
            // records old. Fire stays on the CPU. Returns "OK ..." or "ERR ...".
            std::string SetGpu(ApplicationCore& app, bool on);
            bool IsGpu() const { return m_gpu; }
            // Iso-surface mode: flames and smoke are splatted into a density grid and drawn as its level
            // set (ParticleSplatGrid + ExtractIsoSurface) instead of sprites. Embers and petals stay points.
            void SetSurface(bool on) { m_surface = on; }
            bool IsSurface() const { return m_surface; }
            // Scene-mesh variant of the iso-surface mode: the surfaces go into two StaticModels drawn by the
            // renderer (IsoSurfaceSceneMeshes) instead of ImGui. On turns the iso-surface mode on too; needs the
            // wind overlay (its grid bounds the models). Returns "OK ..." or "ERR ...".
            std::string SetSurfaceScene(ApplicationCore& app, bool on);
            bool IsSurfaceScene() const { return m_surfaceScene; }
            // Volume mode: flames and smoke are splatted into one float4 grid (smoke, soot, flame, flame age)
            // that ParticleVolumeRenderPass ray-marches in the frame (Hillaire 2015 integration, blackbody
            // flames). Replaces the flame/smoke sprites and the iso-surface; embers and petals stay points.
            void SetVolume(ApplicationCore& app, bool on);
            bool IsVolume() const { return m_volume; }

            // Returns "OK ..." or "ERR ...". Restarts the effect with this one source.
            std::string Start(ApplicationCore& app, WindEffect effect, const WindEffectSettings& settings);
            // Adds a source, starting the effect if it was off.
            std::string Add(ApplicationCore& app, WindEffect effect, const WindEffectSettings& settings);
            // Removes the newest source (smoke/petals stop emitting there; a fire keeps burning).
            std::string UndoLast(WindEffect effect);
            // Arms (or disarms) click placement in the viewport.
            void SetPlacement(bool armed, WindEffect effect) { m_placing = armed; m_placeEffect = effect; }
            bool IsPlacementArmed() const { return m_placing; }
            void Stop(WindEffect effect);
            void StopAll();
            bool IsActive(WindEffect effect) const;
            std::string Status(ApplicationCore& app) const;

            // Steps and draws; a no-op while every effect is off.
            void Draw(ApplicationCore& app);
            // Buttons for the Voxel / Wind tab.
            void DrawControls(ApplicationCore& app);

        private:
            bool ResolvePosition(ApplicationCore& app, const WindEffectSettings& settings, float outPosition[3], std::string& error) const;
            // Surface point under a viewport pixel (SDF ray from the main camera); false on a miss.
            bool PickSurface(ApplicationCore& app, float screenX, float screenY, float outPosition[3]) const;
            void HandlePlacementClick(ApplicationCore& app);
            Render::Voxel::WindEmitterSet& Sources(WindEffect effect);
            const Render::Voxel::WindEmitterSet& Sources(WindEffect effect) const;
            // Where a source's particles are born (and where its wind arrow is sampled), relative to the source.
            void EmitOffset(WindEffect effect, float outOffset[3]) const;
            void AppendSourceReport(std::ostringstream& out, WindEffect effect, const Render::Voxel::IWindVelocityField& field) const;
            void DrawSourceMarkers(const Render::Voxel::IWindVelocityField& field);
            void DrawSmoke(ApplicationCore& app, const Render::Voxel::WindDrivenParticles& smoke, float alphaScale);
            void DrawFire(ApplicationCore& app);
            void EmitFire(float dt);
            void DrawPetals(ApplicationCore& app);
            // Iso-surface mode: flame surface (blackbody by age) and one smoke surface for m_smoke + m_fireSmoke.
            void DrawSurfaces(ApplicationCore& app);
            // Scene-mesh mode, on frames that build no surface: empties the models once.
            void ClearSceneSurfaces(ApplicationCore& app);
            // Splats the gathered m_splat* particles into `grid`, extracts the surface into `mesh`; returns its triangles.
            // Splats flames and smoke into m_volumeFlameGrid / m_volumeSmokeGrid on one shared grid and
            // hands the packed cells to the renderer. Returns false when there is nothing to draw.
            bool BuildVolume(ApplicationCore& app);
            uint32_t BuildSurface(Render::Fluid::ParticleSplatGrid& grid, Render::Fluid::IsoSurfaceMesh& mesh);
            void GatherSplat(const Render::Voxel::WindDrivenParticles& particles, float attribute, bool useAge);
            // GPU mode: queue this frame's spawns, set the step params and import the readback.
            void StepGpu(ApplicationCore& app, Render::Voxel::WindDrivenParticles& particles,
                         Render::Voxel::WindDrivenParticles& spawner, Render::Voxel::WindEmitterSet& sources,
                         std::unique_ptr<Render::Voxel::GpuWindParticleStream>& stream,
                         float rate, float radius, const float offset[3], float dt);
            void RecordGpu(Render::IRhiCommandEncoder& encoder);
            void WaitGpuBeforeRelease();
            void RemoveGpuHook();

            bool m_smokeOn = false;
            bool m_fireOn = false;
            bool m_petalsOn = false;
            uint64_t m_windGeneration = 0;
            uint64_t m_frames = 0;
            bool m_placing = false;
            WindEffect m_placeEffect = WindEffect::Smoke;
            float m_smokeRate = 300.0f;             // per source per second
            float m_petalRate = 250.0f;
            float m_fireSmokeRate = 1.5f;           // per burning cell per second
            float m_fireSmokeCarry = 0.0f;
            float m_flameCarry = 0.0f;
            float m_emberCarry = 0.0f;
            float m_flameTime = 0.0f;               // seconds since the fire started, drives the tongue wobble
            float m_voxelSize = 1.0f;               // of the wind overlay, refreshed by Add and Draw
            float m_windSpeed = 0.0f;               // the overlay's wind speed, scales the marker arrows
            double m_lastStepMs = 0.0;
            double m_lastDrawMs = 0.0;
            uint32_t m_lastDrawn = 0;
            bool m_surface = false;
            uint32_t m_lastSurfaceTris = 0;
            double m_lastSurfaceMs = 0.0;           // splat + extract of both surfaces, inside drawMs
            double m_lastExtractMs = 0.0;           // ExtractIsoSurface of both surfaces, part of m_lastSurfaceMs
            bool m_surfaceScene = false;
            bool m_sceneSurfacesEmpty = true;       // the models hold no triangle, ClearSceneSurfaces can skip the push
            IsoSurfaceSceneMeshes m_sceneSurfaces;
            Render::Fluid::ParticleSplatGrid m_flameGrid;
            Render::Fluid::ParticleSplatGrid m_smokeGrid;
            Render::Fluid::IsoSurfaceMesh m_flameMesh;
            Render::Fluid::IsoSurfaceMesh m_smokeMesh;
            bool m_volume = false;
            bool m_volumeDrawn = false;             // this frame's volume was built, DrawFire skips flame sprites
            uint64_t m_volumeVersion = 0;
            uint32_t m_lastVolumeDims[3] = { 0, 0, 0 };
            double m_lastVolumeMs = 0.0;            // splat + pack, inside drawMs
            float m_lastVolumeMaxSmoke = 0.0f;      // raw splat sum before saturation
            float m_lastVolumeMaxFlame = 0.0f;
            Render::Fluid::ParticleSplatGrid m_volumeFlameGrid;
            Render::Fluid::ParticleSplatGrid m_volumeSmokeGrid;
            std::vector<float> m_volumeCells;       // float4 per cell
            std::vector<float> m_splatPositions;    // gather scratch for BuildSurface
            std::vector<float> m_splatRadii;
            std::vector<float> m_splatWeights;
            std::vector<float> m_splatAttributes;
            std::string m_lastMessage;              // shown under the UI buttons
            Render::Voxel::WindDrivenParticles m_smoke;
            Render::Voxel::WindDrivenParticles m_fireSmoke;
            Render::Voxel::WindDrivenParticles m_flames;
            Render::Voxel::WindDrivenParticles m_embers;
            Render::Voxel::WindDrivenParticles m_petals;
            Render::Voxel::WindFireSpread m_fire;
            Render::Voxel::WindEmitterSet m_smokeSources;
            Render::Voxel::WindEmitterSet m_petalSources;
            Render::Voxel::WindEmitterSet m_fireSources;   // ignition points, for markers and the report

            bool m_gpu = false;
            ApplicationCore* m_hookApp = nullptr;   // set while the frame compute hook is installed
            std::unique_ptr<Render::Voxel::GpuWindParticleStepper> m_gpuStepper;
            std::unique_ptr<Render::Voxel::GpuWindParticleStream> m_gpuSmoke;
            std::unique_ptr<Render::Voxel::GpuWindParticleStream> m_gpuPetals;
            Render::Voxel::WindDrivenParticles m_smokeSpawn;    // per-frame spawn scratch, cleared after every export
            Render::Voxel::WindDrivenParticles m_petalSpawn;
            std::vector<Render::Voxel::WindParticleState> m_gpuSpawnStates;
            std::vector<Render::Voxel::WindParticleState> m_gpuReadback;
            std::string m_gpuError;
        };
    }
}
namespace SasamiEngine { using namespace App; }
