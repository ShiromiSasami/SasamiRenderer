#include "AppFramework/Editor/ObjectInspectorPanel.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Light/DirectionalLight.h"
#include "AppFramework/Object/Camera.h"
#include "AppFramework/Object/PointLight.h"
#include "AppFramework/Object/SkinnedModel.h"
#include "AppFramework/Object/SpotLight.h"
#include "AppFramework/Object/StaticModel.h"
#include "imgui.h"

#include <algorithm>

namespace SasamiEngine::App
{
    namespace
    {
        constexpr float DEG_TO_RAD = 0.01745329252f;
        constexpr float RAD_TO_DEG = 57.2957795131f;

        bool DragDegrees(const char* label, float& radians, float minDegrees, float maxDegrees, float speed = 0.5f)
        {
            float degrees = radians * RAD_TO_DEG;
            if (!ImGui::DragFloat(label, &degrees, speed, minDegrees, maxDegrees, "%.1f"))
            {
                return false;
            }
            radians = degrees * DEG_TO_RAD;
            return true;
        }

        bool DragPosition(const char* label, Vector3& position)
        {
            float values[3] = { position.x, position.y, position.z };
            if (!ImGui::DragFloat3(label, values, 0.05f))
            {
                return false;
            }
            position = Vector3(values[0], values[1], values[2]);
            return true;
        }

        bool DrawLightCommon(TransformComponent& transform, float* color, float& intensity, float& range)
        {
            bool changed = false;
            changed |= DragPosition("Position", transform.position);
            changed |= ImGui::ColorEdit3("Color", color);
            changed |= ImGui::DragFloat("Intensity", &intensity, 0.05f, 0.0f, 1000.0f);
            changed |= ImGui::DragFloat("Range", &range, 0.05f, 0.01f, 1000.0f);
            return changed;
        }
    }

    void ObjectInspectorPanel::Draw(ApplicationCore& app, const EditorSelection& selection, bool* open)
    {
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 328.0f, 40.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(320.0f, 420.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(WINDOW_NAME, open))
        {
            ImGui::Text("%s", EditorObjectKindName(selection.kind));
            ImGui::Separator();
            switch (selection.kind)
            {
            case EditorObjectKind::StaticModel:
            case EditorObjectKind::SkinnedModel:
                DrawModel(app, selection);
                break;
            case EditorObjectKind::PointLight:
                DrawPointLight(selection);
                break;
            case EditorObjectKind::SpotLight:
                DrawSpotLight(selection);
                break;
            case EditorObjectKind::DirectionalLight:
                DrawDirectionalLight(app);
                break;
            case EditorObjectKind::Camera:
                DrawCamera(selection);
                break;
            case EditorObjectKind::None:
            default:
                ImGui::TextDisabled("Select an object in WorldObjects.");
                break;
            }
        }
        ImGui::End();
    }

    void ObjectInspectorPanel::DrawModel(ApplicationCore& app, const EditorSelection& selection)
    {
        const bool isStatic = selection.kind == EditorObjectKind::StaticModel;
        StaticModel* staticModel = isStatic ? static_cast<StaticModel*>(selection.object) : nullptr;
        SkinnedModel* skinnedModel = isStatic ? nullptr : static_cast<SkinnedModel*>(selection.object);
        if (!staticModel && !skinnedModel)
        {
            return;
        }

        const float* matrix = isStatic ? staticModel->MeshComponentRef().GetModelMatrix()
                                       : skinnedModel->SkinnedMeshComponentRef().GetModelMatrix();
        if (m_cachedRevision != selection.revision)
        {
            m_cachedTrs = DecomposeTrs(matrix);
            m_cachedRevision = selection.revision;
        }

        bool changed = false;
        changed |= DragPosition("Position", m_cachedTrs.translation);

        float rotation[3] = {
            m_cachedTrs.rotation.pitch * RAD_TO_DEG,
            m_cachedTrs.rotation.yaw * RAD_TO_DEG,
            m_cachedTrs.rotation.roll * RAD_TO_DEG };
        if (ImGui::DragFloat3("Rotation", rotation, 0.5f))
        {
            m_cachedTrs.rotation.pitch = rotation[0] * DEG_TO_RAD;
            m_cachedTrs.rotation.yaw = rotation[1] * DEG_TO_RAD;
            m_cachedTrs.rotation.roll = rotation[2] * DEG_TO_RAD;
            changed = true;
        }

        float scale[3] = { m_cachedTrs.scale.x, m_cachedTrs.scale.y, m_cachedTrs.scale.z };
        if (ImGui::DragFloat3("Scale", scale, 0.01f, 0.001f, 1000000.0f))
        {
            m_cachedTrs.scale = Vector3(scale[0], scale[1], scale[2]);
            changed = true;
        }

        if (ImGui::Button("Reset rotation/scale"))
        {
            m_cachedTrs.rotation = Rotation3{};
            m_cachedTrs.scale = Vector3(1.0f, 1.0f, 1.0f);
            changed = true;
        }

        if (changed)
        {
            float composed[16];
            ComposeTrs(m_cachedTrs, composed);
            if (isStatic)
            {
                staticModel->MeshComponentRef().SetModelMatrix(composed);
                app.InvalidateRenderObjects();
            }
            else
            {
                skinnedModel->SkinnedMeshComponentRef().SetModelMatrix(composed);
            }
        }

        const std::string& assetPath = isStatic ? staticModel->GetModelPath()
                                                : skinnedModel->SkinnedMeshComponentRef().GetAssetPath();
        ImGui::TextDisabled("%s", assetPath.c_str());
    }

