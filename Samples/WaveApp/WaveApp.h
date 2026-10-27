#pragma once

#include "IApplication.h"
#include "Object/Camera.h"
#include "Object/StaticModel.h"
#include "CoastalWindField.h"
#include "GerstnerWaveSet.h"
#include "WindFoliage.h"
#include "WindWaveSpectrum.h"
#include "OceanSurfaceMesh.h"
#include "FlipWaterSolver.h"
#include "FlipDiffuseMesh.h"
#include "FlipHeightFieldMesh.h"
#include "FlipDiffuseParticles.h"
#include <cstdint>
#include <string>
#include <vector>
#include <boost/signals2/connection.hpp>

namespace SasamiEngine::Sample::Wave
{
    // Playground for wave rendering research. Stage 1: Gerstner swell over a sloped beach, with
    // Jacobian whitecap foam and a shore surf band. Stage 2: a FLIP water tank over the same beach
    // (optionally with the tall-cell pressure basis), drawn as a column height field. The water is a
    // CPU mesh whose vertices are rewritten every frame. Wind: one CoastalWindField drives both the
    // wave spectrum / gust ripples and the dune grass, sampled at the same time, so they stay in sync.
    class WaveApp : public IApplication
    {
    public:
        void OnInit(ApplicationCore& app) override;
        void OnUpdate(ApplicationCore& app, float deltaTime) override;
        void OnRender(ApplicationCore& app) override;
        void OnShutdown(ApplicationCore& app) override;

    private:
        void BuildBeach();
        void BuildRocks();
        void PushWaterMesh(ApplicationCore& app);
        bool WriteWaterVertices(ApplicationCore& app);
        bool WriteMeshVertices(ApplicationCore& app, const Mesh& mesh, size_t& meshIndex, float* outMs);
        void UpdateFoliage(ApplicationCore& app);
        void UpdateDiffuseMesh(ApplicationCore& app);
        void RebuildWindWaves();
        std::string WindStatusLine() const;
        std::string WindCommand(const std::vector<std::string>& args);
        void DrawWindSection();
        void DrawWaveTab(ApplicationCore& app);
        void RegisterCommands(ApplicationCore& app);
        std::string StatusLine() const;
        std::string ApplySetting(const std::string& key, const std::string& value);
        const Mesh& CurrentWaterMesh() const;
        void SetFlipMode(ApplicationCore& app, bool on);
        void ResetFlip();
        void DriveFlipPaddle();
        std::string FlipStatusLine() const;
        std::string DiffuseStatusLine() const;
        std::string DiffuseCommand(const std::vector<std::string>& args);
        std::string FlipCommand(ApplicationCore& app, const std::vector<std::string>& args);

        std::vector<boost::signals2::scoped_connection> m_inputConnections;
        Camera* m_camera = nullptr;
        StaticModel* m_beachModel = nullptr;
        StaticModel* m_waterModel = nullptr;
        StaticModel* m_foliageModel = nullptr;
        StaticModel* m_rockModel = nullptr;   // m_flipSettings.rocks, drawn in both modes
        StaticModel* m_diffuseModel = nullptr;   // foam c: m_diffuseMesh, empty outside FLIP mode

        GerstnerWaveSet m_waves = GerstnerWaveSet::MakeDefault();
        std::vector<GerstnerWave> m_baseWaves;   // MakeDefault() or wind-spectrum values; amplitude/steepness scales apply to these
        OceanSurfaceMesh m_ocean;
        float m_amplitudeScale = 1.0f;
        float m_steepnessScale = 1.0f;
        float m_time = 0.0f;
        bool m_paused = false;
        bool m_resolutionDirty = false;

        float m_updateMs = 0.0f;   // CPU: Gerstner evaluate + foam
        float m_pushMs = 0.0f;     // CPU: vertex write into the uploaded buffer (or the full rebuild when it had to)
        // Index of the water mesh in SceneSubmitter::GetMeshes(), found after the first submit; SIZE_MAX = unknown.
        size_t m_waterMeshIndex = SIZE_MAX;
        uint32_t m_fullPushes = 0;   // full rebuilds (init, resolution change, failed in-place writes)

        // Wind (shared by waves and foliage).
        CoastalWindField m_wind;
        WindWaveSpectrumSettings m_spectrumSettings;   // speed/direction are copied from m_wind on rebuild
        WindWaveSpectrumInfo m_spectrumInfo;
        bool m_windWaves = true;       // swell from the wind spectrum + gust ripples; off = MakeDefault waves
        bool m_windDirty = false;      // spectrum inputs changed; rebuild the waves before the next update
        WindFoliage m_foliage;
        bool m_foliageOn = true;
        size_t m_foliageMeshIndex = SIZE_MAX;
        float m_foliageMs = 0.0f;      // CPU: blade bend
        float m_foliagePushMs = 0.0f;

        // Stage 2: FLIP tank. While m_flipMode is set the water model shows m_flipMesh instead of the ocean.
        bool m_flipMode = false;
        bool m_flipResetPending = false;   // settings changed from the UI; reset before the next step
        FlipWaterSettings m_flipSettings;
        FlipWaterSolver m_flip;
        FlipHeightFieldMesh m_flipMesh;
        FlipDiffuseParticles m_diffuse;   // foam b: spray/foam/bubbles from the tank (Ihmsen 2012)
        FlipDiffuseMesh m_diffuseMesh;    // foam c: octahedron per drawn diffuse particle
        size_t m_diffuseMeshIndex = SIZE_MAX;
        float m_diffuseMeshMs = 0.0f;   // CPU: diffuse mesh vertex update
        float m_diffusePushMs = 0.0f;
        float m_flipOceanGain = 1.0f;    // stage 5a: paddle u = gain * Gerstner inlet velocity
        float m_flipOceanDecay = 0.0f;   // 1/m, last depth decay passed to SetPaddleDrive
        float m_flipMs = 0.0f;   // CPU: solver Step + height-field mesh update
    };
}
