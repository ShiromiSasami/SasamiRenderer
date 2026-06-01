// ApplicationObjectManagement.cpp
// Object lifecycle, ECS registration, camera management, and scene sync for ApplicationCore.
// Extracted from ApplicationCore.cpp to separate object/ECS concerns.
#include "ApplicationCore.h"
#include <windows.h>
#include <algorithm>
#include <utility>
#include <vector>

#include "Foundation/Tools/DebugOutput.h"
#include "Object/Camera.h"
#include "Object/PointLight.h"
#include "Object/SpotLight.h"
#include "Object/StaticModel.h"
#include "Renderer/Core/Renderer.h"

namespace SasamiRenderer
{
    void ApplicationCore::RegisterObjectInEcs(SObject* object)
    {
        if (!object) {
            return;
        }

        if (m_objectEntityMap.contains(object)) {
            return;
        }

        const EntityId entity = m_ecsRegistry.CreateEntity();
        if (entity == EcsRegistry::INVALID_ENTITY) {
            DebugLog("ApplicationCore::RegisterObjectInEcs: entity allocation failed.\n");
            return;
        }
        m_objectEntityMap.emplace(object, entity);
        m_ecsRegistry.AddComponent<ObjectRefComponent>(entity, ObjectRefComponent{ object });

        if (dynamic_cast<StaticModel*>(object)) {
            m_ecsRegistry.AddComponent<StaticModelTag>(entity);
        }
        if (dynamic_cast<PointLight*>(object)) {
            m_ecsRegistry.AddComponent<PointLightTag>(entity);
        }
        if (dynamic_cast<SpotLight*>(object)) {
            m_ecsRegistry.AddComponent<SpotLightTag>(entity);
        }
        if (dynamic_cast<Camera*>(object)) {
            m_ecsRegistry.AddComponent<CameraTag>(entity);
        }
    }

    void ApplicationCore::UnregisterObjectInEcs(SObject* object)
    {
        if (!object) {
            return;
        }

        const auto found = m_objectEntityMap.find(object);
        if (found == m_objectEntityMap.end()) {
            return;
        }

        m_ecsRegistry.DestroyEntity(found->second);
        m_objectEntityMap.erase(found);
    }

    std::vector<PointLight*> ApplicationCore::GetPointLightObjects() const
    {
        std::vector<PointLight*> out;
        const auto pointLightEntities = m_ecsRegistry.View<PointLightTag, ObjectRefComponent>();
        out.reserve(pointLightEntities.size());
        for (const EntityId entity : pointLightEntities) {
            const auto* objectRef = m_ecsRegistry.GetComponent<ObjectRefComponent>(entity);
            if (!objectRef || !objectRef->object) {
                continue;
            }
            out.push_back(static_cast<PointLight*>(objectRef->object));
        }
        return out;
    }

    std::vector<SpotLight*> ApplicationCore::GetSpotLightObjects() const
    {
        std::vector<SpotLight*> out;
        const auto spotLightEntities = m_ecsRegistry.View<SpotLightTag, ObjectRefComponent>();
        out.reserve(spotLightEntities.size());
        for (const EntityId entity : spotLightEntities) {
            const auto* objectRef = m_ecsRegistry.GetComponent<ObjectRefComponent>(entity);
            if (!objectRef || !objectRef->object) {
                continue;
            }
            out.push_back(static_cast<SpotLight*>(objectRef->object));
        }
        return out;
    }

    bool ApplicationCore::SetMainCamera(Camera* camera)
    {
        if (!camera) {
            m_activeCamera = nullptr;
            return true;
        }

        const auto found = m_objectEntityMap.find(camera);
        if (found == m_objectEntityMap.end()) {
            return false;
        }
        if (!m_ecsRegistry.HasComponent<CameraTag>(found->second)) {
            return false;
        }

        m_activeCamera = camera;
        return true;
    }

    bool ApplicationCore::SetActiveCamera(Camera* camera)
    {
        return SetMainCamera(camera);
    }

    bool ApplicationCore::DeleteObject(SObject* object)
    {
        if (!object) {
            return false;
        }

        const auto it = std::find_if(m_objects.begin(), m_objects.end(), [object](const std::unique_ptr<SObject>& entry) {
            return entry.get() == object;
        });
        if (it == m_objects.end()) {
            return false;
        }

        if (object == m_activeCamera) {
            m_activeCamera = nullptr;
        }
        UnregisterObjectInEcs(object);
        m_objects.erase(it);
        m_objectsDirty = true;
        return true;
    }

    void ApplicationCore::ClearObjects()
    {
        m_objects.clear();
        m_activeCamera = nullptr;
        m_ecsRegistry.Clear();
        m_objectEntityMap.clear();
        m_objectsDirty = true;
    }

    void ApplicationCore::SyncModelsToRenderer(Renderer& renderer)
    {
        if (!m_objectsDirty) {
            return;
        }

        renderer.ClearRenderObjects();

        std::vector<Renderer::RenderProxy> proxies;
        const auto staticModelEntities = m_ecsRegistry.View<StaticModelTag, ObjectRefComponent>();
        for (const EntityId entity : staticModelEntities) {
            auto* objectRef = m_ecsRegistry.GetComponent<ObjectRefComponent>(entity);
            if (!objectRef || !objectRef->object) {
                continue;
            }

            auto* model = static_cast<StaticModel*>(objectRef->object);
            auto modelProxies = model->BuildRenderProxies();
            proxies.reserve(proxies.size() + modelProxies.size());
            for (auto& proxy : modelProxies) {
                proxies.push_back(std::move(proxy));
            }
        }

        if (!proxies.empty()) {
            renderer.SubmitRenderProxies(std::move(proxies));
        }
        m_objectsDirty = false;
    }

    void ApplicationCore::SyncLightObjectsToRenderer(Renderer& renderer) const
    {
        std::vector<Renderer::PointLight> pointLights;
        std::vector<Renderer::SpotLight> spotLights;
        const auto pointLightEntities = m_ecsRegistry.View<PointLightTag, ObjectRefComponent>();
        const auto spotLightEntities = m_ecsRegistry.View<SpotLightTag, ObjectRefComponent>();
        pointLights.reserve(pointLightEntities.size());
        spotLights.reserve(spotLightEntities.size());

        for (const EntityId entity : pointLightEntities) {
            const auto* objectRef = m_ecsRegistry.GetComponent<ObjectRefComponent>(entity);
            if (!objectRef || !objectRef->object) {
                continue;
            }
            const auto* point = static_cast<const PointLight*>(objectRef->object);
            pointLights.push_back(point->BuildRenderLightProxy());
        }

        for (const EntityId entity : spotLightEntities) {
            const auto* objectRef = m_ecsRegistry.GetComponent<ObjectRefComponent>(entity);
            if (!objectRef || !objectRef->object) {
                continue;
            }
            const auto* spot = static_cast<const SpotLight*>(objectRef->object);
            spotLights.push_back(spot->BuildRenderLightProxy());
        }

        renderer.GetPointLights() = std::move(pointLights);
        renderer.GetSpotLights() = std::move(spotLights);
    }


} // namespace SasamiRenderer
