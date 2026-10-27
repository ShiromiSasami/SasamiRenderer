#include "AppFramework/Editor/ContentBrowserPanel.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Editor/EditorModelSpawner.h"
#include "AppFramework/Object/StaticModel.h"

#include "imgui.h"

#include <cstdio>

namespace SasamiEngine::App
{
    namespace
    {
        constexpr float SPAWN_DISTANCE = 3.0f;

        const char* FormatLabel(MeshComponent::ModelFormat format)
        {
            switch (format)
            {
            case MeshComponent::ModelFormat::Fbx: return "FBX";
            case MeshComponent::ModelFormat::Gltf: return "glTF";
            default: return "OBJ";
            }
        }

        std::string FileName(const std::string& relPath)
        {
            const size_t slash = relPath.rfind('/');
            return slash == std::string::npos ? relPath : relPath.substr(slash + 1);
        }
    }

    std::string ContentBrowserPanel::AddToScene(ApplicationCore& app, EditorSelection& selection,
                                                const std::string& relPath)
    {
        if (!m_catalog.HasScanned())
        {
            m_catalog.Scan();
        }
        const ContentAssetEntry* entry = m_catalog.Find(relPath);
        if (entry == nullptr)
        {
            return "ERR not a model under Assets/: " + relPath;
        }

        float position[3];
        EditorModelSpawner::PointInFrontOfCamera(app, SPAWN_DISTANCE, position);
        std::string error;
        StaticModel* model = EditorModelSpawner::Spawn(app, entry->relPath, entry->format, position, error);
        if (model == nullptr)
        {
            return "ERR " + error;
        }
        selection.Select(EditorObjectKind::StaticModel, model, app.GetSceneGeneration());

        char text[512];
        std::snprintf(text, sizeof(text), "OK added=%s index=%zu at=%.2f,%.2f,%.2f", relPath.c_str(),
                      app.GetStaticModels().size() - 1, position[0], position[1], position[2]);
        return text;
    }

    void ContentBrowserPanel::DrawFolder(ApplicationCore& app, EditorSelection& selection, const ContentFolder& folder)
    {
        for (const ContentFolder& child : folder.folders)
        {
            if (ImGui::TreeNodeEx(child.name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth))
            {
                DrawFolder(app, selection, child);
                ImGui::TreePop();
            }
        }
        for (const ContentAssetEntry& entry : folder.files)
        {
            ImGui::PushID(entry.relPath.c_str());
            ImGui::TreeNodeEx("##file", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen
                | ImGuiTreeNodeFlags_SpanAvailWidth, "%s  [%s, %.1f MB]", FileName(entry.relPath).c_str(),
                FormatLabel(entry.format), static_cast<double>(entry.sizeBytes) / (1024.0 * 1024.0));
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_status = AddToScene(app, selection, entry.relPath);
            }
            ImGui::PopID();
        }
    }

    void ContentBrowserPanel::Draw(ApplicationCore& app, EditorSelection& selection, bool* open)
    {
        if (!m_catalog.HasScanned())
        {
            m_catalog.Scan();
        }

        ImGui::SetNextWindowPos(ImVec2(8.0f, 468.0f), ImGuiCond_FirstUseEver);  // below the Hierarchy
        ImGui::SetNextWindowSize(ImVec2(360.0f, 200.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(WINDOW_NAME, open))
        {
            if (ImGui::Button("Refresh"))
            {
                m_catalog.Scan();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%zu models under Assets/ - double-click to add", m_catalog.Entries().size());
            if (!m_status.empty())
            {
                ImGui::TextWrapped("%s", m_status.c_str());
            }
            ImGui::Separator();
            if (ImGui::BeginChild("##tree"))
            {
                DrawFolder(app, selection, m_catalog.Root());
            }
            ImGui::EndChild();
        }
        ImGui::End();
    }
}
