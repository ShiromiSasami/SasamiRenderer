#include "Renderer/Core/RenderGraph.h"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Passes/IRenderNode.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"

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
        record->resource = desc.resource;
        record->initialState = desc.initialState;
        record->currentState = desc.initialState;
        record->finalState = desc.finalState;
        record->transitionToFinalState = desc.transitionToFinalState;
        record->rtv = desc.rtv;
        record->hasRtv = desc.hasRtv;
        record->dsv = desc.dsv;
        record->hasDsv = desc.hasDsv;
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

    RenderGraph::PassHandle RenderGraph::AddPassInternal(const std::string& name,
                                                         const ExecuteCallback& execute,
                                                         PhaseHandle phase)
    {
        PassNode node{};
        node.name = name;
        node.execute = execute;
        node.phaseIndex = phase.index;
        m_passes.push_back(std::move(node));
        return PassHandle{ m_passes.size() - 1u };
    }

    RenderGraph::PhaseHandle RenderGraph::FindOrAddPhase(std::string_view phaseName)
    {
        std::string normalizedPhaseName;
        if (!phaseName.empty()) {
            normalizedPhaseName.assign(phaseName.begin(), phaseName.end());
        } else {
            normalizedPhaseName = "Default";
        }

        auto it = m_phaseNameToIndex.find(normalizedPhaseName);
        if (it != m_phaseNameToIndex.end()) {
            return PhaseHandle{ it->second };
        }

        const size_t phaseIndex = m_phases.size();
        m_phaseNameToIndex.emplace(normalizedPhaseName, phaseIndex);

        PhaseNode node{};
        node.name = std::move(normalizedPhaseName);
        m_phases.push_back(std::move(node));
        return PhaseHandle{ phaseIndex };
    }

    RenderGraph::PassHandle RenderGraph::AddPass(const IRenderNode& renderNode,
                                                 const RenderGraphExecuteContext& executeContext,
                                                 PassHandle previousPass)
    {
        RenderGraphExecuteContext boundContext = executeContext;
        if (boundContext.resources == nullptr) {
            boundContext.resources = &m_resources;
        }
        m_executeContext = boundContext;
        m_hasExecuteContext = true;

        RenderNodeRequirementBuilder requirementBuilder;
        renderNode.BuildRequirements(requirementBuilder);
        const RenderNodeRequirements requirements = requirementBuilder.Build();

        const ExecuteCallback execute = [&renderNode, boundContext, requirements]() -> bool {
            const RenderNodeContextView contextView = boundContext.CreateContextView(requirements);
            if (!contextView.IsSatisfied()) {
                DebugLog("RenderGraph::AddPass: runtime requirements are not satisfied.\n");
                return false;
            }
            return renderNode.Execute(contextView);
        };

        std::string passName = "Node";
        const std::string_view tag = renderNode.Tag();
        if (!tag.empty()) {
            passName.assign(tag.begin(), tag.end());
        }
        passName += "_" + std::to_string(m_passes.size());

        const PhaseHandle phase = FindOrAddPhase(renderNode.PhaseTag());
        PassHandle pass = AddPassInternal(passName, execute, phase);
        if (!pass.IsValid()) {
            return {};
        }
        if (phase.IsValid() && phase.index < m_phases.size()) {
            ++m_phases[phase.index].passCount;
        }

        if (previousPass.IsValid()) {
            DependsOn(pass, previousPass);
        }

        RenderGraphBuilder builder(*this, m_resources, pass, previousPass);
        renderNode.Setup(builder);
        return pass;
    }

    RenderGraph::PhaseHandle RenderGraph::AddPhase(std::string_view phaseName)
    {
        return FindOrAddPhase(phaseName);
    }

    bool RenderGraph::AddPhaseCompletionNode(std::string_view phaseName,
                                             std::string_view nodeName,
                                             const ExecuteCallback& execute,
                                             PhaseCompletionMode mode)
    {
        if (!execute) {
            return false;
        }

        const PhaseHandle phase = FindOrAddPhase(phaseName);
        if (!phase.IsValid() || phase.index >= m_phases.size()) {
            return false;
        }

        PhaseCompletionNode node{};
        if (!nodeName.empty()) {
            node.name.assign(nodeName.begin(), nodeName.end());
        } else {
            node.name = "PhaseCompletionNode";
        }
        node.execute = execute;

        PhaseNode& phaseNode = m_phases[phase.index];
        if (mode == PhaseCompletionMode::Deterministic) {
            phaseNode.deterministicNodes.push_back(std::move(node));
        } else {
            phaseNode.opportunisticNodes.push_back(std::move(node));
        }
        return true;
    }

    ResourceHandle RenderGraph::ImportExternalResource(std::string_view resourceName,
                                                       const ExternalRenderGraphResourceDesc& desc)
    {
        return m_resources.RegisterExternal(resourceName, desc);
    }

    void RenderGraph::Read(PassHandle pass, ResourceHandle resource)
    {
        if (!pass.IsValid() || pass.index >= m_passes.size() || !resource.IsValid()) {
            return;
        }
        m_passes[pass.index].reads.push_back(resource);
    }

    void RenderGraph::Write(PassHandle pass, ResourceHandle resource)
    {
        if (!pass.IsValid() || pass.index >= m_passes.size() || !resource.IsValid()) {
            return;
        }
        m_passes[pass.index].writes.push_back(resource);
    }

    void RenderGraph::UseColorTarget(PassHandle pass, ResourceHandle resource)
    {
        if (!pass.IsValid() || pass.index >= m_passes.size() || !resource.IsValid()) {
            return;
        }
        m_passes[pass.index].colorTargets.push_back(resource);
    }

    void RenderGraph::UseDepthTarget(PassHandle pass, ResourceHandle resource)
    {
        if (!pass.IsValid() || pass.index >= m_passes.size() || !resource.IsValid()) {
            return;
        }
        m_passes[pass.index].depthTarget = resource;
    }

    void RenderGraph::DependsOn(PassHandle pass, PassHandle dependency)
    {
        if (!pass.IsValid() ||
            !dependency.IsValid() ||
            pass.index >= m_passes.size() ||
            dependency.index >= m_passes.size()) {
            return;
        }
        m_passes[pass.index].explicitDependencies.push_back(dependency.index);
    }

    bool RenderGraph::BuildExecutionOrder(std::vector<size_t>& outOrder) const
    {
        outOrder.clear();
        const size_t passCount = m_passes.size();
        if (passCount == 0) {
            return true;
        }

        std::unordered_map<std::string, size_t> passNameToIndex;
        passNameToIndex.reserve(passCount);
        for (size_t i = 0; i < passCount; ++i) {
            const auto [it, inserted] = passNameToIndex.emplace(m_passes[i].name, i);
            if (!inserted) {
                DebugLog("RenderGraph::BuildExecutionOrder: duplicate pass name detected.\n");
                return false;
            }
        }

        std::vector<std::unordered_set<size_t>> adjacency(passCount);
        std::vector<size_t> indegree(passCount, 0u);

        auto addEdge = [&adjacency, &indegree](size_t from, size_t to) {
            if (from == to) {
                return;
            }
            if (adjacency[from].insert(to).second) {
                ++indegree[to];
            }
        };

        for (size_t i = 0; i < passCount; ++i) {
            for (const size_t dep : m_passes[i].explicitDependencies) {
                if (dep >= passCount) {
                    DebugLog("RenderGraph::BuildExecutionOrder: invalid dependency index.\n");
                    return false;
                }
                addEdge(dep, i);
            }
        }

        auto hasConflict = [](const std::vector<ResourceHandle>& writes,
                              const std::vector<ResourceHandle>& readsOrWrites) -> bool {
            for (const ResourceHandle write : writes) {
                if (!write.IsValid()) {
                    continue;
                }
                if (std::find(readsOrWrites.begin(), readsOrWrites.end(), write) != readsOrWrites.end()) {
                    return true;
                }
            }
            return false;
        };

        for (size_t i = 0; i < passCount; ++i) {
            for (size_t j = i + 1; j < passCount; ++j) {
                std::vector<ResourceHandle> laterAccess = m_passes[j].reads;
                laterAccess.insert(laterAccess.end(), m_passes[j].writes.begin(), m_passes[j].writes.end());
                if (hasConflict(m_passes[i].writes, laterAccess)) {
                    addEdge(i, j);
                }
            }
        }

        std::queue<size_t> ready;
        for (size_t i = 0; i < passCount; ++i) {
            if (indegree[i] == 0) {
                ready.push(i);
            }
        }

        while (!ready.empty()) {
            const size_t node = ready.front();
            ready.pop();
            outOrder.push_back(node);

            for (const size_t next : adjacency[node]) {
                if (--indegree[next] == 0) {
                    ready.push(next);
                }
            }
        }

        if (outOrder.size() != passCount) {
            DebugLog("RenderGraph::BuildExecutionOrder: cycle detected.\n");
            return false;
        }

        return true;
    }

    CommandList* RenderGraph::GetCommandList() const
    {
        if (!m_hasExecuteContext || !m_executeContext.frameInputs) {
            return nullptr;
        }
        return m_executeContext.frameInputs->cmdList;
    }

    bool RenderGraph::TransitionResource(ResourceRegistry::ResourceRecord& resource,
                                         D3D12_RESOURCE_STATES requiredState)
    {
        if (!resource.hasExternalResource || resource.resource == nullptr) {
            return false;
        }

        if (resource.currentState == requiredState) {
            return true;
        }

        CommandList* cmdList = GetCommandList();
        if (!cmdList) {
            return false;
        }

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = resource.resource;
        barrier.Transition.StateBefore = resource.currentState;
        barrier.Transition.StateAfter = requiredState;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
        resource.currentState = requiredState;
        return true;
    }

    bool RenderGraph::PreparePassResources(const PassNode& pass)
    {
        if (pass.colorTargets.empty() && !pass.depthTarget.IsValid()) {
            return true;
        }

        CommandList* cmdList = GetCommandList();
        if (!cmdList) {
            DebugLog("RenderGraph::PreparePassResources: command list is null.\n");
            return false;
        }

        std::vector<CpuDescriptorHandle> rtvs;
        rtvs.reserve(pass.colorTargets.size());
        for (const ResourceHandle handle : pass.colorTargets) {
            ResourceRegistry::ResourceRecord* resource = m_resources.GetMutable(handle);
            if (!resource || !resource->hasExternalResource || resource->resource == nullptr || !resource->hasRtv) {
                DebugLog("RenderGraph::PreparePassResources: color target import is missing.\n");
                return false;
            }
            if (!TransitionResource(*resource, D3D12_RESOURCE_STATE_RENDER_TARGET)) {
                DebugLog("RenderGraph::PreparePassResources: failed to transition color target.\n");
                return false;
            }
            rtvs.push_back(resource->rtv);
        }

        CpuDescriptorHandle dsv{};
        CpuDescriptorHandle* dsvPtr = nullptr;
        if (pass.depthTarget.IsValid()) {
            ResourceRegistry::ResourceRecord* resource = m_resources.GetMutable(pass.depthTarget);
            if (!resource || !resource->hasExternalResource || resource->resource == nullptr || !resource->hasDsv) {
                DebugLog("RenderGraph::PreparePassResources: depth target import is missing.\n");
                return false;
            }
            if (!TransitionResource(*resource, D3D12_RESOURCE_STATE_DEPTH_WRITE)) {
                DebugLog("RenderGraph::PreparePassResources: failed to transition depth target.\n");
                return false;
            }
            dsv = resource->dsv;
            dsvPtr = &dsv;
        }

        cmdList->OMSetRenderTargets(static_cast<UINT>(rtvs.size()),
                                    rtvs.empty() ? nullptr : rtvs.data(),
                                    FALSE,
                                    dsvPtr);

        for (const ResourceHandle handle : pass.colorTargets) {
            ResourceRegistry::ResourceRecord* resource = m_resources.GetMutable(handle);
            if (resource && resource->clearColorOnFirstUse && !resource->colorCleared) {
                cmdList->ClearRenderTargetView(resource->rtv, resource->clearColor, 0, nullptr);
                resource->colorCleared = true;
            }
        }

        if (pass.depthTarget.IsValid()) {
            ResourceRegistry::ResourceRecord* resource = m_resources.GetMutable(pass.depthTarget);
            if (resource && resource->clearDepthOnFirstUse && !resource->depthCleared) {
                cmdList->ClearDepthStencilView(resource->dsv,
                                               D3D12_CLEAR_FLAG_DEPTH,
                                               resource->clearDepth,
                                               resource->clearStencil,
                                               0,
                                               nullptr);
                resource->depthCleared = true;
            }
        }

        return true;
    }

    void RenderGraph::FinalizeExternalResources()
    {
        CommandList* cmdList = GetCommandList();
        if (!cmdList) {
            return;
        }

        for (ResourceRegistry::ResourceRecord& resource : m_resources.MutableRecords()) {
            if (!resource.hasExternalResource || !resource.transitionToFinalState || resource.resource == nullptr) {
                continue;
            }
            (void)TransitionResource(resource, resource.finalState);
        }
    }

    bool RenderGraph::Execute()
    {
        std::vector<size_t> executionOrder;
        if (!BuildExecutionOrder(executionOrder)) {
            return false;
        }

        auto runPhaseCompletionNodes = [this](size_t phaseIndex, PhaseCompletionMode mode) -> bool {
            if (!ExecutePhaseCompletionNodes(phaseIndex, mode)) {
                if (mode == PhaseCompletionMode::Deterministic) {
                    DebugLog("RenderGraph::Execute: deterministic phase completion node failed.\n");
                    return false;
                }
                DebugLog("RenderGraph::Execute: opportunistic phase completion node failed (ignored).\n");
            }
            return true;
        };

        bool executeSucceeded = true;
        std::vector<size_t> completedPassCountByPhase(m_phases.size(), 0u);
        std::vector<bool> phaseCompletionFired(m_phases.size(), false);
        for (size_t phaseIndex = 0; phaseIndex < m_phases.size(); ++phaseIndex) {
            if (m_phases[phaseIndex].passCount == 0u) {
                if (!runPhaseCompletionNodes(phaseIndex, PhaseCompletionMode::Deterministic)) {
                    executeSucceeded = false;
                    break;
                }
                runPhaseCompletionNodes(phaseIndex, PhaseCompletionMode::Opportunistic);
                phaseCompletionFired[phaseIndex] = true;
            }
        }

        if (executeSucceeded) {
            for (const size_t passIndex : executionOrder) {
                const auto& pass = m_passes[passIndex];
                if (!pass.execute) {
                    DebugLog("RenderGraph::Execute: pass execute callback is null.\n");
                    executeSucceeded = false;
                    break;
                }
                if (!PreparePassResources(pass)) {
                    DebugLog("RenderGraph::Execute: failed to prepare pass resources.\n");
                    executeSucceeded = false;
                    break;
                }
                if (!pass.execute()) {
                    DebugLog("RenderGraph::Execute: pass execution failed.\n");
                    executeSucceeded = false;
                    break;
                }

                if (pass.phaseIndex >= m_phases.size() || phaseCompletionFired[pass.phaseIndex]) {
                    continue;
                }

                const size_t completeCount = ++completedPassCountByPhase[pass.phaseIndex];
                const size_t phasePassCount = m_phases[pass.phaseIndex].passCount;
                if (phasePassCount > 0u && completeCount >= phasePassCount) {
                    if (!runPhaseCompletionNodes(pass.phaseIndex, PhaseCompletionMode::Deterministic)) {
                        executeSucceeded = false;
                        break;
                    }
                    runPhaseCompletionNodes(pass.phaseIndex, PhaseCompletionMode::Opportunistic);
                    phaseCompletionFired[pass.phaseIndex] = true;
                }
            }
        }

        FinalizeExternalResources();
        return executeSucceeded;
    }

    void RenderGraph::Clear()
    {
        m_passes.clear();
        m_phaseNameToIndex.clear();
        m_phases.clear();
        m_resources.Clear();
        m_executeContext = {};
        m_hasExecuteContext = false;
    }

    bool RenderGraph::ExecutePhaseCompletionNodes(size_t phaseIndex, PhaseCompletionMode mode) const
    {
        if (phaseIndex >= m_phases.size()) {
            return true;
        }

        const PhaseNode& phase = m_phases[phaseIndex];
        const std::vector<PhaseCompletionNode>& nodes = (mode == PhaseCompletionMode::Deterministic)
            ? phase.deterministicNodes
            : phase.opportunisticNodes;

        for (const PhaseCompletionNode& node : nodes) {
            if (!node.execute) {
                DebugLog("RenderGraph::ExecutePhaseCompletionNodes: completion node callback is null.\n");
                return false;
            }
            if (!node.execute()) {
                DebugLog("RenderGraph::ExecutePhaseCompletionNodes: completion node execution failed.\n");
                return false;
            }
        }
        return true;
    }

    RenderGraphBuilder::RenderGraphBuilder(RenderGraph& renderGraph,
                                           ResourceRegistry& resources,
                                           RenderGraph::PassHandle pass,
                                           RenderGraph::PassHandle previousPass)
        : m_renderGraph(&renderGraph)
        , m_resources(&resources)
        , m_pass(pass)
        , m_previousPass(previousPass)
    {
    }

    ResourceHandle RenderGraphBuilder::Import(std::string_view resourceName)
    {
        if (m_resources == nullptr) {
            return {};
        }
        return m_resources->Register(resourceName);
    }

    ResourceHandle RenderGraphBuilder::Read(std::string_view resourceName)
    {
        const ResourceHandle handle = Import(resourceName);
        return Read(handle);
    }

    ResourceHandle RenderGraphBuilder::Read(ResourceHandle handle)
    {
        if (m_renderGraph == nullptr || !m_pass.IsValid() || !handle.IsValid()) {
            return {};
        }

        m_renderGraph->Read(m_pass, handle);
        return handle;
    }

    ResourceHandle RenderGraphBuilder::Write(std::string_view resourceName)
    {
        const ResourceHandle handle = Import(resourceName);
        return Write(handle);
    }

    ResourceHandle RenderGraphBuilder::Write(ResourceHandle handle)
    {
        if (m_renderGraph == nullptr || !m_pass.IsValid() || !handle.IsValid()) {
            return {};
        }

        m_renderGraph->Write(m_pass, handle);
        return handle;
    }

    ResourceHandle RenderGraphBuilder::UseColorTarget(std::string_view resourceName)
    {
        const ResourceHandle handle = Import(resourceName);
        return UseColorTarget(handle);
    }

    ResourceHandle RenderGraphBuilder::UseColorTarget(ResourceHandle handle)
    {
        if (m_renderGraph == nullptr || !m_pass.IsValid() || !handle.IsValid()) {
            return {};
        }

        m_renderGraph->UseColorTarget(m_pass, handle);
        return handle;
    }

    ResourceHandle RenderGraphBuilder::UseDepthTarget(std::string_view resourceName)
    {
        const ResourceHandle handle = Import(resourceName);
        return UseDepthTarget(handle);
    }

    ResourceHandle RenderGraphBuilder::UseDepthTarget(ResourceHandle handle)
    {
        if (m_renderGraph == nullptr || !m_pass.IsValid() || !handle.IsValid()) {
            return {};
        }

        m_renderGraph->UseDepthTarget(m_pass, handle);
        return handle;
    }

    void RenderGraphBuilder::DependsOnPrevious()
    {
        if (m_renderGraph == nullptr || !m_pass.IsValid() || !m_previousPass.IsValid()) {
            return;
        }
        m_renderGraph->DependsOn(m_pass, m_previousPass);
    }

    RenderNodeContextView RenderGraphExecuteContext::CreateContextView(const RenderNodeRequirements& requirements) const
    {
        return RenderNodeContextView(executionPolicy, frameInputs, executionServices, requirements);
    }

    ResourceHandle RenderGraphExecuteContext::FindGraphResource(std::string_view resourceName) const
    {
        if (resources == nullptr) {
            return {};
        }
        return resources->Find(resourceName);
    }

    std::string_view RenderGraphExecuteContext::GetResourceName(ResourceHandle handle) const
    {
        if (resources == nullptr) {
            return {};
        }
        return resources->GetName(handle);
    }
}
