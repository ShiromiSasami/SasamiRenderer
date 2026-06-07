#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SasamiRenderer
{
    class EcsRegistry
    {
    public:
        using EntityId = std::uint32_t;
        static constexpr EntityId INVALID_ENTITY = 0;

        enum class EntityPreset : std::uint8_t
        {
            Generic = 0,
            StaticModel,
            SkinnedModel,
            PointLight,
            SpotLight,
            Camera,
            Count,
        };
        static constexpr size_t kPresetCount = static_cast<size_t>(EntityPreset::Count);

        EntityId CreateEntity(EntityPreset preset = EntityPreset::Generic)
        {
            EntityId entity = INVALID_ENTITY;
            if (!m_freeEntityIds.empty()) {
                entity = m_freeEntityIds.back();
                m_freeEntityIds.pop_back();
            } else {
                if (m_nextEntityId == INVALID_ENTITY) {
                    return INVALID_ENTITY;
                }
                entity = m_nextEntityId;
                if (m_nextEntityId == ((std::numeric_limits<EntityId>::max)())) {
                    m_nextEntityId = INVALID_ENTITY;
                } else {
                    ++m_nextEntityId;
                }
                if (m_entityAliveFlags.size() <= static_cast<size_t>(entity)) {
                    m_entityAliveFlags.resize(static_cast<size_t>(entity) + 1u, 0u);
                    m_entityPresets.resize(static_cast<size_t>(entity) + 1u, EntityPreset::Generic);
                }
            }
            m_entityAliveFlags[entity] = 1u;
            AssignPreset(entity, preset);
            return entity;
        }

        bool DestroyEntity(EntityId entity)
        {
            if (!IsAlive(entity)) {
                return false;
            }

            for (auto& [_, pool] : m_componentPools) {
                pool->Remove(entity);
            }
            RemoveFromPresetList(entity, GetPreset(entity));
            m_entityPresets[entity] = EntityPreset::Generic;
            m_entityAliveFlags[entity] = 0u;
            m_freeEntityIds.push_back(entity);
            return true;
        }

        void Clear()
        {
            m_componentPools.clear();
            m_entityAliveFlags.clear();
            m_entityAliveFlags.resize(1u, 0u);
            m_entityPresets.clear();
            m_entityPresets.resize(1u, EntityPreset::Generic);
            for (auto& presetEntities : m_presetEntityIds) {
                presetEntities.clear();
            }
            m_freeEntityIds.clear();
            m_nextEntityId = 1u;
        }

        bool IsAlive(EntityId entity) const
        {
            return entity != INVALID_ENTITY &&
                static_cast<size_t>(entity) < m_entityAliveFlags.size() &&
                m_entityAliveFlags[entity] != 0u;
        }

        EntityPreset GetPreset(EntityId entity) const
        {
            if (entity == INVALID_ENTITY ||
                static_cast<size_t>(entity) >= m_entityPresets.size()) {
                return EntityPreset::Generic;
            }
            return m_entityPresets[entity];
        }

        bool SetPreset(EntityId entity, EntityPreset preset)
        {
            if (!IsAlive(entity)) {
                return false;
            }
            AssignPreset(entity, preset);
            return true;
        }

        std::vector<EntityId> ViewPreset(EntityPreset preset) const
        {
            std::vector<EntityId> result;
            const auto& entities = m_presetEntityIds[PresetIndex(preset)];
            result.reserve(entities.size());
            for (const EntityId entity : entities) {
                if (IsAlive(entity) && GetPreset(entity) == preset) {
                    result.push_back(entity);
                }
            }
            return result;
        }

        template<typename TComponent, typename... TArgs>
        TComponent& AddComponent(EntityId entity, TArgs&&... args)
        {
            static_assert(std::is_default_constructible_v<TComponent> || sizeof...(TArgs) > 0,
                          "Component must be constructible.");
            auto& pool = GetOrCreatePool<TComponent>();
            auto [it, _] = pool.components.insert_or_assign(entity, TComponent(std::forward<TArgs>(args)...));
            return it->second;
        }

        template<typename TComponent>
        TComponent* GetComponent(EntityId entity)
        {
            auto* pool = FindPool<TComponent>();
            if (!pool) {
                return nullptr;
            }

            const auto found = pool->components.find(entity);
            if (found == pool->components.end()) {
                return nullptr;
            }
            return &found->second;
        }

        template<typename TComponent>
        const TComponent* GetComponent(EntityId entity) const
        {
            const auto* pool = FindPool<TComponent>();
            if (!pool) {
                return nullptr;
            }

            const auto found = pool->components.find(entity);
            if (found == pool->components.end()) {
                return nullptr;
            }
            return &found->second;
        }

        template<typename TComponent>
        bool HasComponent(EntityId entity) const
        {
            return GetComponent<TComponent>(entity) != nullptr;
        }

        template<typename... TComponents>
        std::vector<EntityId> View() const
        {
            std::vector<EntityId> result;
            if constexpr (sizeof...(TComponents) == 0) {
                result.reserve(m_entityAliveFlags.size());
                for (EntityId entity = 1u;
                     static_cast<size_t>(entity) < m_entityAliveFlags.size();
                     ++entity) {
                    if (m_entityAliveFlags[entity] != 0u) {
                        result.push_back(entity);
                    }
                }
                return result;
            } else {
                const auto seed = GetSeedEntities<TComponents...>();
                result.reserve(seed.size());
                for (const EntityId entity : seed) {
                    if ((HasComponent<TComponents>(entity) && ...)) {
                        result.push_back(entity);
                    }
                }
                return result;
            }
        }

    private:
        static constexpr size_t PresetIndex(EntityPreset preset)
        {
            return static_cast<size_t>(preset);
        }

        struct IComponentPool
        {
            virtual ~IComponentPool() = default;
            virtual void Remove(EntityId entity) = 0;
            virtual std::vector<EntityId> Entities() const = 0;
        };

        template<typename TComponent>
        struct ComponentPool final : IComponentPool
        {
            std::unordered_map<EntityId, TComponent> components;

            void Remove(EntityId entity) override
            {
                components.erase(entity);
            }

            std::vector<EntityId> Entities() const override
            {
                std::vector<EntityId> entities;
                entities.reserve(components.size());
                for (const auto& [entity, _] : components) {
                    entities.push_back(entity);
                }
                return entities;
            }
        };

        template<typename TComponent>
        ComponentPool<TComponent>& GetOrCreatePool()
        {
            const std::type_index key(typeid(TComponent));
            const auto found = m_componentPools.find(key);
            if (found != m_componentPools.end()) {
                return *static_cast<ComponentPool<TComponent>*>(found->second.get());
            }

            auto pool = std::make_unique<ComponentPool<TComponent>>();
            auto* poolRaw = pool.get();
            m_componentPools.emplace(key, std::move(pool));
            return *poolRaw;
        }

        template<typename TComponent>
        ComponentPool<TComponent>* FindPool()
        {
            const std::type_index key(typeid(TComponent));
            const auto found = m_componentPools.find(key);
            if (found == m_componentPools.end()) {
                return nullptr;
            }
            return static_cast<ComponentPool<TComponent>*>(found->second.get());
        }

        template<typename TComponent>
        const ComponentPool<TComponent>* FindPool() const
        {
            const std::type_index key(typeid(TComponent));
            const auto found = m_componentPools.find(key);
            if (found == m_componentPools.end()) {
                return nullptr;
            }
            return static_cast<const ComponentPool<TComponent>*>(found->second.get());
        }

        template<typename TFirst, typename... TRest>
        std::vector<EntityId> GetSeedEntities() const
        {
            const auto* firstPool = FindPool<TFirst>();
            if (!firstPool) {
                return {};
            }

            std::vector<EntityId> best = firstPool->Entities();
            if constexpr (sizeof...(TRest) > 0) {
                (([&]() {
                    const auto* pool = FindPool<TRest>();
                    if (!pool) {
                        best.clear();
                        return;
                    }
                    auto entities = pool->Entities();
                    if (entities.size() < best.size()) {
                        best = std::move(entities);
                    }
                }()), ...);
            }
            return best;
        }

        void AssignPreset(EntityId entity, EntityPreset preset)
        {
            if (entity == INVALID_ENTITY) {
                return;
            }
            if (static_cast<size_t>(entity) >= m_entityPresets.size()) {
                m_entityPresets.resize(static_cast<size_t>(entity) + 1u, EntityPreset::Generic);
            }

            const EntityPreset oldPreset = m_entityPresets[entity];
            if (oldPreset == preset) {
                auto& presetEntities = m_presetEntityIds[PresetIndex(preset)];
                if (std::find(presetEntities.begin(), presetEntities.end(), entity) == presetEntities.end()) {
                    presetEntities.push_back(entity);
                }
                return;
            }

            RemoveFromPresetList(entity, oldPreset);
            m_entityPresets[entity] = preset;
            m_presetEntityIds[PresetIndex(preset)].push_back(entity);
        }

        void RemoveFromPresetList(EntityId entity, EntityPreset preset)
        {
            auto& presetEntities = m_presetEntityIds[PresetIndex(preset)];
            presetEntities.erase(
                std::remove(presetEntities.begin(), presetEntities.end(), entity),
                presetEntities.end());
        }

        EntityId m_nextEntityId = 1u;
        std::vector<std::uint8_t> m_entityAliveFlags = { 0u };
        std::vector<EntityPreset> m_entityPresets = { EntityPreset::Generic };
        std::array<std::vector<EntityId>, kPresetCount> m_presetEntityIds{};
        std::vector<EntityId> m_freeEntityIds;
        std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> m_componentPools;
    };
}
