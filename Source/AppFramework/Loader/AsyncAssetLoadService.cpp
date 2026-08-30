#include "Loader/AsyncAssetLoadService.h"

#include <windows.h>

#include "Foundation/Jobs/JobSystem.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Loader/AssetLoader.h"

namespace SasamiRenderer
{
    namespace
    {
        // Workers decode textures/HDR files via WIC, which requires COM on the calling
        // thread. JobSystem worker threads live for the process lifetime, so we never
        // call CoUninitialize -- the apartment stays valid until process exit.
        void EnsureThreadComInitialized()
        {
            thread_local bool comInitialized = false;
            if (comInitialized) {
                return;
            }

            const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (hr == S_OK || hr == S_FALSE || hr == RPC_E_CHANGED_MODE) {
                comInitialized = true;
            }
        }
    }

    void AsyncAssetLoadService::RequestModelLoad(MeshComponent* target,
                                                 std::string assetPath,
                                                 MeshComponent::ModelFormat format,
                                                 float uniformScale)
    {
        target->MarkLoadPending();

        ModelRecord& record = m_modelLoads.emplace_back();
        record.target = target;
        record.assetPath = std::move(assetPath);
        record.format = format;
        record.uniformScale = uniformScale;
        record.counter.Reset(1);

        JobSystem::Kick(&AsyncAssetLoadService::RunModelLoadJob, &record, &record.counter);
    }

    void AsyncAssetLoadService::CancelFor(const MeshComponent* target)
    {
        for (auto& record : m_modelLoads) {
            if (record.target == target) {
                record.target = nullptr;
            }
        }
    }

    void AsyncAssetLoadService::RequestSkyboxLoad(std::wstring resolvedPath, SkyboxApplyFn applyOnMainThread)
    {
        SkyboxRecord& record = m_skyboxLoads.emplace_back();
        record.resolvedPath = std::move(resolvedPath);
        record.apply = std::move(applyOnMainThread);
        record.counter.Reset(1);

        JobSystem::Kick(&AsyncAssetLoadService::RunSkyboxLoadJob, &record, &record.counter);
    }

    bool AsyncAssetLoadService::PumpCompletions()
    {
        bool anyAdopted = false;

        for (auto& record : m_modelLoads) {
            if (record.applied) {
                continue;
            }
            const RecordState state = record.state.load(std::memory_order_acquire);
            if (state != RecordState::Done && state != RecordState::Failed) {
                continue;
            }
            record.applied = true;

            if (state == RecordState::Failed) {
                if (record.target) {
                    record.target->MarkLoadFailed();
                    DebugLog(("AsyncAssetLoadService: model load failed for \"" + record.assetPath + "\".\n").c_str());
                }
            } else {
                if (record.target) {
                    record.target->AdoptLoadedMeshes(std::move(record.meshes), record.assetPath,
                                                      record.format, record.uniformScale);
                    anyAdopted = true;
                }
            }
        }

        for (auto& record : m_skyboxLoads) {
            if (record.applied) {
                continue;
            }
            if (record.state.load(std::memory_order_acquire) != RecordState::Done) {
                continue;
            }
            record.applied = true;

            if (record.apply) {
                record.apply(std::move(record.payload));
            }
        }

        // Records are intentionally NOT popped here: the worker publishes state=Done
        // BEFORE JobWorker decrements/notifies the record's JobCounter, so destroying a
        // record right after observing Done could free the counter mid-Decrement().
        // Applied records are inert (payloads moved out); they are reclaimed at Shutdown,
        // which waits on every counter first.
        return anyAdopted;
    }

    bool AsyncAssetLoadService::IsIdle() const
    {
        for (const auto& record : m_modelLoads) {
            if (!record.applied) {
                return false;
            }
        }
        for (const auto& record : m_skyboxLoads) {
            if (!record.applied) {
                return false;
            }
        }
        return true;
    }

    void AsyncAssetLoadService::Shutdown()
    {
        // Must run before JobSystem::Shutdown(): the deque storage and each record's
        // JobCounter must stay alive for as long as a worker might still be touching them.
        for (auto& record : m_modelLoads) {
            JobSystem::Wait(record.counter);
        }
        for (auto& record : m_skyboxLoads) {
            JobSystem::Wait(record.counter);
        }
    }

    void AsyncAssetLoadService::RunModelLoadJob(void* userdata)
    {
        ModelRecord* record = static_cast<ModelRecord*>(userdata);
        record->state.store(RecordState::Running, std::memory_order_release);

        EnsureThreadComInitialized();

        const bool ok = MeshComponent::LoadStaticMeshSources(record->assetPath, record->format,
                                                              record->uniformScale, record->meshes);
        record->state.store(ok ? RecordState::Done : RecordState::Failed, std::memory_order_release);
    }

    void AsyncAssetLoadService::RunSkyboxLoadJob(void* userdata)
    {
        SkyboxRecord* record = static_cast<SkyboxRecord*>(userdata);
        record->state.store(RecordState::Running, std::memory_order_release);

        EnsureThreadComInitialized();

        UINT width = 0;
        UINT height = 0;
        const bool decoded = AssetLoader::LoadRadianceHdr(record->resolvedPath, record->payload.equirectPixels,
                                                           width, height);
        record->payload.width = width;
        record->payload.height = height;
        record->payload.succeeded = decoded &&
            IblSystem::GenerateHdrIblData(record->payload.equirectPixels, width, height, record->payload.ibl);

        record->state.store(RecordState::Done, std::memory_order_release);
    }
}
