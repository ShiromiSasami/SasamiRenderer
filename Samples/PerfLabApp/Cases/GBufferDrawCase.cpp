#include "Cases/GBufferDrawCase.h"

#include "Cases/MeshReorder.h"
#include "Cases/PerfMeshSource.h"
#include "ApplicationCore.h"
#include "Object/StaticModel.h"
#include "Object/Camera.h"
#include "Renderer/Scene/SurfaceMaterialFactory.h"
#include "Renderer/Runtime/Renderer.h"
#include "Foundation/Math/MathUtil.h"

#include <algorithm>
#include <cstdio>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        constexpr const char* kGBufferPassName = "OpaqueGBuffer";

        // RenderGraph::AddPass appends "_<pass index>" to every pass name (RenderGraph.cpp), and the
        // index moves when passes such as MeshShader are toggled, so match the prefix only.
        bool IsGBufferScope(const std::string& scopeName)
        {
            const std::string prefix = std::string(kGBufferPassName) + "_";
            return scopeName.size() > prefix.size()
                && scopeName.compare(0, prefix.size(), prefix) == 0
                && std::all_of(scopeName.begin() + prefix.size(), scopeName.end(),
                    [](char c) { return c >= '0' && c <= '9'; });
        }
        // "Shadow_3" -> "Shadow"; names without a numeric suffix are kept whole.
        std::string PassPrefix(const std::string& scopeName)
        {
            const size_t underscore = scopeName.rfind('_');
            if (underscore == std::string::npos || underscore + 1 == scopeName.size()
                || !std::all_of(scopeName.begin() + underscore + 1, scopeName.end(),
                    [](char c) { return c >= '0' && c <= '9'; })) {
                return scopeName;
            }
            return scopeName.substr(0, underscore);
        }

        double Median(std::vector<double>& values)
        {
            std::sort(values.begin(), values.end());
            return values[values.size() / 2];
        }
        constexpr float kFrameDistance = 1.6f; // mesh radii between the camera and a framed mesh
    }

    GBufferDrawCase::GBufferDrawCase(std::string name, App::ApplicationCore& app, std::string assetPath, bool inside,
                                     MeshReorderMode mode)
        : m_name(std::move(name))
        , m_app(app)
        , m_assetPath(std::move(assetPath))
        , m_inside(inside)
        , m_mode(mode)
    {
        m_description = m_assetPath + ": frame GPU ms, loader order vs meshopt reorder (" + MeshReorderModeName(m_mode) + ")"
            + (m_inside ? " (camera inside)" : " (framed)");
    }

    bool GBufferDrawCase::Prepare(std::string& error)
    {
        PerfMeshSource source;
        if (!LoadWeldedMesh(m_assetPath, source, error)) {
            return false;
        }
        m_vertices[0] = std::move(source.vertices);
        m_indices[0] = std::move(source.indices);
        ReorderForVertexCache(m_vertices[0], m_indices[0], m_vertices[1], m_indices[1], m_mode);
        for (uint32_t v = 0; v < 2; ++v) {
            m_extras[v] = AnalyzeMeshOrder(m_vertices[v], m_indices[v]);
        }

        float lo[3] = { 1e30f, 1e30f, 1e30f };
        float hi[3] = { -1e30f, -1e30f, -1e30f };
        for (const Render::Vertex& vertex : m_vertices[0]) {
            for (int c = 0; c < 3; ++c) {
                lo[c] = (std::min)(lo[c], vertex.position[c]);
                hi[c] = (std::max)(hi[c], vertex.position[c]);
            }
        }
        float centre[3] = {};
        float radius = 0.0f;
        for (int c = 0; c < 3; ++c) {
            centre[c] = 0.5f * (lo[c] + hi[c]);
            radius = (std::max)(radius, 0.5f * (hi[c] - lo[c]));
        }
        Camera* camera = m_app.GetMainCamera();
        if (!camera) {
            error = "no main camera";
            return false;
        }
        const Foundation::Vector3 eye = camera->Transform().position;
        float target[3] = { eye.x, eye.y, eye.z };
        if (m_inside) {
            // Eye height a fifth of the way up, which puts Sponza's camera in the atrium.
            centre[1] = lo[1] + 0.2f * (hi[1] - lo[1]);
        } else {
            float forward[3] = {};
            Foundation::Math::DirectionFromYawPitch(camera->Yaw(), camera->Pitch(), forward);
            for (int c = 0; c < 3; ++c) {
                target[c] += forward[c] * radius * kFrameDistance;
            }
        }
        for (int c = 0; c < 3; ++c) {
            m_translation[c] = target[c] - centre[c];
        }

        m_model = m_app.CreateStaticModel();
        if (!m_model) {
            error = "StaticModel creation failed";
            return false;
        }
        m_model->SetTranslation(m_translation[0], m_translation[1], m_translation[2]);
        m_savedMeshShader = m_app.GetUseMeshShader();
        m_app.SetUseMeshShader(false);
        m_clockProbe = std::make_unique<GpuClockProbe>();
        m_running = false;
        m_missingRuns = 0;
        return true;
    }

    void GBufferDrawCase::ShowVariant(PerfVariant variant)
    {
        const uint32_t v = static_cast<uint32_t>(variant);
        Render::Mesh mesh;
        mesh.vertices = m_vertices[v];
        mesh.indices = m_indices[v];
        m_model->ClearModel();
        m_model->AddStaticMesh(std::move(mesh), Render::MakeOpaqueSurfaceMaterial(0.7f, 0.7f, 0.7f, 0.6f, 0.0f));
        m_app.InvalidateRenderObjects();
    }

    PerfMetrics GBufferDrawCase::RunVariant(PerfVariant variant)
    {
        // Single-frame fallback; the runner uses StepVariant.
        PerfMetrics metrics;
        while (!StepVariant(variant, metrics)) {
        }
        return metrics;
    }

    bool GBufferDrawCase::StepVariant(PerfVariant variant, PerfMetrics& out)
    {
        const Render::Renderer& renderer = m_app.GetRenderer();
        if (!m_running) {
            ShowVariant(variant);
            m_running = true;
            m_frame = 0;
            m_samples.clear();
            m_otherSamples.clear();
            m_frameSamples.clear();
            m_passSamples.clear();
            m_shadowSamples.clear();
            m_maxGBufferSeen = 0.0;
            m_frameAtGpuMax = -1.0;
            m_gpuClocks.clear();
            m_memClocks.clear();
            m_worstPState = -1;
            m_lastTimingFrame = renderer.GetGpuTimingFrameCounter();
            return false;
        }
        ++m_frame;
        // The new proxies are built on the next frame and their timings arrive kFrameLatency frames
        // after that.
        const uint32_t settle = Render::GpuTimestampProfiler::kFrameLatency + kSettleFrames;
        const uint64_t timingFrame = renderer.GetGpuTimingFrameCounter();
        if (m_frame > settle && timingFrame != m_lastTimingFrame) {
            // The other passes of the same frame are the control: if they move with the GBuffer
            // figure, the change is the GPU (clock or power state), not the mesh order.
            double gbuffer = -1.0;
            double other = 0.0;
            double shadow = 0.0;
            std::map<std::string, double> passes;
            for (const auto& scope : renderer.GetGpuPassTimings()) {
                const std::string prefix = PassPrefix(scope.name);
                passes[prefix] += scope.milliseconds;
                if (prefix.compare(0, 6, "Shadow") == 0) {
                    shadow += scope.milliseconds;
                }
                if (IsGBufferScope(scope.name)) {
                    gbuffer = scope.milliseconds;
                } else {
                    other += scope.milliseconds;
                }
            }
            if (gbuffer >= 0.0) {
                m_samples.push_back(gbuffer);
                m_otherSamples.push_back(other);
                m_frameSamples.push_back(gbuffer + other);
                m_shadowSamples.push_back(shadow);
                for (const auto& [prefix, ms] : passes) {
                    m_passSamples[prefix].push_back(ms);
                }
                if (m_samples.size() == 1 || gbuffer > m_maxGBufferSeen) {
                    m_maxGBufferSeen = gbuffer;
                    m_frameAtGpuMax = gbuffer + other;
                }
                // Read now, while the timed frame is kFrameLatency frames old (see GpuClockProbe).
                GpuClockSample clock;
                if (m_clockProbe && m_clockProbe->Sample(clock)) {
                    m_gpuClocks.push_back(clock.graphicsMHz);
                    m_memClocks.push_back(clock.memoryMHz);
                    m_worstPState = (std::max)(m_worstPState, clock.pstate);
                }
            }
        }
        m_lastTimingFrame = timingFrame;
        // Give up after three times the sample window so a missing pass cannot hang the runner.
        const bool timedOut = m_frame > settle + 3 * kSampleFrames;
        if (m_samples.size() < kSampleFrames && !timedOut) {
            return false;
        }

        const uint32_t v = static_cast<uint32_t>(variant);
        out = PerfMetrics{};
        if (m_samples.empty()) {
            ++m_missingRuns;
        } else {
            std::sort(m_samples.begin(), m_samples.end());
            std::sort(m_otherSamples.begin(), m_otherSamples.end());
            std::sort(m_frameSamples.begin(), m_frameSamples.end());
            // gpuMs is the frame total, not the GBuffer scope: on gbuffer_sponza the GBuffer scope
            // alone reads ~0.08 or ~1.45 ms run by run while the other passes move the other way
            // (4.43 vs 3.15 ms) and the total stays ~4.5-4.6 ms (2026-10-25), so work slides across
            // the scope boundary. The reorder also changes every other pass that draws the mesh.
            out.gpuMs = m_frameSamples[m_frameSamples.size() / 2];
        }
        out.cpuBytes = m_vertices[v].size() * sizeof(Render::Vertex) + m_indices[v].size() * sizeof(uint32_t);
        out.gpuBytes = out.cpuBytes;
        out.extras = m_extras[v];
        if (!m_samples.empty()) {
            out.extras.push_back({ "gbufferScopeMs", m_samples[m_samples.size() / 2] });
            out.extras.push_back({ "gpuMinMs", m_samples.front() });
            out.extras.push_back({ "gpuMaxMs", m_samples.back() });
            out.extras.push_back({ "otherGpuMs", m_otherSamples[m_otherSamples.size() / 2] });
            // Sum of all timed passes per frame: if a GBuffer spike is matched by a drop elsewhere
            // (frameAtGpuMaxMs ~= frameGpuMs), the spike is work moving across a scope boundary.
            out.extras.push_back({ "frameGpuMs", m_frameSamples[m_frameSamples.size() / 2] });
            out.extras.push_back({ "frameMaxMs", m_frameSamples.back() });
            out.extras.push_back({ "frameAtGpuMaxMs", m_frameAtGpuMax });
            out.extras.push_back({ "shadowGpuMs", Median(m_shadowSamples) });
            for (auto& [prefix, samples] : m_passSamples) {
                // A pass missing from some frames would bias its median; report it only when complete.
                if (samples.size() == m_frameSamples.size()) {
                    out.extras.push_back({ "ms_" + prefix, Median(samples) });
                }
            }
        }
        if (!m_gpuClocks.empty()) {
            std::sort(m_gpuClocks.begin(), m_gpuClocks.end());
            std::sort(m_memClocks.begin(), m_memClocks.end());
            out.extras.push_back({ "gpuClockMHz", m_gpuClocks[m_gpuClocks.size() / 2] });
            out.extras.push_back({ "memClockMHz", m_memClocks[m_memClocks.size() / 2] });
            out.extras.push_back({ "pstate", double(m_worstPState) });
        }
        m_running = false;
        return true;
    }

    bool GBufferDrawCase::Check(std::string& detail)
    {
        const MeshOrderComparison cmp = CompareMeshOrder(m_vertices[0], m_indices[0], m_vertices[1], m_indices[1]);
        char text[192] = {};
        std::snprintf(text, sizeof(text), "tris=%zu sameTriangles=%d unmatched=%zu missingRuns=%u",
            m_indices[0].size() / 3, cmp.sameTriangles ? 1 : 0, cmp.unmatched, m_missingRuns);
        detail = text;
        return cmp.sizesOk && cmp.sameTriangles && m_missingRuns == 0;
    }

    void GBufferDrawCase::Release()
    {
        if (m_model) {
            m_app.DeleteObject(m_model);
            m_model = nullptr;
            m_app.SetUseMeshShader(m_savedMeshShader);
            m_app.InvalidateRenderObjects();
        }
        m_clockProbe.reset();
        m_running = false;
        for (uint32_t v = 0; v < 2; ++v) {
            m_vertices[v].clear();
            m_vertices[v].shrink_to_fit();
            m_indices[v].clear();
            m_indices[v].shrink_to_fit();
            m_extras[v].clear();
        }
    }
}
