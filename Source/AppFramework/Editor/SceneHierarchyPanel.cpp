#include "AppFramework/Editor/SceneHierarchyPanel.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Object/Camera.h"
#include "AppFramework/Object/PointLight.h"
#include "AppFramework/Object/SObject.h"
#include "AppFramework/Object/SkinnedModel.h"
#include "AppFramework/Object/SpotLight.h"
#include "AppFramework/Object/StaticModel.h"
#include "imgui.h"

#include <algorithm>
#include <string>
#include <vector>

namespace SasamiEngine::App
{
    namespace
    {
        template <typename T>
        bool ContainsObject(const std::vector<T*>& list, const SObject* object)
        {
            return std::any_of(list.begin(), list.end(),
                [object](T* item) { return static_cast<SObject*>(item) == object; });
        }

        std::string FileNameOf(const std::string& path)
        {
            if (path.empty())
            {
                return "(procedural)";
            }
            const size_t pos = path.find_last_of("/\\");
            return pos == std::string::npos ? path : path.substr(pos + 1);
        }

        void DrawEntry(
            EditorSelection& selection,
            ApplicationCore& app,
            EditorObjectKind kind,
            SObject* object,
            size_t index,
            const std::string& label)
        {
            const bool isSelected = kind == EditorObjectKind::DirectionalLight
                ? selection.kind == kind
                : selection.object == object && selection.kind == kind;
            ImGui::PushID(static_cast<int>(static_cast<int>(kind) * 100000 + static_cast<int>(index)));
            if (ImGui::Selectable(label.c_str(), isSelected))
            {
                selection.Select(kind, object, app.GetSceneGeneration());
            }
            ImGui::PopID();
        }
    }

    void SceneHierarchyPanel::ValidateSelection(ApplicationCore& app, EditorSelection& selection)
    {
        if (selection.IsEmpty() || selection.kind == EditorObjectKind::DirectionalLight)
        {
            return;
        }
        if (selection.sceneGeneration != app.GetSceneGeneration())
        {
            selection.Clear();
            return;
        }

        bool listed = false;
        switch (selection.kind)
        {
        case EditorObjectKind::PointLight:
            listed = ContainsObject(app.GetPointLightObjects(), selection.object);
            break;
        case EditorObjectKind::SpotLight:
            listed = ContainsObject(app.GetSpotLightObjects(), selection.object);
            break;
        case EditorObjectKind::StaticModel:
            listed = ContainsObject(app.GetStaticModels(), selection.object);
            break;
        case EditorObjectKind::SkinnedModel:
            listed = ContainsObject(app.GetSkinnedModels(), selection.object);
            break;
        case EditorObjectKind::Camera:
            listed = ContainsObject(app.GetCameraObjects(), selection.object);
            break;
        default:
            break;
        }
        if (!listed)
        {
            selection.Clear();
        }
    }

    void SceneHierarchyPanel::Draw(ApplicationCore& app, EditorSelection& selection, bool* open)
    {
        ValidateSelection(app, selection);

        ImGui::SetNextWindowPos(ImVec2(8, 40), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(260, 420), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(WINDOW_NAME, open))
        {
            ImGui::End();
            return;
        }

        const std::vector<PointLight*> pointLights = app.GetPointLightObjects();
        const std::vector<SpotLight*> spotLights = app.GetSpotLightObjects();
        const std::vector<StaticModel*> staticModels = app.GetStaticModels();
        const std::vector<SkinnedModel*> skinnedModels = app.GetSkinnedModels();
        const std::vector<Camera*> cameras = app.GetCameraObjects();

        const std::string lightsLabel = "Lights (" + std::to_string(1 + pointLights.size() + spotLights.size()) + ")###Lights";
        if (ImGui::TreeNodeEx(lightsLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawEntry(selection, app, EditorObjectKind::DirectionalLight, nullptr, 0, "Directional");
            for (size_t i = 0; i < pointLights.size(); ++i)
            {
                DrawEntry(selection, app, EditorObjectKind::PointLight, pointLights[i], i, "Point " + std::to_string(i));
            }
            for (size_t i = 0; i < spotLights.size(); ++i)
            {
                DrawEntry(selection, app, EditorObjectKind::SpotLight, spotLights[i], i, "Spot " + std::to_string(i));
            }
            ImGui::TreePop();
        }

        const std::string modelsLabel = "Models (" + std::to_string(staticModels.size() + skinnedModels.size()) + ")###Models";
        if (ImGui::TreeNodeEx(modelsLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (size_t i = 0; i < staticModels.size(); ++i)
            {
                DrawEntry(selection, app, EditorObjectKind::StaticModel, staticModels[i], i,
                    "Static " + std::to_string(i) + ": " + FileNameOf(staticModels[i]->GetModelPath()));
            }
            for (size_t i = 0; i < skinnedModels.size(); ++i)
            {
                DrawEntry(selection, app, EditorObjectKind::SkinnedModel, skinnedModels[i], i,
                    "Skinned " + std::to_string(i) + ": " + FileNameOf(skinnedModels[i]->SkinnedMeshComponentRef().GetAssetPath()));
            }
            ImGui::TreePop();
        }

        const std::string camerasLabel = "Cameras (" + std::to_string(cameras.size()) + ")###Cameras";
        if (ImGui::TreeNodeEx(camerasLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            const Camera* mainCamera = app.GetMainCamera();
            for (size_t i = 0; i < cameras.size(); ++i)
            {
                std::string label = "Camera " + std::to_string(i);
                if (cameras[i] == mainCamera)
                {
                    label += " (main)";
                }
                DrawEntry(selection, app, EditorObjectKind::Camera, cameras[i], i, label);
            }
            ImGui::TreePop();
        }

        ImGui::Separator();
        ImGui::TextDisabled("Selected: %s", EditorObjectKindName(selection.kind));
        ImGui::End();
    }
}