    void ObjectInspectorPanel::DrawPointLight(const EditorSelection& selection)
    {
        PointLight* light = static_cast<PointLight*>(selection.object);
        if (!light)
        {
            return;
        }
        float intensity = light->Intensity();
        float range = light->Range();
        DrawLightCommon(light->Transform(), light->ColorData(), intensity, range);
        light->SetIntensity(intensity);
        light->SetRange(range);
    }

    void ObjectInspectorPanel::DrawSpotLight(const EditorSelection& selection)
    {
        SpotLight* light = static_cast<SpotLight*>(selection.object);
        if (!light)
        {
            return;
        }
        float intensity = light->Intensity();
        float range = light->Range();
        DrawLightCommon(light->Transform(), light->ColorData(), intensity, range);
        light->SetIntensity(intensity);
        light->SetRange(range);

        DragDegrees("Yaw", light->Transform().rotation.yaw, -360.0f, 360.0f);
        DragDegrees("Pitch", light->Transform().rotation.pitch, -89.9f, 89.9f);

        float inner = light->InnerAngle();
        float outer = light->OuterAngle();
        if (DragDegrees("Inner angle", inner, 0.0f, 89.9f, 0.1f))
        {
            outer = (std::max)(outer, inner);
        }
        if (DragDegrees("Outer angle", outer, 0.1f, 89.9f, 0.1f))
        {
            inner = (std::min)(inner, outer);
        }
        light->SetInnerAngle(inner);
        light->SetOuterAngle(outer);
    }

    void ObjectInspectorPanel::DrawDirectionalLight(ApplicationCore& app)
    {
        DirectionalLight light = app.GetDirectionalLight();
        bool changed = false;
        changed |= DragDegrees("Yaw", light.yaw, -360.0f, 360.0f);
        changed |= DragDegrees("Pitch", light.pitch, -89.9f, 89.9f);
        changed |= ImGui::ColorEdit3("Color", &light.color.r);
        changed |= ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f, 100.0f);
        changed |= ImGui::Checkbox("Contact shadows", &light.contactShadowEnabled);
        changed |= ImGui::DragFloat("Shadow distance", &light.shadowDistance, 0.5f, 1.0f, 2000.0f);
        if (changed)
        {
            app.SetDirectionalLight(light);
        }
    }

    void ObjectInspectorPanel::DrawCamera(const EditorSelection& selection)
    {
        Camera* camera = static_cast<Camera*>(selection.object);
        if (!camera)
        {
            return;
        }

        Vector3 target = camera->Transform().position;
        if (DragPosition("Target", target))
        {
            camera->SetTarget(target.x, target.y, target.z);
        }

        float yaw = camera->Yaw();
        float pitch = camera->Pitch();
        bool angleChanged = false;
        angleChanged |= DragDegrees("Yaw", yaw, -360.0f, 360.0f);
        angleChanged |= DragDegrees("Pitch", pitch, -89.9f, 89.9f);
        if (angleChanged)
        {
            camera->SetYawPitch(yaw, pitch);
        }

        float distance = camera->Distance();
        if (ImGui::DragFloat("Distance", &distance, 0.05f, 0.05f, 200.0f))
        {
            camera->SetDistance(distance);
        }

        float clip[2] = { camera->NearClip(), camera->FarClip() };
        if (ImGui::DragFloat2("Near/Far", clip, 0.05f, 0.001f, 100000.0f))
        {
            camera->SetClipPlanes(clip[0], clip[1]);
        }

        float moveSpeed = camera->MoveSpeed();
        if (ImGui::DragFloat("Move speed", &moveSpeed, 0.05f, 0.0f, 1000.0f))
        {
            camera->SetMoveSpeed(moveSpeed);
        }
    }
}
