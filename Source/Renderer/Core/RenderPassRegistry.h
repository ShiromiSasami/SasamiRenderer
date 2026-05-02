#pragma once
#include "Renderer/Core/RenderGraph.h"
#include "Renderer/Core/RenderNodeConstants.h"
#include "Renderer/Passes/IRenderNode.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"
#include "Renderer/Structures/RendererEnums.h"
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace SasamiRenderer
{
    class RenderPassRegistry
    {
    public:
        using PhaseCompletionMode     = RenderGraph::PhaseCompletionMode;
        using PhaseCompletionCallback = std::function<bool(const RenderNodeContextView&)>;
        using RenderNodeType          = RendererEnums::RenderNodeType;
        using RenderPathMode          = RendererEnums::RenderPathMode;

        struct PassHandle
        {
            size_t index = static_cast<size_t>(-1);
            bool IsValid() const { return index != static_cast<size_t>(-1); }
        };

        static constexpr size_t kBuiltinNodeCount =
            static_cast<size_t>(RendererEnums::RenderNodeType::SdfFluid) + 1u;

        void SetBuiltinNodes(std::array<std::shared_ptr<IRenderNode>, kBuiltinNodeCount> nodes);

        PassHandle AddPass(const std::shared_ptr<IRenderNode>& renderPass);
        PassHandle AddPassBefore(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass);
        PassHandle AddPassAfter(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass);
        bool ReplacePass(std::string_view targetTag, const std::shared_ptr<IRenderNode>& renderPass);

        bool AddPhaseCompletionNode(std::string_view phaseTag,
                                    std::string_view nodeName,
                                    const PhaseCompletionCallback& execute,
                                    PhaseCompletionMode mode,
                                    const RenderNodeRequirements& requirements);
        void ClearPhaseCompletionNodes();
        void ClearPasses();
        void SetRenderNodeSequence(const std::vector<RenderNodeType>& sequence);

        const std::vector<RenderNodeType>&              GetRenderNodeSequence() const { return m_renderNodeQueueTypes; }
        const std::vector<std::shared_ptr<IRenderNode>>& GetPasses()            const { return m_renderPasses; }

        bool RegisterPassesToRenderGraph(RenderGraph& renderGraph,
                                         const RenderGraphExecuteContext& executeContext,
                                         RenderPathMode pathMode,
                                         const std::shared_ptr<IRenderNode>& rayTracingNode) const;

        void RegisterPhaseCompletionNodesToRenderGraph(RenderGraph& renderGraph,
                                                       const RenderGraphExecuteContext& executeContext) const;

    private:
        struct PhaseCompletionNodeEntry
        {
            std::string phaseTag;
            std::string nodeName;
            PhaseCompletionMode mode = PhaseCompletionMode::Deterministic;
            RenderNodeRequirements requirements{};
            PhaseCompletionCallback execute;
        };

        PassHandle InsertPassAt(size_t insertIndex, const std::shared_ptr<IRenderNode>& renderPass);
        size_t FindPassIndexByTag(std::string_view targetTag) const;

        std::array<std::shared_ptr<IRenderNode>, kBuiltinNodeCount> m_builtinNodes{};
        std::vector<std::shared_ptr<IRenderNode>> m_renderPasses;
        std::vector<PhaseCompletionNodeEntry>     m_phaseCompletionNodes;
        std::vector<RenderNodeType>               m_renderNodeQueueTypes;
    };
}
