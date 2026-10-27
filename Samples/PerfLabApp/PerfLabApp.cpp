#include "PerfLabApp.h"
#include "PerfCsvReader.h"
#include "ApplicationCore.h"
#include "ApplicationEntryPoint.h"
#include "Renderer/Scene/SurfaceMaterialFactory.h"
#include "Object/SampleFloorBox.h"
#include "Object/StaticModel.h"
#include "Light/DefaultSunLight.h"
#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/Debug/RemoteControl/DebugCommandArgs.h"
#include "Input/CameraInputBinding.h"
#include "UI/ImGuiCoordinator.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Cases/MemcpyCase.h"
#include "Cases/MeshCodecCase.h"
#include "Cases/MeshQuantizeCase.h"
#include "Cases/VertexCacheCase.h"
#include "Cases/GBufferDrawCase.h"
#include "Cases/TextureUploadCase.h"
#include "Cases/MeshLoadCase.h"
#include "Cases/BrickGridCase.h"
#include "Cases/StreamCase.h"
#include "Cases/CrowdCase.h"
#include "PerfCsvWriter.h"
#include "imgui.h"

#include <windows.h>
#include <cstdio>
#include <memory>

namespace SasamiEngine::Sample::PerfLab
{
    namespace
    {
        std::string FormatPct(float pct)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%.1f%%", pct);
            return text;
        }

        constexpr uint32_t kMaxRepeat = 1000;

        std::filesystem::path DefaultCsvDir()
        {
            // The exe sits in Build/bin/<platform>/<config>[/<rhi>]; results go to Build/PerfLab.
            wchar_t buffer[MAX_PATH] = {};
            const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
            std::filesystem::path dir = length > 0 ? std::filesystem::path(buffer).parent_path() : std::filesystem::current_path();
            for (std::filesystem::path p = dir; !p.empty() && p.has_parent_path() && p != p.parent_path(); p = p.parent_path()) {
                if (p.filename() == L"bin") {
                    return p.parent_path() / L"PerfLab";
                }
            }
            return dir / L"PerfLab";
        }

        std::string FormatMs(double ms)
        {
            if (ms < 0.0) {
                return "-";
            }
            char text[32] = {};
            std::snprintf(text, sizeof(text), "%.3f", ms);
            return text;
        }

        std::string FormatKiB(uint64_t bytes)
        {
            if (bytes == 0) {
                return "-";
            }
            char text[32] = {};
            std::snprintf(text, sizeof(text), "%.1f", double(bytes) / 1024.0);
            return text;
        }

