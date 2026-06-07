#pragma once
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/Passes/Core/RenderNode.h"
#include "Renderer/Passes/Core/RenderPassConstants.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"
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
        using PhaseCompletionCallback = std::function<bool(const RenderPassContextView&)>;
        using RenderPassType          = RendererEnums::RenderPassType;
        using RenderPathMode          = RendererEnums::RenderPathMode;

        struct PassHandle
        {
            size_t index = static_cast<size_t>(-1);
            bool IsValid() const { return index != static_cast<size_t>(-1); }
        };

        struct NodeHandle
        {
            size_t index = static_cast<size_t>(-1);
            bool IsValid() const { return index != static_cast<size_t>(-1); }
        };

        static constexpr size_t kBuiltinPassCount =
            static_cast<size_t>(RendererEnums::RenderPassType::SoftwareReflectionComposite) + 1u;

        void SetBuiltinPasses(std::array<std::shared_ptr<IRenderPass>, kBuiltinPassCount> passes);
        void UseDefaultRenderNodePreset();
        void SetRenderNodePreset(std::shared_ptr<IRenderNode> node);

        NodeHandle AddNode(const std::shared_ptr<IRenderNode>& renderNode);
        PassHandle AddPass(const std::shared_ptr<IRenderPass>& renderPass);
        PassHandle AddPassBefore(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass);
        PassHandle AddPassAfter(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass);
        bool ReplacePass(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass);

        bool AddPhaseCompletionNode(std::string_view phaseTag,
                                    std::string_view nodeName,
                                    const PhaseCompletionCallback& execute,
                                    PhaseCompletionMode mode,
                                    const RenderPassRequirements& requirements);
        void ClearPhaseCompletionNodes();
        void ClearPasses();
        void SetRenderPassSequence(const std::vector<RenderPassType>& sequence);

        const std::vector<RenderPassType>&              GetRenderPassSequence() const { return m_renderPassQueueTypes; }
        const std::vector<std::shared_ptr<IRenderPass>>& GetPasses()            const { return m_renderPasses; }
        const std::vector<std::shared_ptr<IRenderNode>>& GetNodes()             const { return m_renderNodes; }

        bool RegisterPassesToRenderGraph(RenderGraph& renderGraph,
                                         const RenderGraphExecuteContext& executeContext,
                                         RenderPathMode pathMode,
                                         const std::shared_ptr<IRenderPass>& rayTracingPass) const;

        void RegisterPhaseCompletionNodesToRenderGraph(RenderGraph& renderGraph,
                                                       const RenderGraphExecuteContext& executeContext) const;

    private:
        struct PhaseCompletionNodeEntry
        {
            std::string phaseTag;
            std::string nodeName;
            PhaseCompletionMode mode = PhaseCompletionMode::Deterministic;
            RenderPassRequirements requirements{};
            PhaseCompletionCallback execute;
        };

        std::vector<std::shared_ptr<IRenderPass>> BuildPassesFromSequence(const std::vector<RenderPassType>& sequence,
                                                                          bool* allValid = nullptr) const;
        void RebuildPassCache();
        void ReplaceWithCustomPassSequence(std::vector<std::shared_ptr<IRenderPass>> passes);
        PassHandle InsertPassAt(size_t insertIndex, const std::shared_ptr<IRenderPass>& renderPass);
        size_t FindPassIndexByTag(std::string_view targetTag) const;

        std::array<std::shared_ptr<IRenderPass>, kBuiltinPassCount> m_builtinPasses{};
        std::vector<std::shared_ptr<IRenderNode>> m_renderNodes;
        std::vector<std::shared_ptr<IRenderPass>> m_renderPasses;
        std::vector<PhaseCompletionNodeEntry>     m_phaseCompletionNodes;
        std::vector<RenderPassType>               m_renderPassQueueTypes;
    };
}
