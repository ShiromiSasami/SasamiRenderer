#pragma once

#include "Cases/MeshReorder.h"
#include "GpuClockProbe.h"
#include "PerfCase.h"
#include "Renderer/Structures/Vertex.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace SasamiEngine::App
{
    class ApplicationCore;
    class StaticModel;
}

namespace SasamiEngine::Sample::PerfLab
{
    // GPU time of the GBuffer pass ("OpaqueGBuffer" in Renderer::GetGpuPassTimings) for the loader
    // order (Baseline) and the meshopt vertex cache + overdraw + fetch order (Optimized) of the same
    // welded mesh, drawn as one StaticModel from a fixed camera. Each variant run swaps the mesh in,
    // waits for the profiler latency, then takes the median of kSampleFrames frames as gpuMs, so one
    // run spans several frames (StepVariant). The mesh shader path is turned off while the case runs
    // (it would replace the vertex path being measured). extras are meshopt analyze*, whose header says
    // they do not match real GPUs; gpuMs is the measured counterpart. inside = the camera sits at the
    // mesh centre (Sponza), otherwise the mesh is framed in front of it (Bunny).
    // gpuClockMHz / memClockMHz (medians) and pstate (the worst, i.e. highest, NVML P-state) are
    // sampled on the CPU next to each GPU sample (GpuClockProbe, NVIDIA only; absent elsewhere), so
    // PerfCaseRunner can drop runs taken in a lower power state.
    // mode picks which meshopt steps make the Optimized order (MeshReorderMode), to split a frame
    // difference between the overdraw reorder and the vertex cache / fetch steps. extras ms_<pass>
    // are per-pass medians (scope name without the "_<index>" suffix), shadowGpuMs the Shadow* sum.
    // Check: both variants hold the same triangles (CompareMeshOrder) and every run got timings.
    class GBufferDrawCase : public IPerfCase
    {
    public:
        GBufferDrawCase(std::string name, App::ApplicationCore& app, std::string assetPath, bool inside,
                        MeshReorderMode mode = MeshReorderMode::Full);

        const char* Name() const override { return m_name.c_str(); }
        const char* Description() const override { return m_description.c_str(); }

        bool Prepare(std::string& error) override;
        PerfMetrics RunVariant(PerfVariant variant) override;
        bool StepVariant(PerfVariant variant, PerfMetrics& out) override;
        bool Check(std::string& detail) override;
        void Release() override;

    private:
        static constexpr uint32_t kSettleFrames = 30;
        static constexpr uint32_t kSampleFrames = 60;

        void ShowVariant(PerfVariant variant);

        std::string m_name;
        App::ApplicationCore& m_app;
        std::string m_assetPath;
        bool m_inside = false;
        MeshReorderMode m_mode = MeshReorderMode::Full;
        std::string m_description;

        std::vector<Render::Vertex> m_vertices[2];
        std::vector<uint32_t> m_indices[2];
        std::vector<PerfExtraMetric> m_extras[2];
        float m_translation[3] = {};

        App::StaticModel* m_model = nullptr;
        bool m_savedMeshShader = false;
        bool m_running = false;
        uint32_t m_frame = 0;
        uint64_t m_lastTimingFrame = 0;
        std::vector<double> m_samples;
        std::vector<double> m_otherSamples; // sum of every other pass, same frames
        std::vector<double> m_frameSamples; // GBuffer + every other pass, same frames
        std::map<std::string, std::vector<double>> m_passSamples; // per pass prefix, same frames
        std::vector<double> m_shadowSamples; // sum of the Shadow* passes, same frames
        double m_maxGBufferSeen = 0.0;
        double m_frameAtGpuMax = -1.0;      // frame total of the frame with the largest GBuffer time
        std::unique_ptr<GpuClockProbe> m_clockProbe;
        std::vector<double> m_gpuClocks;
        std::vector<double> m_memClocks;
        int m_worstPState = -1;
        uint32_t m_missingRuns = 0;
    };
}
