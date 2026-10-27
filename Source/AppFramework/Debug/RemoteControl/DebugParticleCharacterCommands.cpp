#include "AppFramework/Debug/RemoteControl/DebugParticleCharacterCommands.h"

#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Debug/ParticleCharacterModel.h"
#include "AppFramework/Debug/MolecularBodyGpuCheck.h"
#include "AppFramework/Debug/ParticleCharacterOverlay.h"
#include "Renderer/ParticleCharacter/MolecularBody.h"
#include "Renderer/ParticleCharacter/ParticleCharacterSelfTest.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "AppFramework/Debug/RemoteControl/DebugCommandArgs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        using Foundation::MillisecondsSince;

        void ComputeBounds(const std::vector<float>& points, float outMin[3], float outMax[3])
        {
            for (int k = 0; k < 3; ++k) {
                outMin[k] = (std::numeric_limits<float>::max)();
                outMax[k] = (std::numeric_limits<float>::lowest)();
            }
            for (size_t i = 0; i + 2 < points.size(); i += 3) {
                for (int k = 0; k < 3; ++k) {
                    outMin[k] = (std::min)(outMin[k], points[i + k]);
                    outMax[k] = (std::max)(outMax[k], points[i + k]);
                }
            }
        }

        std::string ViewCommand(ParticleCharacterOverlay& overlay, const std::vector<std::string>& args)
        {
            constexpr const char* USAGE = "ERR usage: pchar.view on [model=fox|vroid] [count=8000] [clip=0] [mode=scatter|swirl|melt] [height=1.5] [at=x,y,z] [cycle=0] | off | status | release | form | mode <m> | cycle <s>";
            if (args.empty()) {
                return USAGE;
            }
            const std::string& sub = args[0];
            if (sub == "off") {
                return overlay.Stop();
            }
            if (sub == "status") {
                return overlay.Status();
            }
            if (sub == "release" || sub == "form") {
                if (!overlay.IsRunning()) {
                    return "ERR pchar.view is not running";
                }
                overlay.SetBondGoal(sub == "release" ? 0.0f : 1.0f);
                return overlay.Status();
            }
            if (sub == "mode") {
                Render::ParticleCharacter::MolecularForce force;
                if (args.size() != 2 || !ParseMolecularForce(args[1], force)) {
                    return USAGE;
                }
                overlay.SetForce(force);
                return overlay.Status();
            }
            if (sub == "cycle") {
                if (args.size() != 2) {
                    return USAGE;
                }
                const auto v = args[1] == "0" ? std::optional<float>(0.0f) : ParsePositiveFloat(args[1]);
                if (!v) { return USAGE; }
                overlay.SetCycle(*v);
                return overlay.Status();
            }
            if (sub != "on") {
                return USAGE;
            }
            std::map<std::string, std::string> kv;
            const std::vector<std::string> rest(args.begin() + 1, args.end());
            if (!ParseKeyValues(rest, { "model", "count", "clip", "mode", "height", "at", "cycle" }, kv)) {
                return USAGE;
            }
            ParticleCharacterViewSettings settings;
            if (kv.count("model")) { settings.model = kv["model"]; }
            if (kv.count("count")) {
                const auto v = ParseUintInRange(kv["count"], 1, 200000);
                if (!v) { return USAGE; }
                settings.count = *v;
            }
            if (kv.count("clip")) {
                const auto v = ParseUintInRange(kv["clip"], 0, 1024);
                if (!v) { return USAGE; }
                settings.clip = *v;
            }
            if (kv.count("mode") && !ParseMolecularForce(kv["mode"], settings.force)) { return USAGE; }
            if (kv.count("height")) {
                const auto v = ParsePositiveFloat(kv["height"]);
                if (!v) { return USAGE; }
                settings.height = *v;
            }
            if (kv.count("at") && !ParseFloat3(kv["at"], settings.at)) { return USAGE; }
            if (kv.count("cycle")) {
                const auto v = kv["cycle"] == "0" ? std::optional<float>(0.0f) : ParsePositiveFloat(kv["cycle"]);
                if (!v) { return USAGE; }
                settings.cycle = *v;
            }
            return overlay.Start(settings);
        }

        std::string GpuCheckCommand(ApplicationCore& app, const std::vector<std::string>& args)
        {
            constexpr const char* USAGE = "ERR usage: pchar.gpucheck [count=8000] [steps=120] [mode=scatter|swirl|melt] [model=fox|vroid]";
            std::map<std::string, std::string> kv;
            if (!ParseKeyValues(args, { "count", "steps", "mode", "model" }, kv)) {
                return USAGE;
            }
            uint32_t count = 8000;
            uint32_t steps = 120;
            std::string mode = "scatter";
            std::string model = "fox";
            if (kv.count("count")) {
                const auto v = ParseUintInRange(kv["count"], 1, 200000);
                if (!v) { return USAGE; }
                count = *v;
            }
            if (kv.count("steps")) {
                const auto v = ParseUintInRange(kv["steps"], 3, 2000);
                if (!v) { return USAGE; }
                steps = *v;
            }
            if (kv.count("mode")) {
                mode = kv["mode"];
                if (mode != "scatter" && mode != "swirl" && mode != "melt") { return USAGE; }
            }
            if (kv.count("model")) {
                model = kv["model"];
                if (!ParticleCharacterModelPath(model)) { return USAGE; }
            }
            return RunMolecularBodyGpuCheck(app, model, mode, count, steps);
        }

        std::string BakeCommand(const std::vector<std::string>& args)
        {
            constexpr const char* USAGE = "ERR usage: pchar.bake [count=20000] [seed=1] [model=fox|vroid] [clip=0]";
            std::map<std::string, std::string> kv;
            if (!ParseKeyValues(args, { "count", "seed", "model", "clip" }, kv)) {
                return USAGE;
            }
            uint32_t count = 20000;
            uint32_t seed = 1;
            uint32_t clip = 0;
            std::string model = "fox";
            if (kv.count("count")) {
                const auto v = ParseUintInRange(kv["count"], 1, 1000000);
                if (!v) { return USAGE; }
                count = *v;
            }
            if (kv.count("seed")) {
                const auto v = ParseUintInRange(kv["seed"], 0, 0xFFFFFFFFu);
                if (!v) { return USAGE; }
                seed = *v;
            }
            if (kv.count("clip")) {
                const auto v = ParseUintInRange(kv["clip"], 0, 100000);
                if (!v) { return USAGE; }
                clip = *v;
            }
            if (kv.count("model")) {
                model = kv["model"];
                if (!ParticleCharacterModelPath(model)) { return USAGE; }
            }

            ParticleCharacterModel baked;
            std::string error;
            if (!LoadParticleCharacterModel(model, count, seed, clip, baked, error)) {
                return "ERR " + error;
            }

            std::vector<float> poseP, poseN;
            const float t = 0.37f * baked.clipDuration;
            if (!EvaluateParticleCharacterPose(baked, t, poseP, poseN)) {
                return "ERR pose evaluation failed";
            }

            const size_t n = baked.set.samples.size();
            std::vector<float> targets(n * 3);
            Render::ParticleCharacter::InterpolateSamples(baked.set, poseP.data(), poseN.data(), 3, targets.data(), nullptr);

            float maxErr = 0.0f;
            const size_t checks = (std::min<size_t>)(1000, n);
            for (size_t c = 0; c < checks; ++c) {
                const size_t i = checks > 1 ? c * (n - 1) / (checks - 1) : 0;
                const Render::ParticleCharacter::SurfaceSample& s = baked.set.samples[i];
                const float b0 = 1.0f - s.b1 - s.b2;
                for (int k = 0; k < 3; ++k) {
                    const float p = b0 * poseP[static_cast<size_t>(s.vertex[0]) * 3 + k]
                                  + s.b1 * poseP[static_cast<size_t>(s.vertex[1]) * 3 + k]
                                  + s.b2 * poseP[static_cast<size_t>(s.vertex[2]) * 3 + k];
                    maxErr = (std::max)(maxErr, std::fabs(p - targets[i * 3 + k]));
                }
            }

            float bmin[3], bmax[3];
            ComputeBounds(targets, bmin, bmax);

            std::ostringstream out;
            out << std::fixed << std::setprecision(4);
            out << "OK model=" << model << " samples=" << n << " vertices=" << baked.set.vertexCount
                << " triangles=" << baked.set.triangleCount << " area=" << baked.set.totalArea
                << " sampleMs=" << baked.sampleMs << " bakeMs=" << baked.bakeMs
                << " clips=" << baked.data.animations.size() << " clipDuration=" << baked.clipDuration
                << " maxErr=" << maxErr
                << " min=" << bmin[0] << "," << bmin[1] << "," << bmin[2]
                << " max=" << bmax[0] << "," << bmax[1] << "," << bmax[2];
            return out.str();
        }

        std::string SimCommand(const std::vector<std::string>& args)
        {
            using Render::ParticleCharacter::MolecularBody;
            using Render::ParticleCharacter::MolecularBodySettings;
            using Render::ParticleCharacter::MolecularForce;

            constexpr const char* USAGE = "ERR usage: pchar.sim [count=20000] [steps=180] [mode=scatter|swirl|melt] [model=fox|vroid] [parallel=1]";
            std::map<std::string, std::string> kv;
            if (!ParseKeyValues(args, { "count", "steps", "mode", "model", "parallel" }, kv)) {
                return USAGE;
            }
            uint32_t count = 20000;
            uint32_t steps = 180;
            bool parallel = true;
            std::string model = "fox";
            std::string mode = "scatter";
            if (kv.count("count")) {
                const auto v = ParseUintInRange(kv["count"], 1, 1000000);
                if (!v) { return USAGE; }
                count = *v;
            }
            if (kv.count("steps")) {
                const auto v = ParseUintInRange(kv["steps"], 3, 2000);
                if (!v) { return USAGE; }
                steps = *v;
            }
            if (kv.count("parallel")) {
                const auto v = ParseUintInRange(kv["parallel"], 0, 1);
                if (!v) { return USAGE; }
                parallel = *v != 0;
            }
            if (kv.count("model")) {
                model = kv["model"];
                if (!ParticleCharacterModelPath(model)) { return USAGE; }
            }
            if (kv.count("mode")) {
                mode = kv["mode"];
                if (mode != "scatter" && mode != "swirl" && mode != "melt") { return USAGE; }
            }
            if (static_cast<uint64_t>(count) * steps > 20000000ull) {
                return "ERR count*steps too large (5 s debug timeout)";
            }

            ParticleCharacterModel baked;
            std::string error;
            if (!LoadParticleCharacterModel(model, count, 1, 0, baked, error)) {
                return "ERR " + error;
            }

            const size_t n = baked.set.samples.size();
            std::vector<float> poseP, poseN;
            std::vector<float> targets(n * 3), normals(n * 3);
            const auto evaluateTargets = [&](float t) -> bool {
                if (!EvaluateParticleCharacterPose(baked, t, poseP, poseN)) {
                    return false;
                }
                Render::ParticleCharacter::InterpolateSamples(baked.set, poseP.data(), poseN.data(), 3,
                                                              targets.data(), normals.data());
                return true;
            };

            if (!evaluateTargets(0.0f)) {
                return "ERR pose evaluation failed";
            }
            float bmin[3], bmax[3];
            ComputeBounds(targets, bmin, bmax);
            const float height = bmax[1] - bmin[1];
            if (!(height > 0.0f)) {
                return "ERR model has no height";
            }

            MolecularBodySettings settings;
            settings.force = mode == "swirl" ? MolecularForce::Swirl
                           : mode == "melt" ? MolecularForce::Melt
                                            : MolecularForce::Scatter;
            settings.scatterSpeed = 1.5f * height;
            settings.gravity = 9.8f * height / 1.6f;
            settings.swirlRise = 0.5f * height;
            settings.floorY = bmin[1];
            settings.centre[0] = 0.5f * (bmin[0] + bmax[0]);
            settings.centre[1] = bmin[1];
            settings.centre[2] = 0.5f * (bmin[2] + bmax[2]);

            MolecularBody bodies[2];
            const bool flags[2] = { parallel, !parallel };
            for (MolecularBody& body : bodies) {
                body.Reset(targets.data(), static_cast<uint32_t>(n), 1);
            }

            constexpr float DT = 1.0f / 60.0f;
            const uint32_t third = steps / 3;
            float holdMaxDist = 0.0f;
            float releaseMeanDist = 0.0f;
            double stepMs = 0.0;
            double targetMs = 0.0;

            for (uint32_t s = 0; s < steps; ++s) {
                if (s == third) {
                    for (MolecularBody& body : bodies) { body.SetBondGoal(0.0f); }
                } else if (s == 2 * third) {
                    for (MolecularBody& body : bodies) { body.SetBondGoal(1.0f); }
                }

                auto start = std::chrono::steady_clock::now();
                if (!evaluateTargets(static_cast<float>(s) * DT)) {
                    return "ERR pose evaluation failed";
                }
                targetMs += MillisecondsSince(start);

                for (int b = 0; b < 2; ++b) {
                    start = std::chrono::steady_clock::now();
                    bodies[b].Step(targets.data(), normals.data(), DT, settings, flags[b]);
                    if (b == 0) {
                        stepMs += MillisecondsSince(start);
                    }
                }

                if (s + 1 == third) {
                    holdMaxDist = bodies[0].MaxTargetDistance(targets.data());
                }
                if (s + 1 == 2 * third) {
                    const std::vector<float>& positions = bodies[0].Positions();
                    double sum = 0.0;
                    for (size_t i = 0; i < n; ++i) {
                        const float dx = positions[i * 3 + 0] - targets[i * 3 + 0];
                        const float dy = positions[i * 3 + 1] - targets[i * 3 + 1];
                        const float dz = positions[i * 3 + 2] - targets[i * 3 + 2];
                        sum += std::sqrt(dx * dx + dy * dy + dz * dz);
                    }
                    releaseMeanDist = static_cast<float>(sum / static_cast<double>(n));
                }
            }

            const float finalMaxDist = bodies[0].MaxTargetDistance(targets.data());

            const uint64_t floorHits = bodies[0].FloorHits();

            const std::vector<float>& primary = bodies[0].Positions();
            const std::vector<float>& other = bodies[1].Positions();
            float minY = (std::numeric_limits<float>::max)();
            uint32_t mismatch = 0;
            for (size_t i = 0; i < n; ++i) {
                minY = (std::min)(minY, primary[i * 3 + 1]);
                if (std::memcmp(&primary[i * 3], &other[i * 3], sizeof(float) * 3) != 0) {
                    ++mismatch;
                }
            }

            std::ostringstream out;
            out << std::fixed << std::setprecision(4);
            out << "OK model=" << model << " count=" << n << " steps=" << steps << " mode=" << mode
                << " holdMaxDist=" << holdMaxDist << " releaseMeanDist=" << releaseMeanDist
                << " finalMaxDist=" << finalMaxDist << " floorHits=" << floorHits
                << " floorMargin=" << (minY - settings.floorY) << " mismatch=" << mismatch
                << " stepMs=" << stepMs << " targetMs=" << targetMs << " height=" << height;
            return out.str();
        }
    }

    void RegisterParticleCharacterCommands(DebugCommandRegistry& registry, ApplicationCore& app)
    {
        registry.Register("pchar.selftest", "run the particle-character CPU self test (surface sampling, molecular body, serial vs parallel identity)",
            [](const std::vector<std::string>& args) -> std::string {
                if (!args.empty()) {
                    return "ERR usage: pchar.selftest";
                }
                try {
                    return Render::ParticleCharacter::RunParticleCharacterSelfTest();
                } catch (const std::exception& e) {
                    return std::string("ERR exception: ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });

        registry.Register("pchar.bake", "[count=20000] [seed=1] [model=fox|vroid] [clip=0] - sample a skinned model surface, bake a VAT clip, check interpolation consistency",
            [](const std::vector<std::string>& args) -> std::string {
                try {
                    return BakeCommand(args);
                } catch (const std::exception& e) {
                    return std::string("ERR exception: ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });

        registry.Register("pchar.sim", "[count=20000] [steps=180] [mode=scatter|swirl|melt] [model=fox|vroid] [parallel=1] - hold / release / re-form a molecular body on an animated model, diff serial vs parallel bitwise",
            [](const std::vector<std::string>& args) -> std::string {
                try {
                    return SimCommand(args);
                } catch (const std::exception& e) {
                    return std::string("ERR exception: ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });

        registry.Register("pchar.view", "on [model=fox|vroid] [count=8000] [clip=0] [mode=scatter|swirl|melt] [height=1.5] [at=x,y,z] [cycle=0] | off | status | release | form | mode <m> | cycle <s> - draw a molecular body on the animated model as ImGui points (no depth test)",
            [&app](const std::vector<std::string>& args) -> std::string {
                try {
                    return ViewCommand(app.GetParticleCharacterOverlay(), args);
                } catch (const std::exception& e) {
                    return std::string("ERR exception: ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });

        registry.Register("pchar.gpucheck", "[count=8000] [steps=120] [mode=scatter|swirl|melt] [model=fox|vroid] - run the pchar.sim schedule on the CPU and on the DX12 compute twin, compare the final state",
            [&app](const std::vector<std::string>& args) -> std::string {
                try {
                    return GpuCheckCommand(app, args);
                } catch (const std::exception& e) {
                    return std::string("ERR exception: ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });
    }
}
