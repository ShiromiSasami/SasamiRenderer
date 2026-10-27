#include "AppFramework/Editor/EditorSelectionOverlay.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Debug/WindOverlayProjection.h"
#include "AppFramework/Editor/EditorPicking.h"
#include "AppFramework/Editor/EditorSelection.h"
#include "AppFramework/Object/Camera.h"
#include "AppFramework/Object/PointLight.h"
#include "AppFramework/Object/SpotLight.h"
#include "imgui.h"

namespace SasamiEngine::App
{
    namespace
    {
        constexpr ImU32 OUTLINE_COLOR = IM_COL32(255, 170, 40, 255);
        constexpr float OUTLINE_THICKNESS = 2.0f;
        constexpr float LIGHT_RADIUS = 12.0f;
        constexpr int BOX_EDGES[12][2] = {
            { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },
            { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
            { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
        };
    }

    void EditorSelectionOverlay::Draw(ApplicationCore& app, const EditorSelection& selection)
    {
        if (selection.IsEmpty())
        {
            return;
        }
        Camera* camera = app.GetMainCamera();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        if (camera == nullptr || viewport == nullptr || viewport->Size.x <= 0.0f || viewport->Size.y <= 0.0f)
        {
            return;
        }
        const Render::RenderCameraProxy proxy = camera->BuildRenderCameraProxy(viewport->Size.x, viewport->Size.y);
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();

        if (selection.kind == EditorObjectKind::PointLight || selection.kind == EditorObjectKind::SpotLight)
        {
            const Vector3& p = selection.kind == EditorObjectKind::PointLight
                ? static_cast<PointLight*>(selection.object)->Transform().position
                : static_cast<SpotLight*>(selection.object)->Transform().position;
            const float position[3] = { p.x, p.y, p.z };
            ImVec2 screen;
            if (Debug::ProjectToOverlay(proxy, viewport->Pos, viewport->Size, position, screen))
            {
                drawList->AddCircle(screen, LIGHT_RADIUS, OUTLINE_COLOR, 24, OUTLINE_THICKNESS);
            }
            return;
        }

        if (m_cachedRevision != selection.revision)
        {
            m_cachedRevision = selection.revision;
            const float* ignored = nullptr;
            m_hasBox = GetSelectionBox(selection, m_boxMin, m_boxMax, ignored);
        }
        const float* model = GetSelectionModelMatrix(selection);
        if (!m_hasBox || model == nullptr)
        {
            return;
        }

        float corners[8][3];
        TransformBoxCorners(m_boxMin, m_boxMax, model, corners);
        ImVec2 screen[8];
        bool visible[8];
        for (int i = 0; i < 8; ++i)
        {
            visible[i] = Debug::ProjectToOverlay(proxy, viewport->Pos, viewport->Size, corners[i], screen[i]);
        }
        // Edges with an end behind the near limit are skipped rather than clipped.
        for (const auto& edge : BOX_EDGES)
        {
            if (visible[edge[0]] && visible[edge[1]])
            {
                drawList->AddLine(screen[edge[0]], screen[edge[1]], OUTLINE_COLOR, OUTLINE_THICKNESS);
            }
        }
    }
}
