#pragma once

#include "Renderer/Voxel/ProjectedWindField.h"
#include "Renderer/Voxel/StreamedVoxelWindow.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    // A streamed window together with the wind projected on it. field references window->sdf, so
    // both live on the heap and move together.
    struct ProjectedWindowField
    {
        std::unique_ptr<StreamedVoxelWindow> window;
        std::unique_ptr<ProjectedWindField> field;
        int32_t centreChunk[3] = { 0, 0, 0 };
        uint64_t warmCells = 0;     // pressure cells carried over from the previous window
        double projectMs = 0.0;
    };

    // Projects settings.wind on the window (floor wall only when the window touches the bottom
    // chunk row, as voxel.stream window) and takes ownership of it.
    ProjectedWindowField BuildProjectedWindowField(std::unique_ptr<StreamedVoxelWindow> window, const int32_t centreChunk[3],
                                                   const ProjectedWindSettings& settings,
                                                   const std::vector<float>* warmStartPressure, uint64_t warmCells);

    // One background wind projection at a time on the JobSystem (6b stage 3b). The job owns its
    // inputs, so the main thread keeps using the previous field until TryTake hands over the new one.
    // The projection itself may call ParallelFor (micSlabs > 1, PCG vectors), which blocks the worker
    // running the job while the rest of the pool does the slices. Main thread only.
    class AsyncWindowProjection
    {
    public:
        AsyncWindowProjection();
        ~AsyncWindowProjection();
        AsyncWindowProjection(const AsyncWindowProjection&) = delete;
        AsyncWindowProjection& operator=(const AsyncWindowProjection&) = delete;

        bool IsBusy() const { return m_job != nullptr; }
        // Ignored (returns false) while a job is in flight.
        bool Kick(std::unique_ptr<StreamedVoxelWindow> window, const int32_t centreChunk[3], const ProjectedWindSettings& settings,
                  std::vector<float> warmStartPressure, uint64_t warmCells);
        // Moves the finished result into out; false while the job runs or when there is none.
        bool TryTake(ProjectedWindowField& out);
        // Blocks until the job (if any) finishes and drops its result.
        void Cancel();

    private:
        struct Job;
        static void RunJob(void* userdata);

        std::unique_ptr<Job> m_job;
    };
}

namespace SasamiEngine { using namespace Render; }
