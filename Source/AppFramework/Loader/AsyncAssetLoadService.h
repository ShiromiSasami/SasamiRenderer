#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "Component/MeshComponent.h"
#include "Foundation/Jobs/JobCounter.h"
#include "Renderer/Scene/IblSystem.h"

namespace SasamiRenderer
{
    // Runs CPU-side asset loading (glTF/OBJ parse, texture decode, HDR skybox decode,
    // IBL pregeneration) on JobSystem workers and applies the results on the main thread
    // via PumpCompletions(), so game OnInit can return immediately instead of blocking
    // the message pump. Owned by ApplicationCore; single instance per process.
    //
    // Threading contract: Request*/CancelFor/PumpCompletions/Shutdown are main-thread
    // only. Workers touch only their own pending record (stable std::deque storage that
    // must outlive the JobCounter -- call Shutdown() before JobSystem::Shutdown()).
    class AsyncAssetLoadService
    {
    public:
        struct SkyboxPayload
        {
            std::vector<float> equirectPixels;
            unsigned int width = 0;
            unsigned int height = 0;
            IblSystem::GeneratedIblData ibl;
            bool succeeded = false;
        };
        // Applied on the main thread from PumpCompletions once the worker finishes.
        using SkyboxApplyFn = std::function<void(SkyboxPayload&&)>;

        // Queues a static-model load. Marks target Loading immediately; on completion
        // PumpCompletions adopts the meshes into target (or drops them if canceled).
        void RequestModelLoad(MeshComponent* target,
                              std::string assetPath,
                              MeshComponent::ModelFormat format,
                              float uniformScale);

        // Detaches a component from any in-flight load (call when its SObject is
        // destroyed); the worker result is dropped at completion.
        void CancelFor(const MeshComponent* target);

        // Queues an HDR-equirect skybox decode + IBL pregeneration.
        // resolvedPath must already be resolved to an absolute file path.
        void RequestSkyboxLoad(std::wstring resolvedPath, SkyboxApplyFn applyOnMainThread);

        // Main thread, once per frame. Applies finished loads. Returns true when at
        // least one model was adopted (caller should invalidate render objects).
        bool PumpCompletions();

        bool IsIdle() const;

        // Blocks on outstanding worker jobs. Call before JobSystem::Shutdown().
        void Shutdown();

    private:
        enum class RecordState : uint8_t { Pending, Running, Done, Failed };

        struct ModelRecord
        {
            MeshComponent* target = nullptr; // null once canceled
            std::string assetPath;
            MeshComponent::ModelFormat format{};
            float uniformScale = 1.0f;
            std::vector<MeshComponent::StaticMeshSource> meshes;
            std::atomic<RecordState> state{RecordState::Pending};
            JobCounter counter;
            bool applied = false;
        };

        struct SkyboxRecord
        {
            std::wstring resolvedPath;
            SkyboxPayload payload;
            SkyboxApplyFn apply;
            std::atomic<RecordState> state{RecordState::Pending};
            JobCounter counter;
            bool applied = false;
        };

        static void RunModelLoadJob(void* userdata);
        static void RunSkyboxLoadJob(void* userdata);

        std::deque<ModelRecord> m_modelLoads;
        std::deque<SkyboxRecord> m_skyboxLoads;
    };
}
