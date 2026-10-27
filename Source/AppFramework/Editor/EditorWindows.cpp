#include "AppFramework/Editor/EditorWindows.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Editor/EditorPicking.h"
#include "AppFramework/Object/Camera.h"
#include "AppFramework/UI/ImGuiCoordinator.h"
#include "AppFramework/UI/UIPanelRegistry.h"

#include "imgui.h"

namespace SasamiEngine::App
{
    namespace
    {
        // Squared pixels the mouse may move between press and release and still count as a click.
        constexpr float CLICK_DRAG_LIMIT_SQ = 16.0f;
    }

    void EditorWindows::SetVisible(bool visible)
    {
        m_visible = visible;
        if (visible)
        {
            m_showHierarchy = true;
            m_showInspector = true;
            m_showContentBrowser = true;
        }
    }

    void EditorWindows::RequestLayoutReset()
    {
        m_showHierarchy = true;
        m_showInspector = true;
        m_showContentBrowser = true;
        m_dockLayout.RequestReset();
    }

    EditorObjectKind EditorWindows::PickAt(ApplicationCore& app, float x, float y)
    {
        Camera* camera = app.GetMainCamera();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        if (camera == nullptr || viewport == nullptr || viewport->Size.x <= 0.0f || viewport->Size.y <= 0.0f)
        {
            return EditorObjectKind::None;
        }
        const Render::RenderCameraProxy proxy = camera->BuildRenderCameraProxy(viewport->Size.x, viewport->Size.y);
        const EditorPickResult hit = PickEditorObject(app, proxy, x, y, viewport->Size.x, viewport->Size.y);
        if (hit.kind == EditorObjectKind::None)
        {
            m_selection.Clear();
        }
        else
        {
            m_selection.Select(hit.kind, hit.object, app.GetSceneGeneration());
        }
        return hit.kind;
    }

    void EditorWindows::Draw(ApplicationCore& app)
    {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_E))
        {
            SetVisible(!IsVisible());
        }

        SceneHierarchyPanel::ValidateSelection(app, m_selection);
        const bool editing = IsVisible() && !m_playSession.IsPlaying();
        if (editing)
        {
            // Outline first, so the gizmo is drawn over it (both go to the background draw list).
            m_selectionOverlay.Draw(app, m_selection);
        }
        m_gizmo.Draw(app, m_selection, editing);

        if (editing)
        {
            const ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                m_pressOnGizmo = m_gizmo.ConsumedClick();
            }
            if (!io.WantCaptureMouse && !m_pressOnGizmo && ImGui::IsMouseReleased(ImGuiMouseButton_Left)
                && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < CLICK_DRAG_LIMIT_SQ)
            {
                const ImGuiViewport* viewport = ImGui::GetMainViewport();
                PickAt(app, io.MousePos.x - viewport->Pos.x, io.MousePos.y - viewport->Pos.y);
                SceneHierarchyPanel::ValidateSelection(app, m_selection);
            }
        }
        if (!IsVisible())
        {
            return;
        }
        bool resetLayout = false;
        m_mainMenu.Draw(app, ImGuiCoordinator::Instance().Panels(), m_showHierarchy, m_showInspector, m_showContentBrowser,
                        resetLayout);
        if (resetLayout)
        {
            RequestLayoutReset();
        }
        const ImGuiID dockSpaceId = ImGuiCoordinator::Instance().GetDockSpaceId();
        std::vector<std::string> appPanels;
        for (const UI::PanelInfo& panel : ImGuiCoordinator::Instance().Panels().List())
        {
            appPanels.push_back(panel.name);
        }
        m_dockLayout.Apply(dockSpaceId, appPanels);
        m_playToolbar.Draw(app, m_playSession, m_gizmo, dockSpaceId);
        const bool playing = m_playSession.IsPlaying();
        ImGui::BeginDisabled(playing);
        if (m_showHierarchy)
        {
            m_hierarchy.Draw(app, m_selection, &m_showHierarchy);
        }
        if (m_showInspector)
        {
            m_inspector.Draw(app, m_selection, &m_showInspector);
        }
        if (m_showContentBrowser)
        {
            m_contentBrowser.Draw(app, m_selection, &m_showContentBrowser);
        }
        ImGui::EndDisabled();
    }
}
