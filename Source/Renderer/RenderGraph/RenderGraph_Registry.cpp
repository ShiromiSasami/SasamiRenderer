// RenderGraph_Registry.cpp
// ResourceRegistry  Enamed resource tracking for the RenderGraph.
// Handles registration, lookup, and external resource import.
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    ResourceHandle ResourceRegistry::Register(std::string_view resourceName)
    {
        if (resourceName.empty()) {
            return {};
        }

        auto it = m_nameToIndex.find(std::string(resourceName));
        if (it != m_nameToIndex.end()) {
            return ResourceHandle{ it->second };
        }

        const size_t index = m_records.size();
        m_records.push_back(ResourceRecord{});
        m_records.back().name.assign(resourceName.begin(), resourceName.end());
        m_nameToIndex.emplace(m_records.back().name, index);
        return ResourceHandle{ index };
    }

    ResourceHandle ResourceRegistry::RegisterExternal(std::string_view resourceName,
                                                      const ExternalRenderGraphResourceDesc& desc)
    {
        const ResourceHandle handle = Register(resourceName);
        ResourceRecord* record = GetMutable(handle);
        if (!record) {
            return {};
        }

        record->hasExternalResource = true;
        record->rhiResource = desc.rhiResource;
        record->rhiInitialState = desc.rhiInitialState;
        record->rhiCurrentState = desc.rhiInitialState;
        record->rhiFinalState = desc.rhiFinalState;
        record->resource = desc.resource;
        record->initialState = desc.initialState;
        record->currentState = desc.initialState;
        record->finalState = desc.finalState;
        record->transitionToFinalState = desc.transitionToFinalState;
        record->rhiRtv = desc.rhiRtv;
        record->rhiDsv = desc.rhiDsv;
        record->rhiSrv = desc.rhiSrv;
        record->rtv = desc.rtv;
        record->hasRtv = desc.hasRtv;
        record->dsv = desc.dsv;
        record->hasDsv = desc.hasDsv;
        record->gpuSrv = desc.gpuSrv;
        record->hasSrv = desc.hasSrv;
        record->clearColorOnFirstUse = desc.clearColorOnFirstUse;
        record->clearColor[0] = desc.clearColor[0];
        record->clearColor[1] = desc.clearColor[1];
        record->clearColor[2] = desc.clearColor[2];
        record->clearColor[3] = desc.clearColor[3];
        record->clearDepthOnFirstUse = desc.clearDepthOnFirstUse;
        record->clearDepth = desc.clearDepth;
        record->clearStencil = desc.clearStencil;
        record->colorCleared = false;
        record->depthCleared = false;
        return handle;
    }

    ResourceHandle ResourceRegistry::Find(std::string_view resourceName) const
    {
        auto it = m_nameToIndex.find(std::string(resourceName));
        if (it == m_nameToIndex.end()) {
            return {};
        }
        return ResourceHandle{ it->second };
    }

    std::string_view ResourceRegistry::GetName(ResourceHandle handle) const
    {
        const ResourceRecord* record = Get(handle);
        return record ? std::string_view(record->name) : std::string_view{};
    }

    GpuDescriptorHandle ResourceRegistry::GetSrv(ResourceHandle handle) const
    {
        const ResourceRecord* record = Get(handle);
        return (record && record->hasSrv) ? record->gpuSrv : GpuDescriptorHandle{};
    }

    RhiGpuDescriptorHandle ResourceRegistry::GetRhiSrv(ResourceHandle handle) const
    {
        const ResourceRecord* record = Get(handle);
        return record ? record->rhiSrv : RhiGpuDescriptorHandle{};
    }

    ResourceRegistry::ResourceRecord* ResourceRegistry::GetMutable(ResourceHandle handle)
    {
        if (!handle.IsValid() || handle.index >= m_records.size()) {
            return nullptr;
        }
        return &m_records[handle.index];
    }

    const ResourceRegistry::ResourceRecord* ResourceRegistry::Get(ResourceHandle handle) const
    {
        if (!handle.IsValid() || handle.index >= m_records.size()) {
            return nullptr;
        }
        return &m_records[handle.index];
    }

    void ResourceRegistry::Clear()
    {
        m_nameToIndex.clear();
        m_records.clear();
    }


} // namespace SasamiRenderer
