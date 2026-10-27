#include "Renderer/RenderGraph/RenderGraph.h"

#include <algorithm>
#include <cstdio>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Profiling/Profiler.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"
#include "Renderer/Profiling/GpuTimestampProfiler.h"

namespace SasamiEngine::Render
{
    namespace
    {
        // Closes the pass's timestamp scope on every exit path, including the two
        // early `return false`s inside executePass. An unclosed scope would be
        // dropped by EndFrame, silently losing every later pass's timing too,
        // because the profiler refuses to open a scope while one is still open.
        class ScopedGpuTimestamp
        {
        public:
            ScopedGpuTimestamp(GpuTimestampProfiler* profiler,
                               ID3D12GraphicsCommandList* cmdList,
                               std::string_view name)
                : m_profiler(profiler), m_cmdList(cmdList)
            {
                if (m_profiler && m_cmdList) {
                    m_profiler->BeginScope(m_cmdList, name);
                }
            }
            ~ScopedGpuTimestamp()
            {
                if (m_profiler && m_cmdList) {
                    m_profiler->EndScope(m_cmdList);
                }
            }
            ScopedGpuTimestamp(const ScopedGpuTimestamp&) = delete;
            ScopedGpuTimestamp& operator=(const ScopedGpuTimestamp&) = delete;

        private:
            GpuTimestampProfiler*      m_profiler = nullptr;
            ID3D12GraphicsCommandList* m_cmdList  = nullptr;
        };

