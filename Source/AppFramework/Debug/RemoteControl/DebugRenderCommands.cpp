#include "AppFramework/Debug/RemoteControl/DebugRenderCommands.h"

#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/ApplicationCore.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace SasamiRenderer::Debug
{
    namespace
    {
        std::optional<float> ParseFloat(const std::string& text)
        {
            try {
                size_t consumed = 0;
                const float value = std::stof(text, &consumed);
                if (consumed != text.size()) {
                    return std::nullopt;
                }
                return value;
            } catch (...) {
                return std::nullopt;
            }
        }

        std::optional<int> ParseInt(const std::string& text)
        {
            try {
                size_t consumed = 0;
                const int value = std::stoi(text, &consumed);
                if (consumed != text.size()) {
                    return std::nullopt;
                }
                return value;
            } catch (...) {
                return std::nullopt;
            }
        }

        std::string FormatFloat(float value)
        {
            std::string text = std::to_string(value);
            // Trim trailing zeros so "1.000000" reads as "1".
            const size_t dot = text.find('.');
            if (dot != std::string::npos) {
                size_t last = text.find_last_not_of('0');
                if (last == dot) {
                    --last;
                }
                text.erase(last + 1);
            }
            return text;
        }

        // Names mirror RendererEnums::GBufferDebugView (Source/Renderer/Structures/RendererEnums.h).
        // Index order is authoritative there; this table exists only so the command can be driven by
        // name instead of a magic number.
        const char* const kGBufferViewNames[] = {
            "FinalLit", "Albedo", "Normal", "Roughness", "Metallic", "AmbientOcclusion",
            "Shadow", "Emissive", "RuntimeAmbientOcclusionRaw", "RuntimeAmbientOcclusionFiltered",
            "DirectionalLightDirection", "DirectionalLightNdotL", "ReflectionRadiance",
            "ReflectionAlpha", "SwrtReflectionHitDistance", "SwrtReflectionComposite",
        };
        constexpr size_t kGBufferViewCount = sizeof(kGBufferViewNames) / sizeof(kGBufferViewNames[0]);

        std::string ToLower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        // Case-insensitive name lookup; returns -1 when not found.
        int FindGBufferViewByName(const std::string& name)
        {
            const std::string lowerName = ToLower(name);
            for (size_t i = 0; i < kGBufferViewCount; ++i) {
                if (ToLower(kGBufferViewNames[i]) == lowerName) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        // "3 (Roughness)" or "7 (out of range)" if unknown.
        std::string GBufferViewLabel(int index)
        {
            if (index < 0 || static_cast<size_t>(index) >= kGBufferViewCount) {
                return std::to_string(index) + " (out of range)";
            }
            return std::to_string(index) + " (" + kGBufferViewNames[index] + ")";
        }
    }

    // Every setter below reports the value it reads back from ApplicationCore rather than the
    // value the caller requested, because a couple of these setters (notably the GBuffer debug
    // view) silently clamp or reject the request depending on unrelated renderer state. Reporting
    // the read-back keeps the remote caller from believing a rejected change actually applied.
    void RegisterRenderCommands(DebugCommandRegistry& registry, ApplicationCore& app)
    {
        registry.Register("render.get", "current render state (gbuffer view, exposure, IBL, AO mode, render path)",
            [&app](const std::vector<std::string>&) -> std::string {
                return "OK gbufferView=" + GBufferViewLabel(app.GetGBufferDebugViewIndex())
                     + " exposure=" + FormatFloat(app.GetExposure())
                     + " iblIntensity=" + FormatFloat(app.GetIblIntensity())
                     + " aoMode=" + std::to_string(app.GetAmbientOcclusionModeIndex())
                     + " renderPath=" + std::to_string(app.GetRenderPathModeIndex());
            });

        registry.Register("render.gbuffer", "<index|name> - set the GBuffer debug view; 'list' enumerates them",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "ERR usage: render.gbuffer <index|name|list>";
                }
                if (ToLower(args[0]) == "list") {
                    std::string out = "OK";
                    for (size_t i = 0; i < kGBufferViewCount; ++i) {
                        out += " " + std::to_string(i) + "=" + kGBufferViewNames[i];
                    }
                    return out;
                }

                int index = -1;
                bool resolved = false;
                if (const auto parsed = ParseInt(args[0])) {
                    index = *parsed;
                    resolved = true;
                } else {
                    index = FindGBufferViewByName(args[0]);
                    resolved = index >= 0;
                }
                if (!resolved) {
                    return "ERR unknown view (try render.gbuffer list)";
                }
                if (index < 0 || static_cast<size_t>(index) >= kGBufferViewCount) {
                    return "ERR index out of range 0.." + std::to_string(kGBufferViewCount - 1);
                }

                app.SetGBufferDebugViewIndex(index);
                // SetGBufferDebugView silently forces FinalLit when the render path is not
                // Raster, so the read-back may differ from the index just requested.
                return "OK gbufferView=" + GBufferViewLabel(app.GetGBufferDebugViewIndex());
            });

        registry.Register("render.exposure", "<f> - set tone-map exposure (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "OK exposure=" + FormatFloat(app.GetExposure());
                }
                const auto value = ParseFloat(args[0]);
                if (!value) {
                    return "ERR argument must be a number";
                }
                app.SetExposure(*value);
                return "OK exposure=" + FormatFloat(app.GetExposure());
            });

        registry.Register("render.ibl", "<f> - set IBL intensity (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "OK iblIntensity=" + FormatFloat(app.GetIblIntensity());
                }
                const auto value = ParseFloat(args[0]);
                if (!value) {
                    return "ERR argument must be a number";
                }
                app.SetIblIntensity(*value);
                return "OK iblIntensity=" + FormatFloat(app.GetIblIntensity());
            });

        registry.Register("render.ao", "<index> - set ambient occlusion mode (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "OK aoMode=" + std::to_string(app.GetAmbientOcclusionModeIndex());
                }
                const auto value = ParseInt(args[0]);
                if (!value) {
                    return "ERR argument must be an integer";
                }
                app.SetAmbientOcclusionModeIndex(*value);
                return "OK aoMode=" + std::to_string(app.GetAmbientOcclusionModeIndex());
            });

        registry.Register("render.path", "<0=Raster|1=HardwareRayTracing> - set render path (no argument: report current)",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (args.empty()) {
                    return "OK renderPath=" + std::to_string(app.GetRenderPathModeIndex());
                }
                const auto value = ParseInt(args[0]);
                if (!value) {
                    return "ERR argument must be an integer";
                }
                app.SetRenderPathModeIndex(*value);
                return "OK renderPath=" + std::to_string(app.GetRenderPathModeIndex());
            });

        // Reports the GPU cost of each render-graph pass so optimisation targets can be picked
        // from measurements instead of guesses. Results lag the frame being recorded by
        // GpuTimestampProfiler::kFrameLatency frames, so the first few polls after launch are empty.
        registry.Register("render.gputime", "per-pass GPU milliseconds for the last completed frame",
            [&app](const std::vector<std::string>&) -> std::string {
                // Distinguish the two empty cases: a profiler that never initialized will stay
                // empty forever, whereas a ready one is merely still filling its latency window.
                if (!app.GetRenderer().IsGpuTimingReady()) {
                    return "ERR GPU timestamp profiler failed to initialize (timing unavailable)";
                }
                const auto& timings = app.GetRenderer().GetGpuPassTimings();
                if (timings.empty()) {
                    return "PENDING profiler ready but no results yet (scopesThisFrame="
                         + std::to_string(app.GetRenderer().GetGpuTimingScopesThisFrame())
                         + " lastResolvedScopes="
                         + std::to_string(app.GetRenderer().GetGpuTimingLastResolvedScopeCount())
                         + " frameCounter="
                         + std::to_string(app.GetRenderer().GetGpuTimingFrameCounter())
                         + " askedFrame="
                         + std::to_string(app.GetRenderer().GetGpuTimingLastUpdateRequestedFrame())
                         + " slotHeldFrame="
                         + std::to_string(app.GetRenderer().GetGpuTimingLastUpdateSlotFrame())
                         + " slotValid="
                         + (app.GetRenderer().GetGpuTimingLastUpdateSlotValid() ? "1" : "0")
                         + ")";
                }

                double total = 0.0;
                std::string out = "OK";
                for (const auto& scope : timings) {
                    total += scope.milliseconds;
                    out += ' ';
                    out += scope.name;
                    out += '=';
                    out += FormatFloat(static_cast<float>(scope.milliseconds));
                }
                out += " | total=" + FormatFloat(static_cast<float>(total));
                return out;
            });
    }
}
