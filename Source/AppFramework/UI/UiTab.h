#pragma once

namespace SasamiRenderer::UI
{
    // RAII scope guard around ImGui::BeginTabItem/EndTabItem (a single
    // "tab" of content). Must be constructed while a tab bar is active
    // (either a UI::TabBar or a raw ImGui::BeginTabBar). EndTabItem() is
    // only invoked when BeginTabItem() reported the tab as selected,
    // matching ImGui's own conditional-close contract.
    //
    //   if (UI::Tab tab("Camera")) {
    //       UI::Text("...");
    //   }
    //
    class Tab
    {
    public:
        explicit Tab(const char* label);
        ~Tab();

        Tab(const Tab&) = delete;
        Tab& operator=(const Tab&) = delete;
        Tab(Tab&&) = delete;
        Tab& operator=(Tab&&) = delete;

        // True while this tab is the selected one and its content should be drawn.
        explicit operator bool() const { return m_selected; }

    private:
        bool m_selected = false;
    };
}
