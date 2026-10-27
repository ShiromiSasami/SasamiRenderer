#include "AppFramework/Debug/RemoteControl/DebugCrowdCommands.h"

#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"
#include "AppFramework/ApplicationCore.h"
#include "AppFramework/ApplicationResourcePaths.h"
#include "AppFramework/Loader/ModelLoader.h"
#include "Renderer/Crowd/CrowdSelfTest.h"
#include "Renderer/Passes/Geometry/CrowdRenderPass.h"
#include "Renderer/Crowd/CrowdSimulation.h"
#include "Renderer/Crowd/CrowdStore.h"
#include "Renderer/Crowd/GpuCrowdUpdater.h"
#include "Renderer/Crowd/VatAnimation.h"
#include "Renderer/Crowd/VatSelfTest.h"
#include "Renderer/Crowd/VatSkinning.h"
#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Scene/AnimationController.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "AppFramework/Debug/RemoteControl/DebugCommandArgs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        bool BitEqual(float a, float b)
        {
            return std::memcmp(&a, &b, sizeof(float)) == 0;
        }

        bool AgentBitEqual(const Render::Crowd::CrowdStore& a, const Render::Crowd::CrowdStore& b, uint32_t i)
        {
            return BitEqual(a.posX[i], b.posX[i]) && BitEqual(a.posY[i], b.posY[i]) && BitEqual(a.posZ[i], b.posZ[i])
                && BitEqual(a.velX[i], b.velX[i]) && BitEqual(a.velZ[i], b.velZ[i])
                && BitEqual(a.animTime[i], b.animTime[i]) && BitEqual(a.animRate[i], b.animRate[i])
                && a.clip[i] == b.clip[i];
        }

        uint32_t CountMismatches(const Render::Crowd::CrowdStore& a, const Render::Crowd::CrowdStore& b)
        {
            uint32_t mismatches = 0;
            for (uint32_t i = 0; i < a.Count(); ++i) {
                if (!AgentBitEqual(a, b, i)) {
                    ++mismatches;
                }
            }
            return mismatches;
        }

        float MaxFloatDiff(const Render::Crowd::CrowdStore& a, const Render::Crowd::CrowdStore& b)
        {
            const std::vector<float>* const fieldsA[] = { &a.posX, &a.posY, &a.posZ, &a.velX, &a.velZ, &a.animTime, &a.animRate };
            const std::vector<float>* const fieldsB[] = { &b.posX, &b.posY, &b.posZ, &b.velX, &b.velZ, &b.animTime, &b.animRate };
            float maxDiff = 0.0f;
            for (int f = 0; f < 7; ++f) {
                for (uint32_t i = 0; i < a.Count(); ++i) {
                    maxDiff = (std::max)(maxDiff, std::fabs((*fieldsA[f])[i] - (*fieldsB[f])[i]));
                }
            }
            return maxDiff;
        }

        using Foundation::MillisecondsSince;

        struct BakeErrors
        {
            float maxPos = 0.0f;
            float maxNormal = 0.0f;
            float midPos = 0.0f;
        };

        // Direct skinning of every vertex (meshes concatenated) at (srcClip, t), compared with the baked sample.
        void CompareAgainstBake(const Render::Crowd::VatBakedAnimation& baked, uint32_t clipIndex, float t,
                                const std::vector<float>& matrices, const SkinnedModelData& model,
                                float& maxPos, float& maxNormal)
        {
            uint32_t vertexIndex = 0;
            for (const Render::SkinnedMesh& mesh : model.meshes) {
                for (const Render::SkinnedVertex& vertex : mesh.vertices) {
                    float refPos[3], refNormal[3], pos[3], normal[3];
                    Render::Crowd::SkinVertexCpu(vertex, matrices.data(), refPos, refNormal);
                    if (!Render::Crowd::SampleVatVertex(baked, clipIndex, t, vertexIndex, vertex, pos, normal)) {
                        maxPos = std::numeric_limits<float>::infinity();
                        return;
                    }
                    for (int k = 0; k < 3; ++k) {
                        maxPos = (std::max)(maxPos, std::fabs(refPos[k] - pos[k]));
                        maxNormal = (std::max)(maxNormal, std::fabs(refNormal[k] - normal[k]));
                    }
                    ++vertexIndex;
                }
            }
        }

        const char* VatModeName(Render::Crowd::VatMode mode)
        {
            return mode == Render::Crowd::VatMode::Vertex ? "vertex" : "bone";
        }

        const char* UpdateModeName(Render::Crowd::CrowdUpdateMode mode)
        {
            switch (mode) {
            case Render::Crowd::CrowdUpdateMode::Gpu: return "gpu";
            case Render::Crowd::CrowdUpdateMode::Cpu: return "cpu";
            default: return "off";
            }
        }

        const char* StepModeName(Render::Crowd::CrowdGpuStepMode mode)
        {
            return mode == Render::Crowd::CrowdGpuStepMode::Recorded ? "recorded" : "immediate";
        }

        // Loads Fox, bakes it and hands it to the crowd system. outError is filled on failure.
        bool LoadFoxCrowdAsset(Render::Crowd::CrowdSystem& crowd, Render::Crowd::VatMode mode, float sampleRate, std::string& outError)
        {
            const std::string path = ApplicationResourcePaths::ResolveAssetPathString("Models/Fox/Fox.gltf");
            SkinnedModelData model;
            if (!LoadGLTFSkinned(path, model)) {
                outError = "load failed " + path;
                return false;
            }

            Render::Crowd::VatBakeDesc desc;
            desc.mode = mode;
            desc.sampleRate = sampleRate;
            Render::Crowd::VatBakedAnimation baked;
            std::string error;
            if (!Render::Crowd::BakeVat(model.meshes, model.skeleton, model.animations, desc, baked, error)) {
                outError = "bake: " + error;
                return false;
            }
            if (!crowd.SetAsset(model.meshes, baked, error)) {
                outError = "set asset: " + error;
                return false;
            }
            return true;
        }
    }

    void RegisterCrowdCommands(DebugCommandRegistry& registry, ApplicationCore& app)
    {
        registry.Register("crowd.selftest", "run the CPU crowd store / update self test (serial vs parallel identity, bounds, wrap, spawn determinism)",
            [](const std::vector<std::string>& args) -> std::string {
                if (!args.empty()) {
                    return "ERR usage: crowd.selftest";
                }
                return Render::Crowd::RunCrowdSelfTest();
            });

        registry.Register("crowd.sim", "<count 1..1000000> <steps 1..10000> [cpu|gpu|both] - step a synthetic crowd on CPU serial / CPU parallel / GPU, time each and diff bitwise (blocks until GPU idle)",
            [&app](const std::vector<std::string>& args) -> std::string {
                try {
                    constexpr const char* USAGE = "ERR usage: crowd.sim <count> <steps> [cpu|gpu|both]";
                    if (args.size() < 2 || args.size() > 3) {
                        return USAGE;
                    }
                    const auto count = ParseUintInRange(args[0], 1, 1000000);
                    const auto steps = ParseUintInRange(args[1], 1, 10000);
                    if (!count || !steps) {
                        return USAGE;
                    }
                    bool runCpu = true;
                    bool runGpu = true;
                    if (args.size() == 3) {
                        if (args[2] == "cpu") {
                            runGpu = false;
                        } else if (args[2] == "gpu") {
                            runCpu = false;
                        } else if (args[2] != "both") {
                            return USAGE;
                        }
                    }

                    Render::Crowd::CrowdSimParams params;
                    params.boundsMinX = -100.0f;
                    params.boundsMaxX = 100.0f;
                    params.boundsMinZ = -100.0f;
                    params.boundsMaxZ = 100.0f;
                    params.deltaTime = 1.0f / 60.0f;
                    params.clipCount = 3;
                    params.clipDurations[0] = 1.0f;
                    params.clipDurations[1] = 0.7f;
                    params.clipDurations[2] = 2.3f;

                    Render::Crowd::CrowdStore spawned;
                    Render::Crowd::SpawnCrowd(spawned, *count, params, 1.0f, 3.0f, 1234u);

                    std::ostringstream out;
                    out << std::fixed;
                    out << "OK count=" << *count << " steps=" << *steps;

                    Render::Crowd::CrowdStore serialStore;
                    if (runCpu) {
                        serialStore = spawned;
                        Render::Crowd::CrowdStore parallelStore = spawned;

                        auto start = std::chrono::steady_clock::now();
                        for (uint32_t s = 0; s < *steps; ++s) {
                            Render::Crowd::StepCrowdCpu(serialStore, params, Render::Crowd::CrowdUpdateBackend::CpuSerial);
                        }
                        const double serialMs = MillisecondsSince(start);

                        start = std::chrono::steady_clock::now();
                        for (uint32_t s = 0; s < *steps; ++s) {
                            Render::Crowd::StepCrowdCpu(parallelStore, params, Render::Crowd::CrowdUpdateBackend::CpuParallel);
                        }
                        const double parallelMs = MillisecondsSince(start);

                        out << std::setprecision(2) << " serialMs=" << serialMs << " parallelMs=" << parallelMs
                            << " cpuMismatch=" << CountMismatches(serialStore, parallelStore);
                    }

                    if (runGpu) {
                        IRHIDevice* device = app.GetRenderer().GetRhiDevice();
                        if (!device) {
                            return "ERR no RHI device";
                        }
                        static Render::Crowd::GpuCrowdUpdater updater;
                        std::string error;
                        if (!updater.IsInitialized() && !updater.Initialize(*device, error)) {
                            return "ERR " + error;
                        }

                        auto start = std::chrono::steady_clock::now();
                        if (!updater.Upload(spawned, error)) {
                            return "ERR " + error;
                        }
                        const double uploadMs = MillisecondsSince(start);

                        start = std::chrono::steady_clock::now();
                        if (!updater.Step(params, *steps, error)) {
                            return "ERR " + error;
                        }
                        const double gpuMs = MillisecondsSince(start);

                        Render::Crowd::CrowdStore gpuStore;
                        gpuStore.Resize(*count);
                        start = std::chrono::steady_clock::now();
                        if (!updater.Readback(gpuStore, error)) {
                            return "ERR " + error;
                        }
                        const double readbackMs = MillisecondsSince(start);

                        out << std::setprecision(2) << " uploadMs=" << uploadMs << " gpuMs=" << gpuMs << " readbackMs=" << readbackMs;
                        if (runCpu) {
                            out << " gpuMismatch=" << CountMismatches(serialStore, gpuStore);
                            out << std::defaultfloat << std::setprecision(6) << " gpuMaxDiff=" << MaxFloatDiff(serialStore, gpuStore);
                        }
                    }
                    return out.str();
                } catch (const std::exception& e) {
                    return std::string("ERR ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });

        registry.Register("crowd.bake", "bake Fox.gltf clips into a vertex-animation (vertex) or bone-matrix (bone) table and check it against direct skinning; `crowd.bake selftest` runs the synthetic test",
            [](const std::vector<std::string>& args) -> std::string {
                try {
                    constexpr const char* USAGE = "ERR usage: crowd.bake <vertex|bone|selftest> [fps]";
                    if (args.empty() || args.size() > 2) {
                        return USAGE;
                    }
                    if (args[0] == "selftest") {
                        if (args.size() != 1) {
                            return USAGE;
                        }
                        return Render::Crowd::RunVatSelfTest();
                    }

                    Render::Crowd::VatMode mode;
                    if (args[0] == "vertex") {
                        mode = Render::Crowd::VatMode::Vertex;
                    } else if (args[0] == "bone") {
                        mode = Render::Crowd::VatMode::Bone;
                    } else {
                        return USAGE;
                    }
                    float fps = 30.0f;
                    if (args.size() == 2) {
                        const auto value = ParsePositiveFloat(args[1]);
                        if (!value) {
                            return USAGE;
                        }
                        fps = *value;
                    }

                    const std::string path = ApplicationResourcePaths::ResolveAssetPathString("Models/Fox/Fox.gltf");
                    SkinnedModelData model;
                    if (!LoadGLTFSkinned(path, model)) {
                        return "ERR load failed " + path;
                    }

                    Render::Crowd::VatBakeDesc desc;
                    desc.mode = mode;
                    desc.sampleRate = fps;
                    Render::Crowd::VatBakedAnimation baked;
                    std::string error;
                    const auto start = std::chrono::steady_clock::now();
                    const bool ok = Render::Crowd::BakeVat(model.meshes, model.skeleton, model.animations, desc, baked, error);
                    const double bakeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                    if (!ok) {
                        return "ERR bake: " + error;
                    }

                    const auto sharedSkeleton = std::make_shared<Render::Skeleton>(model.skeleton);
                    std::vector<float> matrices(static_cast<size_t>(Render::Skeleton::kMaxBones) * 16, 0.0f);

                    BakeErrors errors;
                    float unusedNormal = 0.0f;
                    for (uint32_t ci = 0; ci < baked.clips.size(); ++ci) {
                        const Render::Crowd::VatClip& clip = baked.clips[ci];
                        const float d = clip.durationSec;
                        // Fresh controller per clip, like the baker: channels a clip does not drive keep the
                        // previous clip's pose otherwise.
                        Render::AnimationController controller;
                        controller.SetSkeleton(sharedSkeleton);
                        controller.AddAnimation(model.animations[ci]);
                        for (uint32_t f = 0; f < clip.frameCount; ++f) {
                            const float t = clip.frameCount > 1 ? static_cast<float>(f) * d / static_cast<float>(clip.frameCount) : 0.0f;
                            controller.PlayAnimation(0, true);
                            controller.Update(t);
                            controller.GetBoneMatrices(matrices.data());
                            CompareAgainstBake(baked, ci, t, matrices, model, errors.maxPos, errors.maxNormal);

                            const float tMid = clip.frameCount > 1 ? (static_cast<float>(f) + 0.5f) * d / static_cast<float>(clip.frameCount) : 0.0f;
                            controller.PlayAnimation(0, true);
                            controller.Update(tMid);
                            controller.GetBoneMatrices(matrices.data());
                            CompareAgainstBake(baked, ci, tMid, matrices, model, errors.midPos, unusedNormal);
                        }
                    }

                    std::ostringstream out;
                    out << std::fixed;
                    out << "OK mode=" << (mode == Render::Crowd::VatMode::Vertex ? "vertex" : "bone")
                        << " fps=" << std::setprecision(2) << fps
                        << " clips=" << baked.clips.size()
                        << " frames=" << baked.totalFrames
                        << " vertices=" << baked.vertexCount
                        << " bones=" << baked.boneCount
                        << " bytes=" << baked.SizeInBytes()
                        << " bakeMs=" << std::setprecision(1) << bakeMs
                        << std::setprecision(6)
                        << " maxPosErr=" << errors.maxPos
                        << " maxNormalErr=" << errors.maxNormal
                        << " midFramePosErr=" << errors.midPos
                        << std::setprecision(3)
                        << " bounds=(" << baked.boundsMin[0] << "," << baked.boundsMin[1] << "," << baked.boundsMin[2] << ")-("
                        << baked.boundsMax[0] << "," << baked.boundsMax[1] << "," << baked.boundsMax[2] << ")"
                        << " clipList=";
                    for (size_t i = 0; i < baked.clips.size(); ++i) {
                        const Render::Crowd::VatClip& clip = baked.clips[i];
                        out << (i ? "," : "") << clip.name << ":" << clip.firstFrame << "+" << clip.frameCount;
                    }
                    return out.str();
                } catch (const std::exception& e) {
                    return std::string("ERR ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });

        registry.Register("crowd.asset", "[vertex|bone] [sampleRate] - bake Fox.gltf and upload it as the crowd's VAT asset (default vertex, 30)",
            [&app](const std::vector<std::string>& args) -> std::string {
                try {
                    constexpr const char* USAGE = "ERR usage: crowd.asset [vertex|bone] [sampleRate]";
                    if (args.size() > 2) {
                        return USAGE;
                    }
                    Render::Crowd::VatMode mode = Render::Crowd::VatMode::Vertex;
                    if (!args.empty()) {
                        if (args[0] == "vertex") {
                            mode = Render::Crowd::VatMode::Vertex;
                        } else if (args[0] == "bone") {
                            mode = Render::Crowd::VatMode::Bone;
                        } else {
                            return USAGE;
                        }
                    }
                    float sampleRate = 30.0f;
                    if (args.size() == 2) {
                        const auto value = ParsePositiveFloat(args[1]);
                        if (!value) {
                            return USAGE;
                        }
                        sampleRate = *value;
                    }

                    Render::Crowd::CrowdSystem& crowd = app.GetRenderer().GetCrowdSystem();
                    if (!crowd.IsInitialized()) {
                        return "ERR crowd system not initialized (DX12 only)";
                    }
                    std::string error;
                    if (!LoadFoxCrowdAsset(crowd, mode, sampleRate, error)) {
                        return "ERR " + error;
                    }

                    const Render::Crowd::CrowdVatGpuAsset& asset = crowd.Asset();
                    std::ostringstream out;
                    out << "OK mode=" << VatModeName(asset.Mode())
                        << " vertexCount=" << asset.VertexCount()
                        << " boneCount=" << asset.BoneCount()
                        << " clips=" << asset.ClipCount()
                        << " indexCount=" << asset.IndexCount();
                    return out.str();
                } catch (const std::exception& e) {
                    return std::string("ERR ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });

        registry.Register("crowd.spawn", "<count 0..1000000> [halfExtent=20] [seed=1] [height=1] - spawn the crowd (loads the vertex-mode Fox asset if none) and enable the crowd pass when count > 0",
            [&app](const std::vector<std::string>& args) -> std::string {
                try {
                    constexpr const char* USAGE = "ERR usage: crowd.spawn <count> [halfExtent] [seed] [height]";
                    if (args.empty() || args.size() > 4) {
                        return USAGE;
                    }
                    const auto count = ParseUintInRange(args[0], 0, 1000000);
                    if (!count) {
                        return USAGE;
                    }
                    float halfExtent = 20.0f;
                    if (args.size() >= 2) {
                        const auto value = ParsePositiveFloat(args[1]);
                        if (!value) {
                            return USAGE;
                        }
                        halfExtent = *value;
                    }
                    uint32_t seed = 1;
                    if (args.size() >= 3) {
                        const auto value = ParseUintInRange(args[2], 0, (std::numeric_limits<uint32_t>::max)());
                        if (!value) {
                            return USAGE;
                        }
                        seed = *value;
                    }
                    float height = 1.0f;
                    if (args.size() == 4) {
                        const auto value = ParsePositiveFloat(args[3]);
                        if (!value) {
                            return USAGE;
                        }
                        height = *value;
                    }

                    Render::Renderer& renderer = app.GetRenderer();
                    Render::Crowd::CrowdSystem& crowd = renderer.GetCrowdSystem();
                    if (!crowd.IsInitialized()) {
                        return "ERR crowd system not initialized (DX12 only)";
                    }
                    std::string error;
                    if (!crowd.HasAsset() && !LoadFoxCrowdAsset(crowd, Render::Crowd::VatMode::Vertex, 30.0f, error)) {
                        return "ERR " + error;
                    }
                    crowd.SetTargetHeight(height);
                    if (!crowd.Spawn(*count, halfExtent, seed, error)) {
                        return "ERR spawn: " + error;
                    }
                    renderer.SetCrowdEnabled(*count > 0);

                    std::ostringstream out;
                    out << "OK agents=" << crowd.AgentCount()
                        << " mode=" << VatModeName(crowd.Asset().Mode())
                        << " scale=" << crowd.ModelScale();
                    return out.str();
                } catch (const std::exception& e) {
                    return std::string("ERR ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });

        registry.Register("crowd.update", "<gpu|cpu|off> - choose who advances the crowd each frame",
            [&app](const std::vector<std::string>& args) -> std::string {
                try {
                    constexpr const char* USAGE = "ERR usage: crowd.update <gpu|cpu|off>";
                    if (args.size() != 1) {
                        return USAGE;
                    }
                    Render::Crowd::CrowdUpdateMode mode;
                    if (args[0] == "gpu") {
                        mode = Render::Crowd::CrowdUpdateMode::Gpu;
                    } else if (args[0] == "cpu") {
                        mode = Render::Crowd::CrowdUpdateMode::Cpu;
                    } else if (args[0] == "off") {
                        mode = Render::Crowd::CrowdUpdateMode::Off;
                    } else {
                        return USAGE;
                    }
                    Render::Crowd::CrowdSystem& crowd = app.GetRenderer().GetCrowdSystem();
                    if (!crowd.IsInitialized()) {
                        return "ERR crowd system not initialized (DX12 only)";
                    }
                    std::string error;
                    if (!crowd.SetUpdateMode(mode, error)) {
                        return "ERR " + error;
                    }
                    return std::string("OK update=") + UpdateModeName(crowd.GetUpdateMode());
                } catch (const std::exception& e) {
                    return std::string("ERR ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });

        registry.Register("crowd.status", "report the crowd pass / system state",
            [&app](const std::vector<std::string>& args) -> std::string {
                if (!args.empty()) {
                    return "ERR usage: crowd.status";
                }
                Render::Renderer& renderer = app.GetRenderer();
                Render::Crowd::CrowdSystem& crowd = renderer.GetCrowdSystem();
                std::ostringstream out;
                out << "OK enabled=" << (renderer.GetCrowdEnabled() ? 1 : 0)
                    << " initialized=" << (crowd.IsInitialized() ? 1 : 0)
                    << " hasAsset=" << (crowd.HasAsset() ? 1 : 0)
                    << " agents=" << crowd.AgentCount()
                    << " update=" << UpdateModeName(crowd.GetUpdateMode())
                    << " scale=" << crowd.ModelScale()
                    << " yOffset=" << crowd.ModelYOffset();
                if (crowd.HasAsset()) {
                    const Render::Crowd::CrowdVatGpuAsset& asset = crowd.Asset();
                    out << " assetMode=" << VatModeName(asset.Mode())
                        << " vertexCount=" << asset.VertexCount()
                        << " indexCount=" << asset.IndexCount();
                }

                const Render::Crowd::CrowdCullSettings& cull = crowd.CullSettings();
                Render::Crowd::CrowdGpuCuller& culler = crowd.Culler();
                out << " stepMode=" << StepModeName(crowd.GetGpuStepMode())
                    << " cull=" << (cull.enabled ? "on" : "off")
                    << " frustum=" << (cull.frustum ? "on" : "off");
                if (crowd.HasAsset()) {
                    const Render::Crowd::CrowdVatGpuAsset& asset = crowd.Asset();
                    out << " lodCount=" << asset.LodCount();
                    for (uint32_t i = 0; i < CROWD_MAX_LODS; ++i) {
                        if (i < asset.LodCount()) {
                            out << " lod" << i << "Idx=" << asset.Lod(i).indexCount;
                        }
                    }
                } else {
                    out << " lodCount=0";
                }
                out << " cullerReady=" << (culler.IsInitialized() ? 1 : 0)
                    << " capacity=" << culler.Capacity();
                if (culler.IsInitialized() && culler.RecordedLodCount() > 0) {
                    uint32_t counts[CROWD_MAX_LODS] = {};
                    std::string error;
                    if (culler.ReadVisibleCounts(counts, error)) {
                        uint64_t total = 0;
                        for (uint32_t i = 0; i < CROWD_MAX_LODS; ++i) {
                            out << " visible" << i << "=" << counts[i];
                            total += counts[i];
                        }
                        out << " visibleTotal=" << total;
                    } else {
                        out << " visibleErr=" << error;
                    }
                }
                return out.str();
            });

        registry.Register("crowd.cull", "[on|off | frustum [on|off]] - GPU culling master switch / frustum test switch (no argument reports)",
            [&app](const std::vector<std::string>& args) -> std::string {
                try {
                    constexpr const char* USAGE = "ERR usage: crowd.cull [on|off | frustum [on|off]]";
                    Render::Crowd::CrowdCullSettings& cull = app.GetRenderer().GetCrowdSystem().CullSettings();
                    if (args.size() == 1 && args[0] == "frustum") {
                        // report only
                    } else if (args.size() == 1) {
                        const auto value = ParseOnOff(args[0]);
                        if (!value) {
                            return USAGE;
                        }
                        cull.enabled = *value;
                    } else if (args.size() == 2 && args[0] == "frustum") {
                        const auto value = ParseOnOff(args[1]);
                        if (!value) {
                            return USAGE;
                        }
                        cull.frustum = *value;
                    } else if (!args.empty()) {
                        return USAGE;
                    }
                    return std::string("OK cull=") + (cull.enabled ? "on" : "off") + " frustum=" + (cull.frustum ? "on" : "off");
                } catch (const std::exception& e) {
                    return std::string("ERR ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });

        registry.Register("crowd.shadow", "[on|off] - crowd shadow casting into CSM/spot/point maps (no argument reports)",
            [&app](const std::vector<std::string>& args) -> std::string {
                try {
                    Render::CrowdRenderPass* pass = app.GetRenderer().GetCrowdRenderPass();
                    if (pass == nullptr) {
                        return "ERR crowd pass unavailable";
                    }
                    if (args.size() == 1) {
                        const auto value = ParseOnOff(args[0]);
                        if (!value) {
                            return "ERR usage: crowd.shadow [on|off]";
                        }
                        pass->SetCastShadows(*value);
                    } else if (!args.empty()) {
                        return "ERR usage: crowd.shadow [on|off]";
                    }
                    return std::string("OK shadow=") + (pass->GetCastShadows() ? "on" : "off");
                } catch (const std::exception& e) {
                    return std::string("ERR ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });

        registry.Register("crowd.lod", "[<d1> <d2> [force|auto]] - LOD switch distances (0 < d1 <= d2) and optional forced LOD; no argument reports settings and the asset's LOD table",
            [&app](const std::vector<std::string>& args) -> std::string {
                try {
                    constexpr const char* USAGE = "ERR usage: crowd.lod [<d1> <d2> [force|auto]]";
                    if (args.size() == 1 || args.size() > 3) {
                        return USAGE;
                    }
                    Render::Crowd::CrowdSystem& crowd = app.GetRenderer().GetCrowdSystem();
                    Render::Crowd::CrowdCullSettings& cull = crowd.CullSettings();
                    if (!args.empty()) {
                        const auto d1 = ParsePositiveFloat(args[0]);
                        const auto d2 = ParsePositiveFloat(args[1]);
                        if (!d1 || !d2 || *d1 > *d2) {
                            return USAGE;
                        }
                        int force = -1;
                        if (args.size() == 3 && args[2] != "auto") {
                            const auto value = ParseUintInRange(args[2], 0, CROWD_MAX_LODS - 1);
                            if (!value) {
                                return USAGE;
                            }
                            force = static_cast<int>(*value);
                        }
                        cull.lodDistance[0] = *d1;
                        cull.lodDistance[1] = *d2;
                        cull.forceLod = force;
                    }

                    std::ostringstream out;
                    out << "OK lodDistance0=" << cull.lodDistance[0] << " lodDistance1=" << cull.lodDistance[1]
                        << " forceLod=" << cull.forceLod;
                    if (args.empty() && crowd.HasAsset()) {
                        const Render::Crowd::CrowdVatGpuAsset& asset = crowd.Asset();
                        out << " lodCount=" << asset.LodCount();
                        for (uint32_t i = 0; i < asset.LodCount(); ++i) {
                            const Render::Crowd::CrowdLodRange lod = asset.Lod(i);
                            out << " lod" << i << "=indexCount:" << lod.indexCount << ",firstIndex:" << lod.firstIndex;
                        }
                        out << " sphereCenterY=" << asset.SphereCenterY() << " sphereRadius=" << asset.SphereRadius();
                    }
                    return out.str();
                } catch (const std::exception& e) {
                    return std::string("ERR ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });

        registry.Register("crowd.step", "[recorded|immediate] - how the GPU crowd step reaches the GPU: recorded into the frame encoder, or immediate with its own submit and wait (no argument reports)",
            [&app](const std::vector<std::string>& args) -> std::string {
                constexpr const char* USAGE = "ERR usage: crowd.step [recorded|immediate]";
                if (args.size() > 1) {
                    return USAGE;
                }
                Render::Crowd::CrowdSystem& crowd = app.GetRenderer().GetCrowdSystem();
                if (args.size() == 1) {
                    if (args[0] == "recorded") {
                        crowd.SetGpuStepMode(Render::Crowd::CrowdGpuStepMode::Recorded);
                    } else if (args[0] == "immediate") {
                        crowd.SetGpuStepMode(Render::Crowd::CrowdGpuStepMode::Immediate);
                    } else {
                        return USAGE;
                    }
                }
                return std::string("OK step=") + StepModeName(crowd.GetGpuStepMode());
            });

        registry.Register("crowd.material", "<r> <g> <b> <roughness> - constant GBuffer material of the crowd pass (linear albedo)",
            [&app](const std::vector<std::string>& args) -> std::string {
                try {
                    constexpr const char* USAGE = "ERR usage: crowd.material <r> <g> <b> <roughness>";
                    if (args.size() != 4) {
                        return USAGE;
                    }
                    float values[4];
                    for (int i = 0; i < 4; ++i) {
                        size_t consumed = 0;
                        values[i] = std::stof(args[i], &consumed);
                        if (consumed != args[i].size() || !std::isfinite(values[i]) || values[i] < 0.0f) {
                            return USAGE;
                        }
                    }
                    CrowdRenderPass* pass = app.GetRenderer().GetCrowdRenderPass();
                    if (!pass) {
                        return "ERR no crowd pass";
                    }
                    pass->SetAlbedo(values[0], values[1], values[2]);
                    pass->SetRoughness(values[3]);
                    std::ostringstream out;
                    out << "OK albedo=" << values[0] << "," << values[1] << "," << values[2] << " roughness=" << values[3];
                    return out.str();
                } catch (const std::exception& e) {
                    return std::string("ERR ") + e.what();
                } catch (...) {
                    return "ERR unknown exception";
                }
            });
    }
}