        RhiResourceState ToRhiResourceState(D3D12_RESOURCE_STATES state)
        {
            if ((state & D3D12_RESOURCE_STATE_RENDER_TARGET) != 0) {
                return RhiResourceState::RenderTarget;
            }
            if ((state & D3D12_RESOURCE_STATE_DEPTH_WRITE) != 0) {
                return RhiResourceState::DepthWrite;
            }
            if ((state & D3D12_RESOURCE_STATE_DEPTH_READ) != 0) {
                return RhiResourceState::DepthRead;
            }
            if ((state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0) {
                return RhiResourceState::UnorderedAccess;
            }
            if ((state & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0) {
                return RhiResourceState::CopySource;
            }
            if ((state & D3D12_RESOURCE_STATE_COPY_DEST) != 0) {
                return RhiResourceState::CopyDest;
            }
            if ((state & D3D12_RESOURCE_STATE_PRESENT) != 0) {
                return RhiResourceState::Present;
            }
            if ((state & D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) != 0) {
                return RhiResourceState::IndirectArgument;
            }
            if ((state & (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) != 0) {
                return RhiResourceState::ShaderResource;
            }
            // Reaching here with a non-zero state means the mapping does not know it, and the
            // resource is about to be described to the RHI as Common -- which the Vulkan backend
            // answers with conservative barrier masks, so it is safe, but the fact that a state
            // was lost is otherwise invisible. D3D12_RESOURCE_STATE_COMMON and _PRESENT are both
            // zero and land here legitimately, so only a non-zero state is worth reporting.
            if (state != 0) {
                // Render-thread only, and each distinct state is announced once: this must not
                // turn into per-frame logging.
                static std::vector<D3D12_RESOURCE_STATES> reportedStates;
                if (std::find(reportedStates.begin(), reportedStates.end(), state) == reportedStates.end()) {
                    reportedStates.push_back(state);
                    char message[160] = {};
                    sprintf_s(message,
                              "RenderGraph::ToRhiResourceState: unmapped D3D12 state 0x%X treated as Common.\n",
                              static_cast<unsigned int>(state));
                    DebugLog(message);
                }
            }
            return RhiResourceState::Common;
        }

        // The inverse, and deliberately a local copy rather than the backend's ToDx12State: that
        // one lives behind #if RHI_DIRECTX12, while this file builds for every backend. It must
        // agree with the RHI's own mapping -- that is the whole point of calling it "realised".
        D3D12_RESOURCE_STATES ToRealisedD3D12State(RhiResourceState state)
        {
            switch (state) {
            case RhiResourceState::RenderTarget:    return D3D12_RESOURCE_STATE_RENDER_TARGET;
            case RhiResourceState::DepthWrite:      return D3D12_RESOURCE_STATE_DEPTH_WRITE;
            case RhiResourceState::DepthRead:       return D3D12_RESOURCE_STATE_DEPTH_READ;
            case RhiResourceState::ShaderResource:
                return static_cast<D3D12_RESOURCE_STATES>(
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            case RhiResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            case RhiResourceState::CopySource:      return D3D12_RESOURCE_STATE_COPY_SOURCE;
            case RhiResourceState::CopyDest:        return D3D12_RESOURCE_STATE_COPY_DEST;
            case RhiResourceState::Present:         return D3D12_RESOURCE_STATE_PRESENT;
            case RhiResourceState::IndirectArgument: return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            case RhiResourceState::Common:
            default:                                return D3D12_RESOURCE_STATE_COMMON;
            }
        }
    }

    // ResourceRegistry implementation ↁERenderGraph_Registry.cpp

    RenderGraph::NodeHandle RenderGraph::AddPassInternal(const std::string& name,
                                                         const ExecuteCallback& execute,
                                                         PhaseHandle phase)
    {
        PassNode node{};
        node.name = name;
        node.execute = execute;
        node.phaseIndex = phase.index;
        m_passes.push_back(std::move(node));
        return NodeHandle{ m_passes.size() - 1u };
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

    RenderGraph::NodeHandle RenderGraph::AddNode(const IRenderPass& renderPass,
                                                 const RenderGraphExecuteContext& executeContext,
                                                 NodeHandle previousNode)
    {
        RenderGraphExecuteContext boundContext = executeContext;
        if (boundContext.resources == nullptr) {
            boundContext.resources = &m_resources;
        }
        m_executeContext = boundContext;
        m_hasExecuteContext = true;

        RenderPassRequirementBuilder requirementBuilder;
        renderPass.BuildRequirements(requirementBuilder);
        const RenderPassRequirements requirements = requirementBuilder.Build();

        // Route compute-preferred nodes to computeFrameInputs so their cmdList is the compute CL.
        const bool preferCompute = (renderPass.PreferredQueue() == CommandQueueType::Compute);
        RenderGraphExecuteContext nodeContext = boundContext;
        if (preferCompute && boundContext.computeFrameInputs != nullptr) {
            nodeContext.frameInputs = boundContext.computeFrameInputs;
        }

        const ExecuteCallback execute = [&renderPass, nodeContext, requirements]() -> bool {
            const RenderPassContextView contextView = nodeContext.CreateContextView(requirements);
            if (!contextView.IsSatisfied()) {
                DebugLog("RenderGraph::AddNode: runtime requirements are not satisfied.\n");
                return false;
            }
            return renderPass.Execute(contextView);
        };

        std::string passName = "Node";
        const std::string_view tag = renderPass.Tag();
        if (!tag.empty()) {
            passName.assign(tag.begin(), tag.end());
        }
        passName += "_" + std::to_string(m_passes.size());

        const PhaseHandle phase = FindOrAddPhase(renderPass.PhaseTag());
        NodeHandle node = AddPassInternal(passName, execute, phase);
        if (!node.IsValid()) {
            return {};
        }
        // Tag the pass with its preferred queue for level-based async dispatch.
        m_passes[node.index].preferCompute = preferCompute;
        if (phase.IsValid() && phase.index < m_phases.size()) {
            ++m_phases[phase.index].passCount;
        }

        RenderGraphBuilder builder(*this, m_resources, node, previousNode);
        renderPass.Setup(builder);
        return node;
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

    void RenderGraph::Read(NodeHandle node, ResourceHandle resource)
    {
        if (!node.IsValid() || node.index >= m_passes.size() || !resource.IsValid()) {
            return;
        }
        m_passes[node.index].reads.push_back(resource);
    }

    void RenderGraph::Write(NodeHandle node, ResourceHandle resource)
    {
        if (!node.IsValid() || node.index >= m_passes.size() || !resource.IsValid()) {
            return;
        }
        m_passes[node.index].writes.push_back(resource);
    }

    void RenderGraph::ReadAsCopySource(NodeHandle node, ResourceHandle resource)
    {
        if (!node.IsValid() || node.index >= m_passes.size() || !resource.IsValid()) {
            return;
        }
        // Registered as a read as well, so ordering and hazard analysis treat it like any other
        // read; copySources only redirects the state PreparePassResources asks for.
        m_passes[node.index].reads.push_back(resource);
        m_passes[node.index].copySources.push_back(resource);
    }

    void RenderGraph::WriteAsCopyDest(NodeHandle node, ResourceHandle resource)
    {
        if (!node.IsValid() || node.index >= m_passes.size() || !resource.IsValid()) {
            return;
        }
        m_passes[node.index].writes.push_back(resource);
        m_passes[node.index].copyDests.push_back(resource);
    }

    void RenderGraph::UseColorTarget(NodeHandle node, ResourceHandle resource)
    {
        if (!node.IsValid() || node.index >= m_passes.size() || !resource.IsValid()) {
            return;
        }
        m_passes[node.index].colorTargets.push_back(resource);
    }

    void RenderGraph::UseDepthTarget(NodeHandle node, ResourceHandle resource)
    {
        if (!node.IsValid() || node.index >= m_passes.size() || !resource.IsValid()) {
            return;
        }
        m_passes[node.index].depthTarget = resource;
    }

    void RenderGraph::DependsOn(NodeHandle node, NodeHandle dependency)
    {
        if (!node.IsValid() ||
            !dependency.IsValid() ||
            node.index >= m_passes.size() ||
            dependency.index >= m_passes.size()) {
            return;
        }
        m_passes[node.index].explicitDependencies.push_back(dependency.index);
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

        // Phase order is a render-pass-level dependency. Nodes in the same phase
        // are ordered by explicit node dependencies and resource hazards only.
        for (size_t i = 0; i < passCount; ++i) {
            for (size_t j = i + 1; j < passCount; ++j) {
                if (m_passes[i].phaseIndex < m_passes[j].phaseIndex) {
                    addEdge(i, j);
                } else if (m_passes[j].phaseIndex < m_passes[i].phaseIndex) {
                    addEdge(j, i);
                }
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
        return m_executeContext.frameInputs->execution.cmdList;
    }

    IRhiCommandEncoder* RenderGraph::GetCommandEncoder() const
    {
        if (!m_hasExecuteContext || !m_executeContext.frameInputs) {
            return nullptr;
        }
        return m_executeContext.frameInputs->execution.commandEncoder;
    }

    bool RenderGraph::TransitionResource(ResourceRegistry::ResourceRecord& resource,
                                         D3D12_RESOURCE_STATES requiredState)
    {
        if (!resource.hasExternalResource) {
            return false;
        }

        if (resource.rhiResource.IsValid()) {
            const RhiResourceState requiredRhiState = ToRhiResourceState(requiredState);

            // Record what the RHI actually leaves the resource in, not what was asked for. The
            // round trip is lossy: a request for PIXEL_SHADER_RESOURCE (0x80) maps to
            // RhiResourceState::ShaderResource, which the RHI realises as NON_PIXEL|PIXEL (0xC0).
            // Storing the request meant currentState claimed 0x80 for a resource genuinely in
            // 0xC0, and any later raw D3D12 barrier built from currentState then declared a
            // Before state the resource had never held -- the debug layer's id=527.
            const D3D12_RESOURCE_STATES realisedState = ToRealisedD3D12State(requiredRhiState);

            if (resource.rhiCurrentState == requiredRhiState) {
                resource.currentState = realisedState;
                return true;
            }

            IRhiCommandEncoder* encoder = GetCommandEncoder();
            if (!encoder) {
                return false;
            }

            RhiResourceTransitionDesc transition{};
            transition.resource = resource.rhiResource;
            transition.before = resource.rhiCurrentState;
            transition.after = requiredRhiState;
            encoder->TransitionResources(&transition, 1);
            resource.rhiCurrentState = requiredRhiState;
            resource.currentState = realisedState;
            return true;
        }

        if (resource.resource == nullptr) {
            return false;
        }

        if (resource.currentState == requiredState) {
            return true;
        }

        CommandList* cmdList = GetCommandList();
        if (!cmdList) {
            return false;
        }

        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            resource.resource, resource.currentState, requiredState);
        cmdList->ResourceBarrier(1, &barrier);
        resource.currentState = requiredState;
        return true;
    }

    bool RenderGraph::PreparePassResources(const PassNode& pass)
    {
        if (pass.reads.empty() && pass.copyDests.empty() && pass.colorTargets.empty() && !pass.depthTarget.IsValid()) {
            return true;
        }

        CommandList* cmdList = GetCommandList();
        IRhiCommandEncoder* encoder = GetCommandEncoder();
        if (!cmdList && !encoder) {
            DebugLog("RenderGraph::PreparePassResources: command list and RHI encoder are null.\n");
            return false;
        }

        // Transition read-only resources to PIXEL_SHADER_RESOURCE state.
        // Skip any resource that is also bound as a color or depth target in this pass.
        for (const ResourceHandle handle : pass.reads) {
            ResourceRegistry::ResourceRecord* resource = m_resources.GetMutable(handle);
            if (!resource || !resource->hasExternalResource ||
                (!resource->rhiResource.IsValid() && resource->resource == nullptr)) {
                continue;
            }
            const bool isColorTarget = std::find(
                pass.colorTargets.begin(), pass.colorTargets.end(), handle) != pass.colorTargets.end();
            const bool isDepthTarget = (pass.depthTarget == handle);
            if (isColorTarget || isDepthTarget) {
                continue;
            }
            const bool isCopySource = std::find(
                pass.copySources.begin(), pass.copySources.end(), handle) != pass.copySources.end();
            TransitionResource(*resource, isCopySource ? D3D12_RESOURCE_STATE_COPY_SOURCE
                                                       : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        // Copy destinations are the one kind of write the graph transitions itself. Ordinary
        // writes are dependency-only because the state they need depends on how the pass binds
        // them, but a CopyResource destination always needs COPY_DEST.
        for (const ResourceHandle handle : pass.copyDests) {
            ResourceRegistry::ResourceRecord* resource = m_resources.GetMutable(handle);
            if (!resource || !resource->hasExternalResource ||
                (!resource->rhiResource.IsValid() && resource->resource == nullptr)) {
                continue;
            }
            TransitionResource(*resource, D3D12_RESOURCE_STATE_COPY_DEST);
        }

        const bool useRhiTargets = encoder != nullptr && cmdList == nullptr;
        std::vector<CpuDescriptorHandle> rtvs;
        std::vector<RhiCpuDescriptorHandle> rhiRtvs;
        rtvs.reserve(pass.colorTargets.size());
        rhiRtvs.reserve(pass.colorTargets.size());
        for (const ResourceHandle handle : pass.colorTargets) {
            ResourceRegistry::ResourceRecord* resource = m_resources.GetMutable(handle);
            if (!resource || !resource->hasExternalResource) {
                DebugLog("RenderGraph::PreparePassResources: color target import is missing.\n");
                return false;
            }
            if (useRhiTargets) {
                if (!resource->rhiResource.IsValid() || !resource->rhiRtv.IsValid()) {
                    DebugLog("RenderGraph::PreparePassResources: RHI color target import is missing.\n");
                    return false;
                }
            } else if (resource->resource == nullptr || !resource->hasRtv) {
                DebugLog("RenderGraph::PreparePassResources: D3D12 color target import is missing.\n");
                return false;
            }
            if (!TransitionResource(*resource, D3D12_RESOURCE_STATE_RENDER_TARGET)) {
                DebugLog("RenderGraph::PreparePassResources: failed to transition color target.\n");
                return false;
            }
            if (useRhiTargets) {
                rhiRtvs.push_back(resource->rhiRtv);
            } else {
                rtvs.push_back(resource->rtv);
            }
        }

        CpuDescriptorHandle dsv{};
        CpuDescriptorHandle* dsvPtr = nullptr;
        RhiCpuDescriptorHandle rhiDsv{};
        RhiCpuDescriptorHandle* rhiDsvPtr = nullptr;
        if (pass.depthTarget.IsValid()) {
            ResourceRegistry::ResourceRecord* resource = m_resources.GetMutable(pass.depthTarget);
            if (!resource || !resource->hasExternalResource) {
                DebugLog("RenderGraph::PreparePassResources: depth target import is missing.\n");
                return false;
            }
            if (useRhiTargets) {
                if (!resource->rhiResource.IsValid() || !resource->rhiDsv.IsValid()) {
                    DebugLog("RenderGraph::PreparePassResources: RHI depth target import is missing.\n");
                    return false;
                }
            } else if (resource->resource == nullptr || !resource->hasDsv) {
                DebugLog("RenderGraph::PreparePassResources: D3D12 depth target import is missing.\n");
                return false;
            }
            if (!TransitionResource(*resource, D3D12_RESOURCE_STATE_DEPTH_WRITE)) {
                DebugLog("RenderGraph::PreparePassResources: failed to transition depth target.\n");
                return false;
            }
            if (useRhiTargets) {
                rhiDsv = resource->rhiDsv;
                rhiDsvPtr = &rhiDsv;
            } else {
                dsv = resource->dsv;
                dsvPtr = &dsv;
            }
        }

        if (useRhiTargets) {
            encoder->SetRenderTargets(static_cast<uint32_t>(rhiRtvs.size()),
                                      rhiRtvs.empty() ? nullptr : rhiRtvs.data(),
                                      rhiDsvPtr);
        } else {
            cmdList->OMSetRenderTargets(static_cast<UINT>(rtvs.size()),
                                        rtvs.empty() ? nullptr : rtvs.data(),
                                        FALSE,
                                        dsvPtr);
        }

        for (const ResourceHandle handle : pass.colorTargets) {
            ResourceRegistry::ResourceRecord* resource = m_resources.GetMutable(handle);
            if (resource && resource->clearColorOnFirstUse && !resource->colorCleared) {
                if (useRhiTargets) {
                    const RhiClearColor clearColor{
                        resource->clearColor[0],
                        resource->clearColor[1],
                        resource->clearColor[2],
                        resource->clearColor[3],
                    };
                    encoder->ClearRenderTarget(resource->rhiRtv, clearColor);
                } else {
                    cmdList->ClearRenderTargetView(resource->rtv, resource->clearColor, 0, nullptr);
                }
                resource->colorCleared = true;
            }
        }

        if (pass.depthTarget.IsValid()) {
            ResourceRegistry::ResourceRecord* resource = m_resources.GetMutable(pass.depthTarget);
            if (resource && resource->clearDepthOnFirstUse && !resource->depthCleared) {
                if (useRhiTargets) {
                    encoder->ClearDepthStencil(resource->rhiDsv, resource->clearDepth, resource->clearStencil);
                } else {
                    cmdList->ClearDepthStencilView(resource->dsv,
                                                   D3D12_CLEAR_FLAG_DEPTH,
                                                   resource->clearDepth,
                                                   resource->clearStencil,
                                                   0,
                                                   nullptr);
                }
                resource->depthCleared = true;
            }
        }

        return true;
    }

    void RenderGraph::FinalizeExternalResources()
    {
        CommandList* cmdList = GetCommandList();
        IRhiCommandEncoder* encoder = GetCommandEncoder();
        if (!cmdList && !encoder) {
            return;
        }

        for (ResourceRegistry::ResourceRecord& resource : m_resources.MutableRecords()) {
            if (!resource.hasExternalResource || !resource.transitionToFinalState ||
                (!resource.rhiResource.IsValid() && resource.resource == nullptr)) {
                continue;
            }
            (void)TransitionResource(resource, resource.finalState);
        }
    }

    std::vector<std::vector<size_t>> RenderGraph::BuildExecutionLevels(
        const std::vector<size_t>& topoOrder) const
    {
        // Assign each node to a level = longest path from any root node to this node.
        // Nodes at the same level have no dependency on each other and can run in parallel.
        const size_t passCount = m_passes.size();
        std::vector<int> level(passCount, 0);

        // Build adjacency (from ↁEto) mirroring BuildExecutionOrder's logic.
        // We need: for each node, what is the max level of its predecessors?
        // Since topoOrder is already sorted, iterate in topo order and propagate.

        // First map node index ↁEits position in topoOrder (to detect predecessors).
        std::vector<size_t> posInTopo(passCount, 0);
        for (size_t i = 0; i < topoOrder.size(); ++i) {
            posInTopo[topoOrder[i]] = i;
        }

        // For each node in topo order, update its level from explicit deps.
        for (size_t i = 0; i < topoOrder.size(); ++i) {
            const size_t nodeIdx = topoOrder[i];
            for (const size_t dep : m_passes[nodeIdx].explicitDependencies) {
                if (dep < passCount) {
                    level[nodeIdx] = (std::max)(level[nodeIdx], level[dep] + 1);
                }
            }
            // Also propagate from resource-conflict predecessors (earlier nodes that write
            // resources we read/write). Iterate previous nodes in topoOrder.
            for (size_t j = 0; j < i; ++j) {
                const size_t prevIdx = topoOrder[j];
                if (m_passes[prevIdx].phaseIndex < m_passes[nodeIdx].phaseIndex) {
                    level[nodeIdx] = (std::max)(level[nodeIdx], level[prevIdx] + 1);
                    continue;
                }

                bool hasConflict = false;
                for (const ResourceHandle w : m_passes[prevIdx].writes) {
                    if (!w.IsValid()) continue;
                    for (const ResourceHandle r : m_passes[nodeIdx].reads) {
                        if (r == w) { hasConflict = true; break; }
                    }
                    if (hasConflict) break;
                    for (const ResourceHandle w2 : m_passes[nodeIdx].writes) {
                        if (w2 == w) { hasConflict = true; break; }
                    }
                    if (hasConflict) break;
                }
                if (hasConflict) {
                    level[nodeIdx] = (std::max)(level[nodeIdx], level[prevIdx] + 1);
                }
            }
        }

        // Group nodes by level.
        int maxLevel = 0;
        for (size_t nodeIdx : topoOrder) {
            maxLevel = (std::max)(maxLevel, level[nodeIdx]);
        }
        std::vector<std::vector<size_t>> levels(static_cast<size_t>(maxLevel) + 1u);
        for (size_t nodeIdx : topoOrder) {
            levels[static_cast<size_t>(level[nodeIdx])].push_back(nodeIdx);
        }
        return levels;
    }

    bool RenderGraph::Execute()
    {
        Profiler::ScopedCpuEvent graphEvent("RenderGraph::Execute");

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

        // Async compute context (may be null if no compute queue is available).
        // submitCurrentGraphicsNode/acquireNextGraphicsNode must also be present:
        // without them there is no way to Signal work that has actually been
        // submitted (a single frame-long command list is not closed/submitted
        // until frame end), so the cross-queue join would guard nothing.
        const bool asyncComputeEnabled =
            m_hasExecuteContext &&
            m_executeContext.computeFrameInputs != nullptr &&
            m_executeContext.computeQueueRaw    != nullptr &&
            m_executeContext.graphicsQueueRaw   != nullptr &&
            m_executeContext.crossQueueFence    != nullptr &&
            m_executeContext.crossQueueFenceVal != nullptr &&
            static_cast<bool>(m_executeContext.submitCurrentGraphicsNode) &&
            static_cast<bool>(m_executeContext.acquireNextGraphicsNode);

        // Build execution levels for parallel/async dispatch.
        const std::vector<std::vector<size_t>> levels = BuildExecutionLevels(executionOrder);

        auto executePass = [&](const PassNode& pass) -> bool {
            if (!pass.execute) {
                DebugLog("RenderGraph::Execute: pass execute callback is null.\n");
                return false;
            }

            CommandList* profileCmdList = nullptr;
            if (pass.preferCompute) {
                profileCmdList = (m_hasExecuteContext && m_executeContext.computeFrameInputs)
                    ? m_executeContext.computeFrameInputs->execution.cmdList
                    : GetCommandList();
            } else {
                profileCmdList = GetCommandList();
            }
            Profiler::ScopedCpuEvent cpuEvent(pass.name.c_str());
            Profiler::ScopedGpuEvent gpuEvent(profileCmdList, pass.name.c_str());

            {
            // Timestamps are only taken for graphics passes. A compute pass records to the
            // compute queue, whose timestamps come from a different clock domain than the
            // graphics queue that owns the query heap and resolve, so timing them here would
            // produce meaningless deltas. No pass currently returns CommandQueueType::Compute.
            ScopedGpuTimestamp gpuTimestamp(
                (!pass.preferCompute && m_hasExecuteContext) ? m_executeContext.gpuTimestampProfiler : nullptr,
                profileCmdList ? profileCmdList->Get() : nullptr,
                pass.name);

            // Compute-preferred passes bypass RTV/DSV setup (no rasterization).
            // NOTE: this also means compute passes get no resource-state transition
            // handoff at all. Cross-queue resource-state transitions (e.g. a resource
            // written by a graphics pass and read by a compute pass, or vice versa)
            // are NOT implemented anywhere in RenderGraph yet. The Signal/Wait pair
            // around the compute dispatch below only orders GPU timelines; it does
            // NOT insert the barriers a resource would need to safely cross queues.
            // Until that handoff exists, opting a real pass into preferCompute is
            // unsafe beyond passes that touch no resource shared with the graphics
            // queue in the same frame.
            if (!pass.preferCompute) {
                if (!PreparePassResources(pass)) {
                    DebugLog("RenderGraph::Execute: failed to prepare pass resources.\n");
                    return false;
                }
            }
#if defined(_DEBUG)
            const unsigned int debugDrawCountBefore = DebugGetDrawCount();
            const unsigned int debugDispatchCountBefore = DebugGetDispatchCount();
#endif
            if (!pass.execute()) {
                std::string message = "RenderGraph::Execute: pass execution failed: ";
                message += pass.name;
                message += "\n";
                DebugLog(message.c_str());
                return false;
            }
#if defined(_DEBUG)
            const unsigned int debugDrawCountAfter = DebugGetDrawCount();
            if (debugDrawCountAfter > debugDrawCountBefore) {
                char drawRangeBuf[256];
                std::snprintf(drawRangeBuf, sizeof(drawRangeBuf), "[DrawRange] %s: draws [%u, %u)\n",
                              pass.name.c_str(), debugDrawCountBefore, debugDrawCountAfter);
                DebugLog(drawRangeBuf);
            }
            const unsigned int debugDispatchCountAfter = DebugGetDispatchCount();
            if (debugDispatchCountAfter > debugDispatchCountBefore) {
                char dispatchRangeBuf[256];
                std::snprintf(dispatchRangeBuf, sizeof(dispatchRangeBuf), "[DispatchRange] %s: dispatches [%u, %u)\n",
                              pass.name.c_str(), debugDispatchCountBefore, debugDispatchCountAfter);
                DebugLog(dispatchRangeBuf);
            }
#endif
            } // end timestamp scope -- phase completion nodes below run their own GPU work
              // and must not be billed to this pass.

            if (pass.phaseIndex < m_phases.size() && !phaseCompletionFired[pass.phaseIndex]) {
                const size_t completeCount = ++completedPassCountByPhase[pass.phaseIndex];
                const size_t phasePassCount = m_phases[pass.phaseIndex].passCount;
                if (phasePassCount > 0u && completeCount >= phasePassCount) {
                    if (!runPhaseCompletionNodes(pass.phaseIndex, PhaseCompletionMode::Deterministic)) {
                        return false;
                    }
                    runPhaseCompletionNodes(pass.phaseIndex, PhaseCompletionMode::Opportunistic);
                    phaseCompletionFired[pass.phaseIndex] = true;
                }
            }
            return true;
        };

        if (executeSucceeded) {
            for (size_t levelIdx = 0; levelIdx < levels.size() && executeSucceeded; ++levelIdx) {
                const auto& levelNodes = levels[levelIdx];

                bool levelHasCompute  = false;
                bool levelHasGraphics = false;
                for (const size_t passIdx : levelNodes) {
                    levelHasCompute  |= m_passes[passIdx].preferCompute;
                    levelHasGraphics |= !m_passes[passIdx].preferCompute;
                }

                // ---- Execute graphics nodes for this level ----
                for (const size_t passIdx : levelNodes) {
                    const auto& pass = m_passes[passIdx];
                    if (!pass.preferCompute) {
                        if (!executePass(pass)) {
                            executeSucceeded = false;
                            break;
                        }
                    }
                }
                if (!executeSucceeded) break;

                // ---- Cross-queue sync and compute dispatch ----
                if (levelHasCompute && asyncComputeEnabled) {
                    // Close and submit the graphics command list holding this Node's
                    // work, and open a fresh one for the next Node. Without this, the
                    // Signal below would refer to work that has not actually been
                    // submitted to the queue yet (it would resolve against whatever
                    // was submitted by a previous Execute() call, guarding nothing).
                    if (!m_executeContext.submitCurrentGraphicsNode()) {
                        DebugLog("RenderGraph::Execute: failed to submit graphics node before cross-queue join.\n");
                        executeSucceeded = false;
                        break;
                    }

                    // Graphics queue signals that its work for this level is done.
                    // Compute queue waits before starting its work.
                    UINT64& fenceVal = *m_executeContext.crossQueueFenceVal;
                    ++fenceVal;
                    m_executeContext.graphicsQueueRaw->Signal(m_executeContext.crossQueueFence, fenceVal);
                    m_executeContext.computeQueueRaw->Wait(m_executeContext.crossQueueFence, fenceVal);

                    // Execute compute nodes (recorded to computeCmdList via lambda capture).
                    for (const size_t passIdx : levelNodes) {
                        const auto& pass = m_passes[passIdx];
                        if (pass.preferCompute) {
                            if (!executePass(pass)) {
                                executeSucceeded = false;
                                break;
                            }
                        }
                    }
                    if (!executeSucceeded) break;

                    // Compute signals that its work for this level is done.
                    // Next level's graphics work must wait before reading compute output.
                    ++fenceVal;
                    m_executeContext.computeQueueRaw->Signal(m_executeContext.crossQueueFence, fenceVal);
                    m_executeContext.graphicsQueueRaw->Wait(m_executeContext.crossQueueFence, fenceVal);

                    // Open the next Node's graphics command list now that the queue has
                    // been told to wait for the compute signal; *frameInputs is updated
                    // in place so already-registered passes pick up the new command list.
                    if (!m_executeContext.acquireNextGraphicsNode()) {
                        DebugLog("RenderGraph::Execute: failed to acquire next graphics node after cross-queue join.\n");
                        executeSucceeded = false;
                        break;
                    }
                } else if (levelHasCompute) {
                    // No async compute queue: fall back to sequential execution on graphics.
                    for (const size_t passIdx : levelNodes) {
                        const auto& pass = m_passes[passIdx];
                        if (pass.preferCompute) {
                            if (!executePass(pass)) {
                                executeSucceeded = false;
                                break;
                            }
                        }
                    }
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
            Profiler::ScopedCpuEvent cpuEvent(node.name.c_str());
            // Completion nodes run outside every pass's timestamp scope, so they need their own
            // or their GPU work (e.g. SWRT AO) is missing from render.gputime.
            CommandList* profileCmdList = GetCommandList();
            ScopedGpuTimestamp gpuTimestamp(
                m_hasExecuteContext ? m_executeContext.gpuTimestampProfiler : nullptr,
                profileCmdList ? profileCmdList->Get() : nullptr,
                node.name);
            if (!node.execute()) {
                DebugLog("RenderGraph::ExecutePhaseCompletionNodes: completion node execution failed.\n");
                return false;
            }
        }
        return true;
    }

    RenderGraphBuilder::RenderGraphBuilder(RenderGraph& renderGraph,
                                           ResourceRegistry& resources,
                                           RenderGraph::NodeHandle pass,
                                           RenderGraph::NodeHandle previousPass)
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

    ResourceHandle RenderGraphBuilder::ReadAsCopySource(std::string_view resourceName)
    {
        const ResourceHandle handle = Import(resourceName);
        if (m_renderGraph == nullptr || !m_pass.IsValid() || !handle.IsValid()) {
            return {};
        }
        m_renderGraph->ReadAsCopySource(m_pass, handle);
        return handle;
    }

    ResourceHandle RenderGraphBuilder::WriteAsCopyDest(std::string_view resourceName)
    {
        const ResourceHandle handle = Import(resourceName);
        if (m_renderGraph == nullptr || !m_pass.IsValid() || !handle.IsValid()) {
            return {};
        }
        m_renderGraph->WriteAsCopyDest(m_pass, handle);
        return handle;
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

    RenderPassContextView RenderGraphExecuteContext::CreateContextView(const RenderPassRequirements& requirements) const
    {
        return RenderPassContextView(executionPolicy, frameInputs, executionServices, requirements);
    }

}
