#pragma once

namespace SasamiRenderer::UI
{
    // RAII scope guard around ImGui::BeginTabBar/EndTabBar (the "tab bar"
    // container that holds one or more Tab parts). EndTabBar() is only
    // invoked when BeginTabBar() reported success, matching ImGui's own
    // conditional-close contract, so the caller cannot mismatch the pair.
    //
    // Most callers should prefer TabbedPanel, which creates a TabBar
    // automatically once a second tab is registered. Use TabBar directly
    // only when hand-rolling a tab set (e.g. to interleave with existing
    // raw ImGui tab items during incremental migration).
    class TabBar
    {
    public:
        explicit TabBar(const char* id);
        ~TabBar();

        TabBar(const TabBar&) = delete;
        TabBar& operator=(const TabBar&) = delete;
        TabBar(TabBar&&) = delete;
        TabBar& operator=(TabBar&&) = delete;

        // True while the tab bar is active and Tab parts may be added to it.
        explicit operator bool() const { return m_open; }

    private:
        bool m_open = false;
    };
}
