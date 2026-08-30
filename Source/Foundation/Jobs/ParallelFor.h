#pragma once

#include "Foundation/Jobs/JobSystem.h"
#include "Foundation/Jobs/JobTypes.h"

#include <cstdint>
#include <vector>

namespace SasamiRenderer
{
    // Splits [0, count) into one job per index and runs them via
    // JobSystem::KickAndWait, blocking the calling thread until all complete.
    // fn is called as fn(index) for each index in [0, count).
    template <typename Func>
    void ParallelFor(uint32_t count, Func&& fn)
    {
        if (count == 0) {
            return;
        }

        struct Context
        {
            Func* fn;
            uint32_t index;
        };

        std::vector<Context> contexts(count);
        std::vector<Job> jobs(count);

        for (uint32_t i = 0; i < count; ++i) {
            contexts[i] = Context{ &fn, i };
            jobs[i].function = [](void* userdata) {
                Context* ctx = static_cast<Context*>(userdata);
                (*ctx->fn)(ctx->index);
            };
            jobs[i].userdata = &contexts[i];
        }

        JobSystem::KickAndWait(jobs.data(), static_cast<uint32_t>(jobs.size()));
    }
}
