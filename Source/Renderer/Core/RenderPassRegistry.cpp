#define NOMINMAX
#include "Renderer/Core/RenderPassRegistry.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void RenderPassRegistry::SetBuiltinNodes(std::array<std::shared_ptr<IRenderNode>, kBuiltinNodeCount> nodes)
    {
        m_builtinNodes = std::move(nodes);
    }

    RenderPassRegistry::PassHandle RenderPassRegistry::InsertPassAt(size_t insertIndex, const std::shared_ptr<IRenderNode>& renderPass)
    {
        if (!renderPass) {
            return {};
        }

        if (insertIndex > m_renderPasses.size()) {
            insertIndex = m_renderPasses.size();
        }

        m_renderPasses.insert(m_renderPasses.begin() + insertIndex, renderPass);
        return PassHandle{ insertIndex };
    }

    size_t RenderPassRegistry::FindPassIndexByTag(std::string_view targetTag) const
    {
        if (targetTag.empty()) {
            return static_cast<size_t>(-1);
        }

        for (size_t i = 0; i < m_renderPasses.size(); ++i) {
            const auto& pass = m_renderPasses[i];
            if (!pass) {
                continue;
            }
            if (pass->Tag() == targetTag) {
                return i;
            }
        }
        return static_cast<size_t>(-1);
    }

    RenderPassRegistry::PassHandle RenderPassRegistry::AddPass(const std::shared_ptr<IRenderNode>& renderPass)
    {
        return InsertPassAt(m_renderPasses.size(), renderPass);
    }

    RenderPassRegistry::PassHandle RenderPassRegistry::AddPassBefore(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass)
    {
        const size_t targetIndex = FindPassIndexByTag(targetTag);
        if (targetIndex == static_cast<size_t>(-1)) {
            return {};
        }
        return InsertPassAt(targetIndex, renderPass);
    }

    RenderPassRegistry::PassHandle RenderPassRegistry::AddPassAfter(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass)
    {
        const size_t targetIndex = FindPassIndexByTag(targetTag);
        if (targetIndex == static_cast<size_t>(-1)) {
            return {};
        }
        return InsertPassAt(targetIndex + 1u, renderPass);
    }

    bool RenderPassRegistry::ReplacePass(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass)
    {
        if (!renderPass) {
            return false;
        }
        const size_t targetIndex = FindPassIndexByTag(targetTag);
        if (targetIndex == static_cast<size_t>(-1)) {
            return false;
        }
        m_renderPasses[targetIndex] = renderPass;
        return true;
    }

    bool RenderPassRegistry::AddPhaseCompletionNode(std::string_view phaseTag,
                                                    std::string_view nodeName,
                                                    const PhaseCompletionCallback& execute,
                                                    PhaseCompletionMode mode,
                                                    const RenderNodeRequirements& requirements)
    {
        if (phaseTag.empty() || !execute) {
            return false;
        }

        PhaseCompletionNodeEntry entry{};
        entry.phaseTag.assign(phaseTag.begin(), phaseTag.end());
        entry.nodeName.assign(nodeName.begin(), nodeName.end());
        entry.mode = mode;
        entry.requirements = requirements;
        entry.execute = execute;
        m_phaseCompletionNodes.push_back(std::move(entry));
        return true;
    }

    void RenderPassRegistry::ClearPhaseCompletionNodes()
    {
        m_phaseCompletionNodes.clear();
    }

    void RenderPassRegistry::ClearPasses()
    {
        m_renderPasses.clear();
        m_renderNodeQueueTypes.clear();
    }

    void RenderPassRegistry::SetRenderNodeSequence(const std::vector<RenderNodeType>& sequence)
    {
        if (sequence.empty()) {
            DebugLog("Renderer::SetRenderNodeSequence: empty sequence is ignored.\n");
            return;
        }

        ClearPasses();
        m_renderNodeQueueTypes.clear();

        for (const RenderNodeType nodeType : sequence) {
            const size_t nodeIndex = static_cast<size_t>(nodeType);
            if (nodeIndex >= m_builtinNodes.size() || m_builtinNodes[nodeIndex] == nullptr) {
                DebugLog("Renderer::SetRenderNodeSequence: invalid node type is ignored.\n");
                continue;
            }
            AddPass(m_builtinNodes[nodeIndex]);
            m_renderNodeQueueTypes.push_back(nodeType);
        }
    }

    bool RenderPassRegistry::RegisterPassesToRenderGraph(RenderGraph& renderGraph,
                                                         const RenderGraphExecuteContext& executeContext,
                                                         RenderPathMode pathMode,
                                                         const std::shared_ptr<IRenderNode>& rayTracingNode) const
    {
        const std::vector<std::shared_ptr<IRenderNode>> runtimeNodes =
            (pathMode == RenderPathMode::HardwareRayTracing)
                ? std::vector<std::shared_ptr<IRenderNode>>{ rayTracingNode }
                : m_renderPasses;

        std::vector<RenderGraph::NodeHandle> orderedNodes;
        orderedNodes.reserve(runtimeNodes.size());

        for (const auto& runtimeNode : runtimeNodes) {
            if (!runtimeNode) {
                DebugLog("RenderPassRegistry::RegisterPassesToRenderGraph: runtime node is null.\n");
                continue;
            }

            const RenderGraph::NodeHandle previousNode =
                orderedNodes.empty() ? RenderGraph::NodeHandle{} : orderedNodes.back();
            RenderGraph::NodeHandle node = renderGraph.AddNode(*runtimeNode, executeContext, previousNode);
            if (!node.IsValid()) {
                continue;
            }
            orderedNodes.push_back(node);
        }

        if (orderedNodes.empty()) {
            DebugLog("RenderPassRegistry::RegisterPassesToRenderGraph: no executable runtime nodes were registered.\n");
            return false;
        }

        return true;
    }

    void RenderPassRegistry::RegisterPhaseCompletionNodesToRenderGraph(RenderGraph& renderGraph,
                                                                       const RenderGraphExecuteContext& executeContext) const
    {
        for (const PhaseCompletionNodeEntry& entry : m_phaseCompletionNodes) {
            if (!entry.execute) {
                DebugLog("RenderPassRegistry::RegisterPhaseCompletionNodesToRenderGraph: execute callback is null.\n");
                continue;
            }

            const bool registered = renderGraph.AddPhaseCompletionNode(
                entry.phaseTag,
                entry.nodeName,
                [entry, executeContext]() -> bool {
                    const RenderNodeContextView contextView = executeContext.CreateContextView(entry.requirements);
                    if (!contextView.IsSatisfied()) {
                        DebugLog("RenderPassRegistry::RegisterPhaseCompletionNodesToRenderGraph: requirements are not satisfied.\n");
                        return false;
                    }
                    return entry.execute(contextView);
                },
                entry.mode);
            if (!registered) {
                DebugLog("RenderPassRegistry::RegisterPhaseCompletionNodesToRenderGraph: failed to register completion node.\n");
            }
        }
    }
}
