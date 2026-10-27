#include "Renderer/Voxel/AsyncWindowProjection.h"

#include "Foundation/Jobs/JobCounter.h"
#include "Foundation/Jobs/JobSystem.h"

#include <chrono>

namespace SasamiEngine::Render::Voxel
{
    ProjectedWindowField BuildProjectedWindowField(std::unique_ptr<StreamedVoxelWindow> window, const int32_t centreChunk[3],
                                                   const ProjectedWindSettings& settings,
                                                   const std::vector<float>* warmStartPressure, uint64_t warmCells)
    {
        ProjectedWindowField result;
        ProjectedWindSettings windowSettings = settings;
        // Only the bottom chunk row holds the scene floor; elsewhere the lower face is open air.
        windowSettings.projection.solidFloor = window->chunkLo[1] == 0;
        const auto begin = std::chrono::steady_clock::now();
        result.field = std::make_unique<ProjectedWindField>(window->occupancy, window->sdf, window->desc, windowSettings,
                                                            warmStartPressure);
        result.projectMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        result.window = std::move(window);
        for (int a = 0; a < 3; ++a) {
            result.centreChunk[a] = centreChunk[a];
        }
        result.warmCells = warmCells;
        return result;
    }

    struct AsyncWindowProjection::Job
    {
        std::unique_ptr<StreamedVoxelWindow> window;
        int32_t centreChunk[3] = { 0, 0, 0 };
        ProjectedWindSettings settings;
        std::vector<float> warm;
        uint64_t warmCells = 0;
        ProjectedWindowField result;
        Foundation::JobCounter counter;
    };

    AsyncWindowProjection::AsyncWindowProjection() = default;

    AsyncWindowProjection::~AsyncWindowProjection()
    {
        Cancel();
    }

    bool AsyncWindowProjection::Kick(std::unique_ptr<StreamedVoxelWindow> window, const int32_t centreChunk[3],
                                     const ProjectedWindSettings& settings, std::vector<float> warmStartPressure,
                                     uint64_t warmCells)
    {
        if (m_job || !window) {
            return false;
        }
        m_job = std::make_unique<Job>();
        m_job->window = std::move(window);
        for (int a = 0; a < 3; ++a) {
            m_job->centreChunk[a] = centreChunk[a];
        }
        m_job->settings = settings;
        m_job->warm = std::move(warmStartPressure);
        m_job->warmCells = warmCells;
        m_job->counter.Reset(1);
        Foundation::JobSystem::Kick(&AsyncWindowProjection::RunJob, m_job.get(), &m_job->counter);
        return true;
    }

    void AsyncWindowProjection::RunJob(void* userdata)
    {
        Job& job = *static_cast<Job*>(userdata);
        job.result = BuildProjectedWindowField(std::move(job.window), job.centreChunk, job.settings,
                                               job.warm.empty() ? nullptr : &job.warm, job.warmCells);
    }

    bool AsyncWindowProjection::TryTake(ProjectedWindowField& out)
    {
        if (!m_job || m_job->counter.Get() != 0) {
            return false;
        }
        out = std::move(m_job->result);
        m_job.reset();
        return true;
    }

    void AsyncWindowProjection::Cancel()
    {
        if (m_job) {
            m_job->counter.Wait();
            m_job.reset();
        }
    }
}
