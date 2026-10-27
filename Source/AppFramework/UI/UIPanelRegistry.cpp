#include "UIPanelRegistry.h"

#include "imgui.h"

namespace SasamiEngine::App::UI
{
    void UIPanelRegistry::AddWindow(const std::string& name, const std::string& category,
                                    std::function<void()> drawFn)
    {
        AddTab(name, category, name, std::move(drawFn));
    }

    void UIPanelRegistry::AddTab(const std::string& windowName, const std::string& category,
                                 const std::string& tabLabel, std::function<void()> drawFn)
    {
        Panel* panel = Find(windowName);
        if (panel == nullptr)
        {
            m_panels.push_back(Panel{ windowName, category, TabbedPanel(windowName + "Tabs"), 0, true });
            panel = &m_panels.back();
        }
        panel->tabs.AddTab(tabLabel, std::move(drawFn));
        ++panel->tabCount;
    }

    void UIPanelRegistry::Draw()
    {
        for (const Panel& panel : m_panels)
        {
            if (!panel.open)
            {
                continue;
            }
            if (ImGui::Begin(panel.name.c_str()))
            {
                panel.tabs.Draw();
            }
            ImGui::End();
        }
    }

    bool UIPanelRegistry::SetOpen(const std::string& name, bool open)
    {
        Panel* panel = Find(name);
        if (panel == nullptr)
        {
            return false;
        }
        panel->open = open;
        return true;
    }

    bool UIPanelRegistry::IsOpen(const std::string& name) const
    {
        const Panel* panel = Find(name);
        return panel != nullptr && panel->open;
    }

    std::vector<PanelInfo> UIPanelRegistry::List() const
    {
        std::vector<PanelInfo> infos;
        infos.reserve(m_panels.size());
        for (const Panel& panel : m_panels)
        {
            infos.push_back(PanelInfo{ panel.name, panel.category, panel.tabCount, panel.open });
        }
        return infos;
    }

    UIPanelRegistry::Panel* UIPanelRegistry::Find(const std::string& name)
    {
        for (Panel& panel : m_panels)
        {
            if (panel.name == name)
            {
                return &panel;
            }
        }
        return nullptr;
    }

    const UIPanelRegistry::Panel* UIPanelRegistry::Find(const std::string& name) const
    {
        return const_cast<UIPanelRegistry*>(this)->Find(name);
    }

    int RunPanelRegistrySelfTest(int& checks)
    {
        int failures = 0;
        auto expect = [&](bool condition) {
            ++checks;
            if (!condition)
            {
                ++failures;
            }
        };

        UIPanelRegistry registry;
        registry.AddWindow("Camera", "App", [] {});
        registry.AddTab("Fluid", "Fluid", "Water", [] {});
        registry.AddTab("Fluid", "Ignored", "Smoke", [] {});

        const std::vector<PanelInfo> infos = registry.List();
        expect(infos.size() == 2);
        expect(infos.size() == 2 && infos[0].name == "Camera" && infos[0].tabCount == 1);
        expect(infos.size() == 2 && infos[1].name == "Fluid" && infos[1].tabCount == 2);
        expect(infos.size() == 2 && infos[1].category == "Fluid");
        expect(registry.IsOpen("Fluid"));
        expect(registry.SetOpen("Fluid", false) && !registry.IsOpen("Fluid") && registry.IsOpen("Camera"));
        expect(!registry.SetOpen("Missing", true) && !registry.IsOpen("Missing"));
        return failures;
    }
}
