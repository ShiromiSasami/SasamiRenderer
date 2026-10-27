#include "Cases/CrowdCase.h"

#include "ApplicationCore.h"
#include "AppFramework/ApplicationResourcePaths.h"
#include "AppFramework/Loader/ModelLoader.h"
#include "Renderer/Crowd/CrowdSystem.h"
#include "Renderer/Crowd/VatAnimation.h"
#include "Renderer/Runtime/Renderer.h"

#include <algorithm>
#include <cstdio>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        using Render::Crowd::CrowdGpuStepMode;
        using Render::Crowd::CrowdUpdateMode;

        // Same pass-name rule as GBufferDrawCase: RenderGraph appends "_<pass index>".
        bool IsCrowdScope(const std::string& scopeName)
        {
            const std::string prefix = "Crowd_";
            return scopeName.size() > prefix.size()
                && scopeName.compare(0, prefix.size(), prefix) == 0
                && std::all_of(scopeName.begin() + prefix.size(), scopeName.end(),
                    [](char c) { return c >= '0' && c <= '9'; });
        }

        // As LoadFoxCrowdAsset in DebugCrowdCommands.cpp (Vertex mode, 30 Hz).
        bool LoadFoxAsset(Render::Crowd::CrowdSystem& crowd, std::string& error)
        {
            const std::string path = ApplicationResourcePaths::ResolveAssetPathString("Models/Fox/Fox.gltf");
            SkinnedModelData model;
            if (!LoadGLTFSkinned(path, model)) {
                error = "load failed " + path;
                return false;
            }
            Render::Crowd::VatBakeDesc desc;
            desc.mode = Render::Crowd::VatMode::Vertex;
            desc.sampleRate = 30.0f;
            Render::Crowd::VatBakedAnimation baked;
            std::string bakeError;
            if (!Render::Crowd::BakeVat(model.meshes, model.skeleton, model.animations, desc, baked, bakeError)) {
                error = "bake: " + bakeError;
                return false;
            }
            if (!crowd.SetAsset(model.meshes, baked, bakeError)) {
                error = "set asset: " + bakeError;
                return false;
            }
            return true;
        }

        double Median(std::vector<double>& values)
        {
            std::sort(values.begin(), values.end());
            return values[values.size() / 2];
        }
    }

    CrowdCase::CrowdCase(std::string name, App::ApplicationCore& app, CrowdCaseKind kind)
        : m_name(std::move(name))
        , m_app(app)
        , m_kind(kind)
    {
        m_description = (m_kind == CrowdCaseKind::Update)
            ? "100k VAT crowd: CPU update vs GPU update (culled), frame GPU/CPU ms"
            : "100k VAT crowd: GPU update, culling off vs on, frame GPU/CPU ms";
    }

    bool CrowdCase::Prepare(std::string& error)
    {
        Render::Renderer& renderer = m_app.GetRenderer();
        Render::Crowd::CrowdSystem& crowd = renderer.GetCrowdSystem();
        if (!crowd.IsInitialized()) {
            error = "crowd system not initialized (DX12 only)";
            return false;
        }
        if (!crowd.HasAsset() && !LoadFoxAsset(crowd, error)) {
            return false;
        }
        m_savedEnabled = renderer.GetCrowdEnabled();
        m_savedMode = static_cast<uint32_t>(crowd.GetUpdateMode());
        m_savedStepMode = static_cast<uint32_t>(crowd.GetGpuStepMode());
        m_savedCull = crowd.CullSettings().enabled;
        m_savedHeight = crowd.GetTargetHeight();

        crowd.SetGpuStepMode(CrowdGpuStepMode::Recorded);
        crowd.SetTargetHeight(1.0f);
        if (!crowd.Spawn(kAgentCount, kHalfExtent, 1, error)) {
            return false;
        }
        renderer.SetCrowdEnabled(true);
        m_clockProbe = std::make_unique<GpuClockProbe>();
        m_runs[0] = m_runs[1] = 0;
        m_timedRuns[0] = m_timedRuns[1] = 0;
        m_lastError.clear();
        m_running = false;
        m_prepared = true;
        return true;
    }

    bool CrowdCase::ApplyVariant(PerfVariant variant, std::string& error)
    {
        Render::Crowd::CrowdSystem& crowd = m_app.GetRenderer().GetCrowdSystem();
        const bool optimized = variant == PerfVariant::Optimized;
        CrowdUpdateMode mode = CrowdUpdateMode::Gpu;
        bool cull = true;
        if (m_kind == CrowdCaseKind::Update) {
            mode = optimized ? CrowdUpdateMode::Gpu : CrowdUpdateMode::Cpu;
        } else {
            cull = optimized;
        }
        crowd.CullSettings().enabled = cull;
        return crowd.SetUpdateMode(mode, error);
    }

    PerfMetrics CrowdCase::RunVariant(PerfVariant variant)
    {
        // Single-frame fallback; the runner uses StepVariant.
        PerfMetrics metrics;
        while (!StepVariant(variant, metrics)) {
        }
        return metrics;
    }

    bool CrowdCase::StepVariant(PerfVariant variant, PerfMetrics& out)
    {
        Render::Renderer& renderer = m_app.GetRenderer();
        const auto now = std::chrono::steady_clock::now();
        if (!m_running) {
            std::string error;
            if (!ApplyVariant(variant, error)) {
                m_lastError = error;
            }
            m_running = true;
            m_frame = 0;
            m_frameSamples.clear();
            m_crowdSamples.clear();
            m_wallSamples.clear();
            m_gpuClocks.clear();
            m_memClocks.clear();
            m_worstPState = -1;
            m_lastTimingFrame = renderer.GetGpuTimingFrameCounter();
            m_lastFrameTime = now;
            return false;
        }
        ++m_frame;
        const uint32_t settle = Render::GpuTimestampProfiler::kFrameLatency + kSettleFrames;
        if (m_frame > settle) {
            m_wallSamples.push_back(std::chrono::duration<double, std::milli>(now - m_lastFrameTime).count());
        }
        m_lastFrameTime = now;

        const uint64_t timingFrame = renderer.GetGpuTimingFrameCounter();
        if (m_frame > settle && timingFrame != m_lastTimingFrame) {
            double crowdMs = -1.0;
            double total = 0.0;
            for (const auto& scope : renderer.GetGpuPassTimings()) {
                if (IsCrowdScope(scope.name)) {
                    crowdMs = scope.milliseconds;
                }
                total += scope.milliseconds;
            }
            if (crowdMs >= 0.0) {
                m_crowdSamples.push_back(crowdMs);
                m_frameSamples.push_back(total);
                GpuClockSample clock;
                if (m_clockProbe && m_clockProbe->Sample(clock)) {
                    m_gpuClocks.push_back(clock.graphicsMHz);
                    m_memClocks.push_back(clock.memoryMHz);
                    m_worstPState = (std::max)(m_worstPState, clock.pstate);
                }
            }
        }
        m_lastTimingFrame = timingFrame;
        const bool timedOut = m_frame > settle + 3 * kSampleFrames;
        if (m_crowdSamples.size() < kSampleFrames && !timedOut) {
            return false;
        }

        const uint32_t v = static_cast<uint32_t>(variant);
        ++m_runs[v];
        out = PerfMetrics{};
        Render::Crowd::CrowdSystem& crowd = renderer.GetCrowdSystem();
        out.gpuBytes = uint64_t(crowd.AgentCount()) * 64ull; // GpuCrowdAgent is 64 B (not asserted here)
        if (!m_wallSamples.empty()) {
            out.cpuMs = Median(m_wallSamples);
        }
        if (!m_crowdSamples.empty()) {
            ++m_timedRuns[v];
            out.gpuMs = Median(m_frameSamples);
            out.extras.push_back({ "crowdScopeMs", Median(m_crowdSamples) });
        }
        const bool culled = crowd.CullSettings().enabled && crowd.Asset().LodCount() > 0;
        uint32_t visible[CROWD_MAX_LODS] = {};
        std::string error;
        const bool haveVisible = culled && crowd.Culler().ReadVisibleCounts(visible, error);
        for (uint32_t i = 0; i < CROWD_MAX_LODS; ++i) {
            char key[16];
            std::snprintf(key, sizeof(key), "visibleLod%u", i);
            out.extras.push_back({ key, haveVisible ? double(visible[i]) : -1.0 });
        }
        if (!m_gpuClocks.empty()) {
            std::sort(m_gpuClocks.begin(), m_gpuClocks.end());
            std::sort(m_memClocks.begin(), m_memClocks.end());
            out.extras.push_back({ "gpuClockMHz", double(m_gpuClocks[m_gpuClocks.size() / 2]) });
            out.extras.push_back({ "memClockMHz", double(m_memClocks[m_memClocks.size() / 2]) });
            out.extras.push_back({ "pstate", double(m_worstPState) });
        }
        m_running = false;
        return true;
    }

    bool CrowdCase::Check(std::string& detail)
    {
        const uint32_t agents = m_app.GetRenderer().GetCrowdSystem().AgentCount();
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "agents=%u timedRuns=%u/%u of %u/%u%s%s",
            agents, m_timedRuns[0], m_timedRuns[1], m_runs[0], m_runs[1],
            m_lastError.empty() ? "" : " error=", m_lastError.c_str());
        detail = buffer;
        return m_lastError.empty() && agents == kAgentCount
            && m_timedRuns[0] == m_runs[0] && m_timedRuns[1] == m_runs[1] && m_runs[0] > 0;
    }

    void CrowdCase::Release()
    {
        if (!m_prepared) {
            return;
        }
        Render::Renderer& renderer = m_app.GetRenderer();
        Render::Crowd::CrowdSystem& crowd = renderer.GetCrowdSystem();
        std::string error;
        crowd.Spawn(0, kHalfExtent, 1, error);
        renderer.SetCrowdEnabled(m_savedEnabled);
        crowd.SetUpdateMode(static_cast<CrowdUpdateMode>(m_savedMode), error);
        crowd.SetGpuStepMode(static_cast<CrowdGpuStepMode>(m_savedStepMode));
        crowd.CullSettings().enabled = m_savedCull;
        crowd.SetTargetHeight(m_savedHeight);
        m_clockProbe.reset();
        m_running = false;
        m_prepared = false;
    }
}
