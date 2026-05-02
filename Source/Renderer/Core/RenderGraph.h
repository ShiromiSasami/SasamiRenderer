#pragma once

#include "Renderer/Core/GraphicsDevice.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SasamiRenderer
{
    class IRenderNode;
    struct RenderNodeExecutionPolicy;
    struct RenderNodeFrameInputs;
    struct RenderNodeExecutionServices;
    struct RenderNodeRequirements;
    class RenderNodeContextView;
    class ResourceRegistry;

    struct ResourceHandle
    {
        size_t index = static_cast<size_t>(-1);
        bool IsValid() const { return index != static_cast<size_t>(-1); }
        bool operator==(const ResourceHandle& other) const { return index == other.index; }
        bool operator!=(const ResourceHandle& other) const { return index != other.index; }
    };

    struct ExternalRenderGraphResourceDesc
    {
        ID3D12Resource* resource = nullptr;
        D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON;
        bool transitionToFinalState = false;

        CpuDescriptorHandle rtv{};
        bool hasRtv = false;
        CpuDescriptorHandle dsv{};
        bool hasDsv = false;

        bool clearColorOnFirstUse = false;
        float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        bool clearDepthOnFirstUse = false;
        float clearDepth = 1.0f;
        UINT8 clearStencil = 0;
    };

    struct RenderGraphExecuteContext
    {
        const RenderNodeExecutionPolicy*     executionPolicy      = nullptr;
        const RenderNodeFrameInputs*         frameInputs          = nullptr;  // graphics CL
        const RenderNodeFrameInputs*         computeFrameInputs   = nullptr;  // compute CL (may be null)
        const RenderNodeExecutionServices*   executionServices    = nullptr;
        const ResourceRegistry*              resources            = nullptr;

        // Async compute cross-queue sync. All may be null if no compute queue.
        ID3D12CommandQueue*  graphicsQueueRaw   = nullptr;
        ID3D12CommandQueue*  computeQueueRaw    = nullptr;
        ID3D12Fence*         crossQueueFence    = nullptr;
        UINT64*              crossQueueFenceVal = nullptr;

        RenderNodeContextView CreateContextView(const RenderNodeRequirements& requirements) const;
        RenderNodeContextView CreateComputeContextView(const RenderNodeRequirements& requirements) const;
        ResourceHandle FindGraphResource(std::string_view resourceName) const;
        std::string_view GetResourceName(ResourceHandle handle) const;
    };

    class ResourceRegistry
    {
    public:
        struct ResourceRecord
        {
            std::string name;
            bool hasExternalResource = false;
            ID3D12Resource* resource = nullptr;
            D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
            D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_COMMON;
            D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON;
            bool transitionToFinalState = false;

            CpuDescriptorHandle rtv{};
            bool hasRtv = false;
            CpuDescriptorHandle dsv{};
            bool hasDsv = false;

            bool clearColorOnFirstUse = false;
            float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            bool clearDepthOnFirstUse = false;
            float clearDepth = 1.0f;
            UINT8 clearStencil = 0;
            bool colorCleared = false;
            bool depthCleared = false;
        };

        ResourceHandle Register(std::string_view resourceName);
        ResourceHandle RegisterExternal(std::string_view resourceName, const ExternalRenderGraphResourceDesc& desc);
        ResourceHandle Find(std::string_view resourceName) const;
        std::string_view GetName(ResourceHandle handle) const;
        ResourceRecord* GetMutable(ResourceHandle handle);
        const ResourceRecord* Get(ResourceHandle handle) const;
        std::vector<ResourceRecord>& MutableRecords() { return m_records; }
        const std::vector<ResourceRecord>& Records() const { return m_records; }
        void Clear();

    private:
        std::unordered_map<std::string, size_t> m_nameToIndex;
        std::vector<ResourceRecord> m_records;
    };

    class RenderGraph
    {
    public:
        using ExecuteCallback = std::function<bool()>;
        enum class PhaseCompletionMode
        {
            Deterministic,
            Opportunistic
        };

        struct NodeHandle
        {
            size_t index = static_cast<size_t>(-1);
            bool IsValid() const { return index != static_cast<size_t>(-1); }
        };
        struct PhaseHandle
        {
            size_t index = static_cast<size_t>(-1);
            bool IsValid() const { return index != static_cast<size_t>(-1); }
        };

        NodeHandle AddNode(const IRenderNode& renderNode,
                           const RenderGraphExecuteContext& executeContext,
                           NodeHandle previousNode = {});
        PhaseHandle AddPhase(std::string_view phaseName);
        bool AddPhaseCompletionNode(std::string_view phaseName,
                                    std::string_view nodeName,
                                    const ExecuteCallback& execute,
                                    PhaseCompletionMode mode);
        ResourceHandle ImportExternalResource(std::string_view resourceName, const ExternalRenderGraphResourceDesc& desc);
        void Read(NodeHandle node, ResourceHandle resource);
        void Write(NodeHandle node, ResourceHandle resource);
        void UseColorTarget(NodeHandle node, ResourceHandle resource);
        void UseDepthTarget(NodeHandle node, ResourceHandle resource);
        void DependsOn(NodeHandle node, NodeHandle dependency);

        bool Execute();
        void Clear();
        const ResourceRegistry& GetResourceRegistry() const { return m_resources; }

    private:
        struct PassNode;
        struct PhaseCompletionNode;
        struct PhaseNode;

        NodeHandle AddPassInternal(const std::string& name, const ExecuteCallback& execute, PhaseHandle phase);
        PhaseHandle FindOrAddPhase(std::string_view phaseName);
        bool ExecutePhaseCompletionNodes(size_t phaseIndex, PhaseCompletionMode mode) const;
        bool BuildExecutionOrder(std::vector<size_t>& outOrder) const;
        // Group nodes in topological order into parallel levels (same level = no dependency on each other).
        std::vector<std::vector<size_t>> BuildExecutionLevels(const std::vector<size_t>& topoOrder) const;
        CommandList* GetCommandList() const;
        bool TransitionResource(ResourceRegistry::ResourceRecord& resource, D3D12_RESOURCE_STATES requiredState);
        bool PreparePassResources(const PassNode& pass);
        void FinalizeExternalResources();

        struct PassNode
        {
            std::string name;
            ExecuteCallback execute;
            size_t phaseIndex = static_cast<size_t>(-1);
            std::vector<ResourceHandle> reads;
            std::vector<ResourceHandle> writes;
            std::vector<ResourceHandle> colorTargets;
            ResourceHandle depthTarget{};
            std::vector<size_t> explicitDependencies;
            bool preferCompute = false; // cached from IRenderNode::PreferredQueue()
        };
        struct PhaseCompletionNode
        {
            std::string name;
            ExecuteCallback execute;
        };
        struct PhaseNode
        {
            std::string name;
            size_t passCount = 0;
            std::vector<PhaseCompletionNode> deterministicNodes;
            std::vector<PhaseCompletionNode> opportunisticNodes;
        };

        std::vector<PassNode> m_passes;
        std::unordered_map<std::string, size_t> m_phaseNameToIndex;
        std::vector<PhaseNode> m_phases;
        ResourceRegistry m_resources;
        RenderGraphExecuteContext m_executeContext{};
        bool m_hasExecuteContext = false;
    };

    class RenderGraphBuilder
    {
    public:
        RenderGraphBuilder(RenderGraph& renderGraph,
                           ResourceRegistry& resources,
                           RenderGraph::NodeHandle pass,
                           RenderGraph::NodeHandle previousPass);

        ResourceHandle Import(std::string_view resourceName);
        ResourceHandle Read(std::string_view resourceName);
        ResourceHandle Read(ResourceHandle handle);
        ResourceHandle Write(std::string_view resourceName);
        ResourceHandle Write(ResourceHandle handle);
        ResourceHandle UseColorTarget(std::string_view resourceName);
        ResourceHandle UseColorTarget(ResourceHandle handle);
        ResourceHandle UseDepthTarget(std::string_view resourceName);
        ResourceHandle UseDepthTarget(ResourceHandle handle);
        void DependsOnPrevious();

    private:
        RenderGraph* m_renderGraph = nullptr;
        ResourceRegistry* m_resources = nullptr;
        RenderGraph::NodeHandle m_pass{};
        RenderGraph::NodeHandle m_previousPass{};
    };

}
