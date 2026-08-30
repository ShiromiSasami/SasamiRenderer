#define NOMINMAX
#include "Renderer/Passes/Core/RenderPassRegistry.h"
#include "Foundation/Tools/DebugOutput.h"

namespace SasamiRenderer
{
    void RenderPassRegistry::SetBuiltinPasses(std::array<std::shared_ptr<IRenderPass>, kBuiltinPassCount> passes)
    {
        m_builtinPasses = std::move(passes);
    }

    std::vector<std::shared_ptr<IRenderPass>> RenderPassRegistry::BuildPassesFromSequence(
        const std::vector<RenderPassType>& sequence,
        bool* allValid) const
    {
        if (allValid) {
            *allValid = true;
        }

        std::vector<std::shared_ptr<IRenderPass>> passes;
        passes.reserve(sequence.size());
        for (const RenderPassType passType : sequence) {
            const size_t passIndex = static_cast<size_t>(passType);
            if (passIndex >= m_builtinPasses.size() || m_builtinPasses[passIndex] == nullptr) {
                if (allValid) {
                    *allValid = false;
                }
                DebugLog("RenderPassRegistry::BuildPassesFromSequence: invalid pass type is ignored.\n");
                continue;
            }
            passes.push_back(m_builtinPasses[passIndex]);
        }
        return passes;
    }

    void RenderPassRegistry::RebuildPassCache()
    {
        m_renderPasses.clear();
        for (const auto& node : m_renderNodes) {
            if (node) {
                node->AppendPasses(m_renderPasses);
            }
        }
    }

    void RenderPassRegistry::ReplaceWithCustomPassSequence(std::vector<std::shared_ptr<IRenderPass>> passes)
    {
        m_renderNodes.clear();
        if (!passes.empty()) {
            m_renderNodes.push_back(std::make_shared<RenderPassSequenceNode>("CustomRenderNode", std::move(passes)));
        }
        RebuildPassCache();
    }

    void RenderPassRegistry::UseDefaultRenderNodePreset()
    {
        SetRenderPassSequence(std::vector<RenderPassType>(RenderPassConstants::kDefaultRenderPathSequence.begin(),
                                                          RenderPassConstants::kDefaultRenderPathSequence.end()));
        if (!m_renderPasses.empty()) {
            m_renderNodes.clear();
            m_renderNodes.push_back(std::make_shared<RenderPassSequenceNode>("DefaultRenderNode", m_renderPasses));
            RebuildPassCache();
        }
    }

    void RenderPassRegistry::SetRenderNodePreset(std::shared_ptr<IRenderNode> node)
    {
        ClearPasses();
        if (node) {
            m_renderNodes.push_back(std::move(node));
        }
        RebuildPassCache();
    }

    RenderPassRegistry::NodeHandle RenderPassRegistry::AddNode(const std::shared_ptr<IRenderNode>& renderNode)
    {
        if (!renderNode) {
            return {};
        }
        m_renderNodes.push_back(renderNode);
        RebuildPassCache();
        return NodeHandle{ m_renderNodes.size() - 1u };
    }

    RenderPassRegistry::PassHandle RenderPassRegistry::InsertPassAt(size_t insertIndex, const std::shared_ptr<IRenderPass>& renderPass)
    {
        if (!renderPass) {
            return {};
        }

        if (insertIndex > m_renderPasses.size()) {
            insertIndex = m_renderPasses.size();
        }

        std::vector<std::shared_ptr<IRenderPass>> passes = m_renderPasses;
        passes.insert(passes.begin() + insertIndex, renderPass);
        ReplaceWithCustomPassSequence(std::move(passes));
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

    RenderPassRegistry::PassHandle RenderPassRegistry::AddPass(const std::shared_ptr<IRenderPass>& renderPass)
    {
        return InsertPassAt(m_renderPasses.size(), renderPass);
    }

    RenderPassRegistry::PassHandle RenderPassRegistry::AddPassBefore(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass)
    {
        const size_t targetIndex = FindPassIndexByTag(targetTag);
        if (targetIndex == static_cast<size_t>(-1)) {
            return {};
        }
        return InsertPassAt(targetIndex, renderPass);
    }

    RenderPassRegistry::PassHandle RenderPassRegistry::AddPassAfter(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass)
    {
        const size_t targetIndex = FindPassIndexByTag(targetTag);
        if (targetIndex == static_cast<size_t>(-1)) {
            return {};
        }
        return InsertPassAt(targetIndex + 1u, renderPass);
    }

    bool RenderPassRegistry::ReplacePass(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass)
    {
        if (!renderPass) {
            return false;
        }
        const size_t targetIndex = FindPassIndexByTag(targetTag);
        if (targetIndex == static_cast<size_t>(-1)) {
            return false;
        }
        std::vector<std::shared_ptr<IRenderPass>> passes = m_renderPasses;
        passes[targetIndex] = renderPass;
        ReplaceWithCustomPassSequence(std::move(passes));
        return true;
    }

    bool RenderPassRegistry::AddPhaseCompletionNode(std::string_view phaseTag,
                                                    std::string_view nodeName,
                                                    const PhaseCompletionCallback& execute,
                                                    PhaseCompletionMode mode,
                                                    const RenderPassRequirements& requirements)
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
        m_renderNodes.clear();
        m_renderPasses.clear();
        m_renderPassQueueTypes.clear();
    }

    void RenderPassRegistry::SetRenderPassSequence(const std::vector<RenderPassType>& sequence)
    {
        if (sequence.empty()) {
            DebugLog("Renderer::SetRenderPassSequence: empty sequence is ignored.\n");
            return;
        }

        bool allValid = true;
        std::vector<std::shared_ptr<IRenderPass>> passes = BuildPassesFromSequence(sequence, &allValid);
        if (passes.empty()) {
            DebugLog("Renderer::SetRenderPassSequence: no valid pass was found.\n");
            return;
        }
        m_renderPassQueueTypes = sequence;
        if (!allValid) {
            m_renderPassQueueTypes.clear();
        }
        ReplaceWithCustomPassSequence(std::move(passes));
    }

    bool RenderPassRegistry::RegisterPassesToRenderGraph(RenderGraph& renderGraph,
                                                         const RenderGraphExecuteContext& executeContext,
                                                         RenderPathMode pathMode,
                                                         const std::shared_ptr<IRenderPass>& rayTracingPass) const
    {
        const std::vector<std::shared_ptr<IRenderPass>> runtimePasses =
            (pathMode == RenderPathMode::HardwareRayTracing)
                ? std::vector<std::shared_ptr<IRenderPass>>{ rayTracingPass }
                : m_renderPasses;

        std::vector<RenderGraph::NodeHandle> orderedNodes;
        orderedNodes.reserve(runtimePasses.size());

        for (const auto& runtimePass : runtimePasses) {
            if (!runtimePass) {
                DebugLog("RenderPassRegistry::RegisterPassesToRenderGraph: runtime pass is null.\n");
                continue;
            }

            const RenderGraph::NodeHandle previousNode =
                orderedNodes.empty() ? RenderGraph::NodeHandle{} : orderedNodes.back();
            RenderGraph::NodeHandle node = renderGraph.AddNode(*runtimePass, executeContext, previousNode);
            if (!node.IsValid()) {
                continue;
            }
            orderedNodes.push_back(node);
        }

        if (orderedNodes.empty()) {
            DebugLog("RenderPassRegistry::RegisterPassesToRenderGraph: no executable runtime passes were registered.\n");
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
                    const RenderPassContextView contextView = executeContext.CreateContextView(entry.requirements);
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
