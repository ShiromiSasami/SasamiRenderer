#include "AppFramework/Editor/EditorSceneSnapshot.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Object/Camera.h"
#include "AppFramework/Object/PointLight.h"
#include "AppFramework/Object/SkinnedModel.h"
#include "AppFramework/Object/SpotLight.h"
#include "AppFramework/Object/StaticModel.h"

#include <algorithm>

namespace SasamiEngine::App
{
    namespace
    {
        template <typename TObject, typename TState>
        const TState* FindState(const std::vector<TState>& states, const TObject* object)
        {
            for (const TState& state : states)
            {
                if (state.object == object)
                {
                    return &state;
                }
            }
            return nullptr;
        }

        // Counts captured states whose object is not in `current` (deleted while playing).
        template <typename TObject, typename TState>
        uint32_t CountMissing(const std::vector<TState>& states, const std::vector<TObject*>& current)
        {
            uint32_t missing = 0;
            for (const TState& state : states)
            {
                if (std::find(current.begin(), current.end(), state.object) == current.end())
                {
                    ++missing;
                }
            }
            return missing;
        }

        void CopyMatrix(std::array<float, 16>& out, const float* matrix)
        {
            std::copy(matrix, matrix + 16, out.begin());
        }
    }

    void EditorSceneSnapshot::Clear()
    {
        m_captured = false;
        m_staticModels.clear();
        m_skinnedModels.clear();
        m_pointLights.clear();
        m_spotLights.clear();
        m_cameras.clear();
    }

    void EditorSceneSnapshot::Capture(const ApplicationCore& app)
    {
        Clear();
        for (StaticModel* model : app.GetStaticModels())
        {
            ModelState state;
            state.object = model;
            CopyMatrix(state.matrix, model->MeshComponentRef().GetModelMatrix());
            m_staticModels.push_back(state);
        }
        for (SkinnedModel* model : app.GetSkinnedModels())
        {
            ModelState state;
            state.object = model;
            CopyMatrix(state.matrix, model->SkinnedMeshComponentRef().GetModelMatrix());
            m_skinnedModels.push_back(state);
        }
        for (PointLight* light : app.GetPointLightObjects())
        {
            LightState state;
            state.object = light;
            state.position = light->Transform().position;
            state.rotation = light->Transform().rotation;
            std::copy(light->ColorData(), light->ColorData() + 4, state.color.begin());
            state.intensity = light->Intensity();
            state.range = light->Range();
            m_pointLights.push_back(state);
        }
        for (SpotLight* light : app.GetSpotLightObjects())
        {
            LightState state;
            state.object = light;
            state.position = light->Transform().position;
            state.rotation = light->Transform().rotation;
            std::copy(light->ColorData(), light->ColorData() + 4, state.color.begin());
            state.intensity = light->Intensity();
            state.range = light->Range();
            state.innerAngle = light->InnerAngle();
            state.outerAngle = light->OuterAngle();
            m_spotLights.push_back(state);
        }
        for (Camera* camera : app.GetCameraObjects())
        {
            CameraState state;
            state.object = camera;
            state.target = camera->Transform().position;
            state.yaw = camera->Yaw();
            state.pitch = camera->Pitch();
            state.distance = camera->Distance();
            state.nearClip = camera->NearClip();
            state.farClip = camera->FarClip();
            state.moveSpeed = camera->MoveSpeed();
            m_cameras.push_back(state);
        }
        m_directionalLight = app.GetDirectionalLight();
        m_captured = true;
    }

    EditorSceneRestoreReport EditorSceneSnapshot::Restore(ApplicationCore& app) const
    {
        EditorSceneRestoreReport report;
        if (!m_captured)
        {
            return report;
        }

        const std::vector<StaticModel*> staticModels = app.GetStaticModels();
        for (StaticModel* model : staticModels)
        {
            const ModelState* state = FindState(m_staticModels, model);
            if (state == nullptr)
            {
                ++report.added;
                continue;
            }
            model->MeshComponentRef().SetModelMatrix(state->matrix.data());
            ++report.restored;
        }
        report.missing += CountMissing(m_staticModels, staticModels);

        const std::vector<SkinnedModel*> skinnedModels = app.GetSkinnedModels();
        for (SkinnedModel* model : skinnedModels)
        {
            const ModelState* state = FindState(m_skinnedModels, model);
            if (state == nullptr)
            {
                ++report.added;
                continue;
            }
            model->SkinnedMeshComponentRef().SetModelMatrix(state->matrix.data());
            ++report.restored;
        }
        report.missing += CountMissing(m_skinnedModels, skinnedModels);

        const std::vector<PointLight*> pointLights = app.GetPointLightObjects();
        for (PointLight* light : pointLights)
        {
            const LightState* state = FindState(m_pointLights, light);
            if (state == nullptr)
            {
                ++report.added;
                continue;
            }
            light->Transform().position = state->position;
            light->Transform().rotation = state->rotation;
            std::copy(state->color.begin(), state->color.end(), light->ColorData());
            light->SetIntensity(state->intensity);
            light->SetRange(state->range);
            ++report.restored;
        }
        report.missing += CountMissing(m_pointLights, pointLights);

        const std::vector<SpotLight*> spotLights = app.GetSpotLightObjects();
        for (SpotLight* light : spotLights)
        {
            const LightState* state = FindState(m_spotLights, light);
            if (state == nullptr)
            {
                ++report.added;
                continue;
            }
            light->Transform().position = state->position;
            light->Transform().rotation = state->rotation;
            std::copy(state->color.begin(), state->color.end(), light->ColorData());
            light->SetIntensity(state->intensity);
            light->SetRange(state->range);
            light->SetInnerAngle(state->innerAngle);
            light->SetOuterAngle(state->outerAngle);
            ++report.restored;
        }
        report.missing += CountMissing(m_spotLights, spotLights);

        const std::vector<Camera*> cameras = app.GetCameraObjects();
        for (Camera* camera : cameras)
        {
            const CameraState* state = FindState(m_cameras, camera);
            if (state == nullptr)
            {
                ++report.added;
                continue;
            }
            camera->Transform().position = state->target;
            camera->SetYawPitch(state->yaw, state->pitch);
            camera->SetDistance(state->distance);
            camera->SetClipPlanes(state->nearClip, state->farClip);
            camera->SetMoveSpeed(state->moveSpeed);
            ++report.restored;
        }
        report.missing += CountMissing(m_cameras, cameras);

        app.SetDirectionalLight(m_directionalLight);
        app.InvalidateRenderObjects();
        return report;
    }
}
