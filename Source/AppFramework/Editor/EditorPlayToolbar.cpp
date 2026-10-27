#include "AppFramework/Editor/EditorPlayToolbar.h"

#include "AppFramework/Editor/EditorGizmo.h"
#include "AppFramework/Editor/EditorPlaySession.h"

#include "imgui_internal.h"

namespace SasamiEngine::App
{
    namespace
    {
        constexpr float TOOLBAR_TOP_MARGIN = 6.0f;

        // A toggle-style button: highlighted while `active`. Returns true when clicked.
        bool ModeButton(const char* label, bool active)
        {
            if (active)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            const bool clicked = ImGui::Button(label);
            if (active)
            {
                ImGui::PopStyleColor();
            }
            return clicked;
        }

        void DrawGizmoButtons(EditorGizmo& gizmo)
        {
            struct OperationButton
            {
                const char* label;
                EditorGizmoOperation operation;
            };
            constexpr OperationButton OPERATIONS[] = {
                { "T", EditorGizmoOperation::Translate },
                { "R", EditorGizmoOperation::Rotate },
                { "S", EditorGizmoOperation::Scale },
            };
            for (const OperationButton& button : OPERATIONS)
            {
                ImGui::SameLine();
                if (ModeButton(button.label, gizmo.Operation() == button.operation))
                {
                    gizmo.SetOperation(button.operation);
                }
                ImGui::SetItemTooltip("%s (Space cycles)", EditorGizmoOperationName(button.operation));
            }
            ImGui::SameLine();
            const bool local = gizmo.Space() == EditorGizmoSpace::Local;
            if (ImGui::Button(local ? "Local" : "World"))
            {
                gizmo.SetSpace(local ? EditorGizmoSpace::World : EditorGizmoSpace::Local);
            }
            ImGui::SetItemTooltip("Gizmo space");
        }
    }

    void EditorPlayToolbar::Draw(ApplicationCore& app, EditorPlaySession& playSession, EditorGizmo& gizmo,
                                 ImGuiID dockSpaceId)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 areaPos = viewport->WorkPos;
        ImVec2 areaSize = viewport->WorkSize;
        if (dockSpaceId != 0)
        {
            if (const ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockSpaceId))
            {
                areaPos = central->Pos;
                areaSize = central->Size;
            }
        }

        ImGui::SetNextWindowPos(ImVec2(areaPos.x + areaSize.x * 0.5f, areaPos.y + TOOLBAR_TOP_MARGIN), ImGuiCond_Always,
                                ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.75f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove
                                     | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
                                     | ImGuiWindowFlags_NoFocusOnAppearing;
        if (ImGui::Begin("##EditorPlayToolbar", nullptr, flags))
        {
            if (playSession.IsPlaying())
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.30f, 0.18f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.40f, 0.25f, 1.0f));
                const bool stop = ImGui::Button("Stop");
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "Playing");
                if (stop)
                {
                    playSession.Stop(app);
                }
            }
            else
            {
                if (ImGui::Button("Play"))
                {
                    playSession.Play(app);
                }
                DrawGizmoButtons(gizmo);
            }
        }
        ImGui::End();
    }
}
