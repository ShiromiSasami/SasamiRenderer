#pragma once

namespace SasamiRenderer
{
    class JobCounter;

    using JobFunction = void(*)(void* userdata);

    // Plain-old-data job descriptor. No heap allocation, no captures --
    // callers stash any needed state behind userdata (caller-owned storage
    // that must outlive the corresponding JobCounter::Wait()).
    struct Job
    {
        JobFunction function = nullptr;
        void* userdata = nullptr;
        JobCounter* counter = nullptr; // decremented on completion; may be null (fire-and-forget)
    };
}
