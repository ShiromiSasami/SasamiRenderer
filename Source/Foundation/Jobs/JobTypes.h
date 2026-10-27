#pragma once

namespace SasamiEngine::Foundation
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

// Lets the other layers name Foundation types unqualified. The directive sits on SasamiEngine rather
// than on a layer, so anything nested under it -- SasamiEngine::Render, SasamiEngine::App -- finds these
// names by enclosing-namespace lookup.
//
// The SasamiRenderer -> SasamiEngine::<layer> namespace migration it was written for is finished; what
// is left is deciding whether the layers should qualify Foundation:: explicitly instead. That is not a
// mechanical change (roughly 76 files name these types unqualified), so the directive stays until then.
namespace SasamiEngine { using namespace Foundation; }