        std::string FormatExtras(const PerfMetrics& m)
        {
            std::string text;
            for (const PerfExtraMetric& e : m.extras) {
                char item[96] = {};
                std::snprintf(item, sizeof(item), "%s%s=%.3f", text.empty() ? "" : " ", e.name.c_str(), e.value);
                text += item;
            }
            return text.empty() ? "-" : text;
        }
    }

    void PerfLabApp::OnInit(ApplicationCore& app)
    {
        m_camera = app.CreateCameraObject();
        if (!m_camera) {
            DebugLog("PerfLabApp: Camera creation failed.\n");
            app.RequestQuit();
            return;
        }
        if (!app.SetMainCamera(m_camera)) {
            DebugLog("PerfLabApp: SetMainCamera failed.\n");
            app.RequestQuit();
            return;
        }
        m_camera->SetTarget(0.0f, 2.5f, -10.0f);
        m_camera->SetYawPitch(0.0f, 0.2f);
        m_camera->SetMoveSpeed(5.0f);
        m_camera->SetClipPlanes(0.05f, 500.0f);

        app.UseDefaultRenderNodePreset();

        StaticModel* floorModel = app.CreateStaticModel();
        if (!floorModel) {
            DebugLog("PerfLabApp: Floor model creation failed.\n");
            app.RequestQuit();
            return;
        }
        AddSampleFloorBox(*floorModel, MakeOpaqueSurfaceMaterial(0.52f, 0.54f, 0.58f, 0.9f, 0.0f));
        app.SetDirectionalLight(MakeDefaultSunLight());

        m_inputConnections = ConnectCameraInput([this]() { return m_camera; });

        m_csvDir = DefaultCsvDir();
        RegisterCases(app);
        RegisterCommands(app);
        ImGuiCoordinator::Instance().RegisterTab("PerfLab", "App", "Cases", [this]() { DrawPanel(); });
    }

    void PerfLabApp::RegisterCases(ApplicationCore& app)
    {
        m_runner.AddCase(std::make_unique<MemcpyCase>("memcpy_1mb", 1ull << 20));
        m_runner.AddCase(std::make_unique<MemcpyCase>("memcpy_64mb", 64ull << 20));
        m_runner.AddCase(std::make_unique<MeshCodecCase>("meshcodec_bunny", "Models/Bunny.obj"));
        m_runner.AddCase(std::make_unique<MeshCodecCase>("meshcodec_sponza", "Models/Sponza/glTF/Sponza.gltf"));
        m_runner.AddCase(std::make_unique<MeshQuantizeCase>("meshquant_bunny", "Models/Bunny.obj"));
        m_runner.AddCase(std::make_unique<MeshQuantizeCase>("meshquant_sponza", "Models/Sponza/glTF/Sponza.gltf"));
        m_runner.AddCase(std::make_unique<MeshQuantizeCase>("meshquant_bistro", "Models/Bistro/BistroExterior.fbx"));
        m_runner.AddCase(std::make_unique<VertexCacheCase>("vcache_bunny", "Models/Bunny.obj"));
        m_runner.AddCase(std::make_unique<VertexCacheCase>("vcache_sponza", "Models/Sponza/glTF/Sponza.gltf"));
        m_runner.AddCase(std::make_unique<GBufferDrawCase>("gbuffer_bunny", app, "Models/Bunny.obj", false));
        m_runner.AddCase(std::make_unique<GBufferDrawCase>("gbuffer_sponza", app, "Models/Sponza/glTF/Sponza.gltf", true));
        m_runner.AddCase(std::make_unique<GBufferDrawCase>("gbuffer_sponza_nooverdraw", app, "Models/Sponza/glTF/Sponza.gltf", true,
                                                           MeshReorderMode::NoOverdraw));
        m_runner.AddCase(std::make_unique<GBufferDrawCase>("gbuffer_sponza_cacheonly", app, "Models/Sponza/glTF/Sponza.gltf", true,
                                                           MeshReorderMode::CacheOnly));
        m_runner.AddCase(std::make_unique<GBufferDrawCase>("gbuffer_sponza_overdrawonly", app, "Models/Sponza/glTF/Sponza.gltf", true,
                                                           MeshReorderMode::OverdrawOnly));
        m_runner.AddCase(std::make_unique<TextureUploadCase>("texupload_bistro", app, "Models/Bistro/Textures"));
        m_runner.AddCase(std::make_unique<MeshLoadCase>("meshload_sponza", "Models/Sponza/glTF/Sponza.gltf"));
        m_runner.AddCase(std::make_unique<MeshLoadCase>("meshload_bistro", "Models/Bistro/BistroExterior.fbx"));
        m_runner.AddCase(std::make_unique<BrickGridCase>("brickgrid_sponza_s512", "Models/Sponza/glTF/Sponza.gltf", 512, false));
        m_runner.AddCase(std::make_unique<BrickGridCase>("brickgrid_sponza_v512", "Models/Sponza/glTF/Sponza.gltf", 512, true));
        m_runner.AddCase(std::make_unique<BrickGridCase>("brickgrid_bistro_s512", "Models/Bistro/BistroExterior.fbx", 512, false));
        m_runner.AddCase(std::make_unique<StreamCase>("stream_sponza_c32_r1_sdf0", "Models/Sponza/glTF/Sponza.gltf", 384, 32, 1, 0));
        m_runner.AddCase(std::make_unique<StreamCase>("stream_sponza_c32_r1_sdf4", "Models/Sponza/glTF/Sponza.gltf", 384, 32, 1, 4));
        m_runner.AddCase(std::make_unique<StreamCase>("stream_sponza_c32_r3_sdf4", "Models/Sponza/glTF/Sponza.gltf", 384, 32, 3, 4));
        m_runner.AddCase(std::make_unique<StreamCase>("stream_bistro_c32_r1_sdf4", "Models/Bistro/BistroExterior.fbx", 384, 32, 1, 4));
        m_runner.AddCase(std::make_unique<StreamCase>("stream_bistro_c32_r3_sdf4", "Models/Bistro/BistroExterior.fbx", 384, 32, 3, 4));
        m_runner.AddCase(std::make_unique<CrowdCase>("crowd_update", app, CrowdCaseKind::Update));
        m_runner.AddCase(std::make_unique<CrowdCase>("crowd_cull", app, CrowdCaseKind::Cull));
    }

    void PerfLabApp::OnUpdate(ApplicationCore& app, float deltaTime)
    {
        (void)app;
        if (m_camera) {
            m_camera->Update(deltaTime);
        }
        if (m_runner.Tick()) {
            WriteFinishedCsv();
        }
    }

    void PerfLabApp::WriteFinishedCsv()
    {
        const std::vector<PerfCaseResult>& results = m_runner.Results();
        if (results.empty()) {
            return;
        }
        // Tick finishes at most one case per call, and the newest result is always last.
        const PerfCaseResult& result = results.back();
        // Read the previous run before WritePerfCsv overwrites it.
        PerfCsvMedians previous;
        std::string readError;
        PerfComparison comparison;
        if (ReadPerfCsvMedians(m_csvDir / (result.caseName + ".csv"), previous, readError)) {
            comparison = ComparePerfResult(previous, result, m_regressionPct / 100.0);
        } else {
            comparison.note = readError;
        }
        m_comparisons[result.caseName] = comparison;
        std::string error;
        if (WritePerfCsv(m_csvDir, result, error)) {
            m_lastCsvError.clear();
        } else {
            m_lastCsvError = error;
            DebugLog(("PerfLabApp: " + error + "\n").c_str());
        }
    }

    void PerfLabApp::OnRender(ApplicationCore& app)
    {
        if (!app.IsRendererReady()) {
            return;
        }
        app.RenderFrame();
    }

    void PerfLabApp::OnShutdown(ApplicationCore& app)
    {
        m_inputConnections.clear();
        app.ClearObjects();
        m_camera = nullptr;
    }

    std::string PerfLabApp::ResultLine(const PerfCaseResult& r) const
    {
        if (!r.prepared) {
            return r.caseName + " prepare=FAIL (" + r.detail + ")";
        }
        const PerfVariantResult& b = r.variants[0];
        const PerfVariantResult& o = r.variants[1];
        const double speedup = o.cpuMs.median > 0.0 ? b.cpuMs.median / o.cpuMs.median : 0.0;
        char text[512] = {};
        std::snprintf(text, sizeof(text),
            "%s repeat=%u check=%s base(cold=%s med=%s min=%s max=%s gpu=%s) opt(cold=%s med=%s min=%s max=%s gpu=%s) speedup=%.2f",
            r.caseName.c_str(), r.repeat, r.checkOk ? "ok" : "FAIL",
            FormatMs(b.cold.cpuMs).c_str(), FormatMs(b.cpuMs.median).c_str(), FormatMs(b.cpuMs.min).c_str(),
            FormatMs(b.cpuMs.max).c_str(), b.gpuMs.count ? FormatMs(b.gpuMs.median).c_str() : "-",
            FormatMs(o.cold.cpuMs).c_str(), FormatMs(o.cpuMs.median).c_str(), FormatMs(o.cpuMs.min).c_str(),
            FormatMs(o.cpuMs.max).c_str(), o.gpuMs.count ? FormatMs(o.gpuMs.median).c_str() : "-",
            speedup);
        std::string line = text;
        if (b.gpuMs.count && o.gpuMs.count && o.gpuMs.median > 0.0) {
            char gpuText[64] = {};
            std::snprintf(gpuText, sizeof(gpuText), " gpuSpeedup=%.3f", b.gpuMs.median / o.gpuMs.median);
            line += gpuText;
        }
        if (b.gpuExcluded || o.gpuExcluded) {
            line += " gpuExcluded=" + std::to_string(b.gpuExcluded) + "/" + std::to_string(o.gpuExcluded);
        }
        if (!b.cold.extras.empty() || !o.cold.extras.empty()) {
            line += " extras(base " + FormatExtras(b.cold) + " | opt " + FormatExtras(o.cold) + ")";
        }
        const auto comparison = m_comparisons.find(r.caseName);
        if (comparison != m_comparisons.end()) {
            line += " " + ComparisonText(comparison->second);
        }
        return line + " [" + r.detail + "]";
    }

    std::string PerfLabApp::ComparisonText(const PerfComparison& c) const
    {
        if (!c.hasPrevious) {
            return "prev=none (" + c.note + ")";
        }
        auto ratio = [](const PerfDelta& d) {
            if (d.ratio <= 0.0) {
                return std::string("-");
            }
            char text[32] = {};
            std::snprintf(text, sizeof(text), "%.3f", d.ratio);
            return std::string(text);
        };
        char threshold[32] = {};
        std::snprintf(threshold, sizeof(threshold), "%.1f%%", c.threshold * 100.0);
        // ratio = now / prev of the warm median; > 1 is slower.
        std::string text = "vsPrev(cpu " + ratio(c.cpu[0]) + "/" + ratio(c.cpu[1])
            + " gpu " + ratio(c.gpu[0]) + "/" + ratio(c.gpu[1]) + ") threshold=" + threshold + " regress=";
        std::string which;
        const char* names[2] = { "base", "opt" };
        for (uint32_t v = 0; v < 2; ++v) {
            if (c.cpu[v].regressed) {
                which += (which.empty() ? "" : ",") + std::string(names[v]) + ".cpu";
            }
            if (c.gpu[v].regressed) {
                which += (which.empty() ? "" : ",") + std::string(names[v]) + ".gpu";
            }
        }
        return text + (which.empty() ? "none" : which);
    }

    std::string PerfLabApp::StatusText() const
    {
        std::string text = m_runner.IsBusy() ? "busy" : "idle";
        if (!m_runner.ActiveName().empty()) {
            text += " active=" + m_runner.ActiveName() + " step=" + std::to_string(m_runner.ActiveStep())
                + "/" + std::to_string(m_runner.ActiveTotal());
        }
        text += " queued=" + std::to_string(m_runner.QueuedCount())
            + " results=" + std::to_string(m_runner.Results().size())
            + " csvDir=" + m_csvDir.string();
        if (!m_lastCsvError.empty()) {
            text += " csvError=" + m_lastCsvError;
        }
        for (const PerfCaseResult& r : m_runner.Results()) {
            text += "\n  " + ResultLine(r);
        }
        return text;
    }

    std::string PerfLabApp::RunCaseCommand(const std::vector<std::string>& args)
    {
        using namespace App::Debug;
        const std::string sub = args.empty() ? "status" : ToLower(args[0]);
        if (sub == "list") {
            std::string text = "OK cases=" + std::to_string(m_runner.Cases().size());
            for (const auto& c : m_runner.Cases()) {
                text += "\n  " + std::string(c->Name()) + " - " + c->Description();
            }
            return text;
        }
        if (sub == "status") {
            return "OK " + StatusText();
        }
        if (sub == "threshold") {
            if (args.size() < 2) {
                return "OK threshold=" + FormatPct(m_regressionPct);
            }
            const float pct = std::strtof(args[1].c_str(), nullptr);
            if (pct <= 0.0f) {
                return "ERR usage: perf.case threshold <percent > 0>";
            }
            m_regressionPct = pct;
            return "OK threshold=" + FormatPct(m_regressionPct) + " (applies to the next finished case)";
        }
        if (sub == "csv") {
            if (args.size() < 2) {
                return "OK csvDir=" + m_csvDir.string();
            }
            m_csvDir = std::filesystem::path(JoinFrom(args, 1));
            return "OK csvDir=" + m_csvDir.string() + " (applies to the next finished case)";
        }
        if (sub == "run" || sub == "all") {
            const size_t optionStart = sub == "run" ? 2 : 1;
            if (sub == "run" && args.size() < 2) {
                return "ERR usage: perf.case run <case> [repeat=N]";
            }
            std::map<std::string, std::string> options;
            const std::vector<std::string> rest(args.begin() + (std::min)(optionStart, args.size()), args.end());
            if (!ParseKeyValues(rest, { "repeat" }, options)) {
                return "ERR unknown option (repeat=N)";
            }
            uint32_t repeat = 5;
            if (auto it = options.find("repeat"); it != options.end()) {
                const auto value = ParseUintInRange(it->second, 1, kMaxRepeat);
                if (!value) {
                    return "ERR repeat must be 1.." + std::to_string(kMaxRepeat);
                }
                repeat = *value;
            }
            if (sub == "all") {
                m_runner.EnqueueAll(repeat);
                return "OK queued " + std::to_string(m_runner.Cases().size()) + " cases repeat=" + std::to_string(repeat)
                    + " (one run per frame; poll perf.case status)";
            }
            if (!m_runner.Enqueue(args[1], repeat)) {
                return "ERR unknown case '" + args[1] + "' (perf.case list)";
            }
            return "OK queued " + args[1] + " repeat=" + std::to_string(repeat) + " (one run per frame; poll perf.case status)";
        }
        return "ERR usage: perf.case list | run <case> [repeat=N] | all [repeat=N] | csv [dir] | threshold [pct] | status";
    }

    void PerfLabApp::RegisterCommands(ApplicationCore& app)
    {
        Debug::DebugCommandRegistry* registry = app.GetDebugCommandRegistry();
        if (!registry) {
            return;
        }
        // The lambdas capture this; PerfLabApp outlives the registry (ApplicationEntryPoint.h
        // declares the application before ApplicationCore).
        registry->Register("perf.case",
            "list | run <case> [repeat=N] | all [repeat=N] | csv [dir] | status - baseline vs optimised benchmarks (async, one run per frame)",
            [this](const std::vector<std::string>& args) -> std::string { return RunCaseCommand(args); });
        registry->Register("perf.status", "- same as perf.case status",
            [this](const std::vector<std::string>&) -> std::string { return "OK " + StatusText(); });
    }

    void PerfLabApp::DrawPanel()
    {
        ImGui::TextUnformatted(m_runner.IsBusy() ? "Running..." : "Idle");
        if (!m_runner.ActiveName().empty()) {
            ImGui::SameLine();
            ImGui::Text("%s %u/%u", m_runner.ActiveName().c_str(), m_runner.ActiveStep(), m_runner.ActiveTotal());
        }
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Repeat", &m_uiRepeat);
        m_uiRepeat = std::clamp(m_uiRepeat, 1, static_cast<int>(kMaxRepeat));
        ImGui::SameLine();
        if (ImGui::Button("Run all")) {
            m_runner.EnqueueAll(static_cast<uint32_t>(m_uiRepeat));
        }
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputFloat("Regression %", &m_regressionPct, 1.0f, 5.0f, "%.1f");
        m_regressionPct = (std::max)(m_regressionPct, 0.1f);
        ImGui::TextDisabled("CSV: %s", m_csvDir.string().c_str());
        if (!m_lastCsvError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "CSV error: %s", m_lastCsvError.c_str());
        }
        ImGui::Separator();

        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit
            | ImGuiTableFlags_ScrollX;
        if (!ImGui::BeginTable("##perfcases", 11, flags)) {
            return;
        }
        ImGui::TableSetupColumn("Case");
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("Check");
        ImGui::TableSetupColumn("Base med ms");
        ImGui::TableSetupColumn("Opt med ms");
        ImGui::TableSetupColumn("Speedup");
        ImGui::TableSetupColumn("vs prev b / o");
        ImGui::TableSetupColumn("Cold b/o ms");
        ImGui::TableSetupColumn("Min-max b / o");
        ImGui::TableSetupColumn("Disk b/o KiB");
        ImGui::TableSetupColumn("Extras b | o");
        ImGui::TableHeadersRow();
        for (const auto& c : m_runner.Cases()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(c->Name());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", c->Description());
            }
            ImGui::TableNextColumn();
            ImGui::PushID(c->Name());
            if (ImGui::SmallButton("Run")) {
                m_runner.Enqueue(c->Name(), static_cast<uint32_t>(m_uiRepeat));
            }
            ImGui::PopID();
            const PerfCaseResult* r = m_runner.FindResult(c->Name());
            ImGui::TableNextColumn();
            if (!r) {
                ImGui::TextDisabled("-");
                continue;
            }
            if (!r->prepared) {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "prepare failed");
                continue;
            }
            if (r->checkOk) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "ok");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "FAIL");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", r->detail.c_str());
            }
            const PerfVariantResult& b = r->variants[0];
            const PerfVariantResult& o = r->variants[1];
            // A case that times the GPU (gbuffer_*) does almost nothing on the CPU, so show its GPU figures.
            const bool gpu = b.gpuMs.count && o.gpuMs.count;
            const PerfSummary& bs = gpu ? b.gpuMs : b.cpuMs;
            const PerfSummary& os = gpu ? o.gpuMs : o.cpuMs;
            const char* unit = gpu ? " gpu" : "";
            ImGui::TableNextColumn();
            ImGui::Text("%.3f%s", bs.median, unit);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f%s", os.median, unit);
            ImGui::TableNextColumn();
            ImGui::Text("%.2fx", os.median > 0.0 ? bs.median / os.median : 0.0);
            ImGui::TableNextColumn();
            DrawComparisonCell(*r);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f / %.3f", gpu ? b.cold.gpuMs : b.cold.cpuMs, gpu ? o.cold.gpuMs : o.cold.cpuMs);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f-%.3f / %.3f-%.3f", bs.min, bs.max, os.min, os.max);
            ImGui::TableNextColumn();
            ImGui::Text("%s / %s", FormatKiB(b.cold.diskBytes).c_str(), FormatKiB(o.cold.diskBytes).c_str());
            ImGui::TableNextColumn();
            if (b.cold.extras.empty() && o.cold.extras.empty()) {
                ImGui::TextDisabled("-");
            } else {
                ImGui::Text("%s | %s", FormatExtras(b.cold).c_str(), FormatExtras(o.cold).c_str());
            }
        }
        ImGui::EndTable();
    }

    void PerfLabApp::DrawComparisonCell(const PerfCaseResult& result) const
    {
        const auto it = m_comparisons.find(result.caseName);
        if (it == m_comparisons.end() || !it->second.hasPrevious) {
            ImGui::TextDisabled("-");
            if (it != m_comparisons.end() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", it->second.note.c_str());
            }
            return;
        }
        const PerfComparison& c = it->second;
        // Same metric as the med columns: GPU when both variants timed it, else CPU.
        const bool gpu = result.variants[0].gpuMs.count && result.variants[1].gpuMs.count;
        for (uint32_t v = 0; v < 2; ++v) {
            const PerfDelta& d = gpu ? c.gpu[v] : c.cpu[v];
            if (v == 1) {
                ImGui::SameLine(0.0f, 4.0f);
                ImGui::TextUnformatted("/");
                ImGui::SameLine(0.0f, 4.0f);
            }
            if (d.ratio <= 0.0) {
                ImGui::TextDisabled("-");
            } else if (d.regressed) {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%+.1f%%", (d.ratio - 1.0) * 100.0);
            } else if (d.improved) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%+.1f%%", (d.ratio - 1.0) * 100.0);
            } else {
                ImGui::Text("%+.1f%%", (d.ratio - 1.0) * 100.0);
            }
            if (ImGui::IsItemHovered() && d.ratio > 0.0) {
                ImGui::SetTooltip("prev %.4f ms -> now %.4f ms (threshold %.1f%%)", d.prev, d.now, c.threshold * 100.0);
            }
        }
    }
}

SASAMI_IMPLEMENT_APPLICATION(SasamiEngine::Sample::PerfLab::PerfLabApp, 1280, 720, L"PerfLab Sample")
