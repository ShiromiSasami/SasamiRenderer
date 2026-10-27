#pragma once
// FrameRenderContext: the frame-local state shared across the stages Renderer::Render
// was split into.
//
// Render() used to be a single ~1400-line function. Splitting it into per-stage member
// functions (see RendererDrawCallbacks.cpp and RendererPassServices.cpp) runs into the
// fact that several of those stages build closures -- draw callbacks, render-pass
// services, the cross-queue graphics-node rotation callbacks -- that capture locals by
// reference and are only *invoked* much later, during RenderGraph::Execute() in a later
// stage that still lives in RendererFrameGraph.cpp. If those locals lived on the stack
// of a stage function that already returned, the captured references would dangle.
// FrameRenderContext exists purely to give that frame-local state a home that outlives
// every stage call: Renderer::Render() keeps one of these on its own stack, and each
// stage function receives it by reference, reads what earlier stages produced, and
// writes the fields later stages need.
//
// This type is a non-owning VIEW plus the handful of small per-frame values and
// std::function/optional/unique_ptr slots that stages hand off to each other. It never
// owns any GPU resource itself (those all live in Renderer's own members, RenderTargetPool,
// FrameCoordinator, etc.) -- it only bundles pointers, references and small frame-scoped
// values so the split stage functions can share them safely.
//
// `cmdList` is deliberately a pointer to Renderer::Render's own `CommandList*` local
// (not a copy of its value): the graphics-node rotation callback built in stage 5
// (BuildFramePassInputsAndServices) reassigns that variable mid-frame when it rotates
// onto a new command list for a cross-queue join, and stages 6-9 (still in
// RendererFrameGraph.cpp) must see that reassignment through their own unmodified,
// bare `cmdList` references. A plain pointer copy here would not observe that mutation.

#include "Renderer/Runtime/D3D12CommandListRhiEncoder.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/Frame/RendererFrameCoordinator.h"
#include "Renderer/RayTracing/SWRTExecutor.h"
#include "Renderer/Scene/LightSystem.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace SasamiEngine::Render
{
    struct FrameRenderContext
    {
        // --- Produced before any stage runs (Renderer::Render's own stage-1 locals) ---
        CommandList** cmdList = nullptr;
        RendererFrameCoordinator::FrameContext* frame = nullptr;
        UINT backIndex = 0;

        // --- Produced by stage 4 (RendererDrawCallbacks.cpp), consumed by stage 5 -------
        std::optional<D3D12CommandListRhiEncoder> graphicsCommandEncoder;
        std::function<GpuDescriptorHandle()> selectRuntimeAoSrv;
        std::function<void(bool drawTransparent, bool dynamicVerticesOnly)> drawItems;
        std::function<void(const LightSystem::ShadowPassContext&)> drawShadowItems;

        // Accumulated across drawShadowItems' four per-frame cascade invocations; moved
        // here (rather than staying a stage-4-local) because drawShadowItems itself is
        // stored in this context and can still be invoked, via the render-pass graph,
        // after stage 4 returns.
        struct ShadowRecordStats
        {
            double   recordMicroseconds = 0.0;
            uint64_t triangles          = 0;
            uint64_t draws              = 0;
        };
        ShadowRecordStats shadowRecordStats{};

        D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu = 0;
        GpuDescriptorHandle defaultAoSrv{};
        SWRTExecutor::PartialBehavior partialBehavior{};
        bool useShadowTessPath = false;
        bool useSoftwareRayTracedReflections = false;
        bool useSoftwareRayTracedAmbientOcclusion = false;

        // --- Produced by stage 5 (RendererPassServices.cpp), consumed by stages 6-9 -----
        RenderPassExecutionPolicy executionPolicy{};
        RenderPassFrameInputs frameInputs{};
        CommandList* computeCmdList = nullptr;
        std::unique_ptr<D3D12CommandListRhiEncoder> computeCommandEncoder;
        RenderPassFrameInputs computeFrameInputs{};
        UINT computeSlot = 0;
        RenderPassExecutionServices executionServices{};
        std::vector<std::unique_ptr<D3D12CommandListRhiEncoder>> extraGraphicsEncoders;
        size_t nextGraphicsNodeSlot = 0;
        RenderGraphExecuteContext executeContext{};
    };
}
