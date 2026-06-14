#pragma once

namespace SasamiRenderer
{
    class CommandList;
}

namespace SasamiRenderer::Profiler
{
    void Initialize();
    void Shutdown();

    void BeginCpuEvent(const char* name);
    void EndCpuEvent(const char* name);
    void MarkCpuEvent(const char* name);

    void BeginGpuEvent(CommandList* cmdList, const char* name);
    void EndGpuEvent(CommandList* cmdList);
    void MarkGpuEvent(CommandList* cmdList, const char* name);

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
        ScopedGpuEvent(CommandList* cmdList, const char* name)
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
        CommandList* m_cmdList = nullptr;
    };
}
