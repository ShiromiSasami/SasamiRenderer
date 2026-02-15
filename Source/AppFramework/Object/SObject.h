#pragma once

#include <memory>
#include <typeindex>
#include <unordered_map>
#include <utility>

#include "AppFramework/Component/IComponent.h"

namespace SasamiRenderer
{
    class SObject
    {
    public:
        virtual ~SObject() = default;

        template<ComponentType TComponent, typename... TArgs>
        TComponent& AddComponent(TArgs&&... args)
        {
            const std::type_index key(typeid(TComponent));
            const auto existing = m_components.find(key);
            if (existing != m_components.end()) {
                return *static_cast<TComponent*>(existing->second.get());
            }

            std::unique_ptr<TComponent> component =
                std::make_unique<TComponent>(std::forward<TArgs>(args)...);
            TComponent* componentRaw = component.get();
            m_components.emplace(key, std::move(component));
            return *componentRaw;
        }

        template<ComponentType TComponent>
        TComponent* GetComponent()
        {
            const std::type_index key(typeid(TComponent));
            const auto found = m_components.find(key);
            if (found == m_components.end()) {
                return nullptr;
            }
            return static_cast<TComponent*>(found->second.get());
        }

        template<ComponentType TComponent>
        const TComponent* GetComponent() const
        {
            const std::type_index key(typeid(TComponent));
            const auto found = m_components.find(key);
            if (found == m_components.end()) {
                return nullptr;
            }
            return static_cast<const TComponent*>(found->second.get());
        }

        template<ComponentType TComponent>
        bool HasComponent() const
        {
            const std::type_index key(typeid(TComponent));
            return m_components.find(key) != m_components.end();
        }

    private:
        std::unordered_map<std::type_index, std::unique_ptr<IComponent>> m_components;
    };
}
