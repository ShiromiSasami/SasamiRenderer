#pragma once

// CommandList belongs to the Renderer layer, one level above this one. Splitting Foundation into
// its own namespace turned that inversion into a compile error, which is exactly what the split is
// for -- the dependency was invisible while every layer shared one namespace. Naming it explicitly
// keeps the build green and makes the violation greppable (and it has to be re-pointed by hand
// every time the Renderer layer moves, as it did when Renderer became SasamiEngine::Render); the real
// fix is to move the GPU half of
// the profiler up into Renderer, or to hand it an opaque handle. See TODO.md.
namespace SasamiEngine::Render
{
    class CommandList;
}

namespace SasamiEngine::Foundation::Profiler
{
    void Initialize();
    void Shutdown();

    void BeginCpuEvent(const char* name);
    void EndCpuEvent(const char* name);

    void BeginGpuEvent(SasamiEngine::Render::CommandList* cmdList, const char* name);
    void EndGpuEvent(SasamiEngine::Render::CommandList* cmdList);

    class ScopedCpuEvent
    {
    public:
        explicit ScopedCpuEvent(const char* name)
            : m_name(name)
        {
            BeginCpuEvent(m_name);
        }

        ~ScopedCpuEvent()
        {
            EndCpuEvent(m_name);
        }

        ScopedCpuEvent(const ScopedCpuEvent&) = delete;
        ScopedCpuEvent& operator=(const ScopedCpuEvent&) = delete;

    private:
        const char* m_name = nullptr;
    };

    class ScopedGpuEvent
    {
    public:
        ScopedGpuEvent(SasamiEngine::Render::CommandList* cmdList, const char* name)
            : m_cmdList(cmdList)
        {
            BeginGpuEvent(m_cmdList, name);
        }

        ~ScopedGpuEvent()
        {
            EndGpuEvent(m_cmdList);
        }

        ScopedGpuEvent(const ScopedGpuEvent&) = delete;
        ScopedGpuEvent& operator=(const ScopedGpuEvent&) = delete;

    private:
        SasamiEngine::Render::CommandList* m_cmdList = nullptr;
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
