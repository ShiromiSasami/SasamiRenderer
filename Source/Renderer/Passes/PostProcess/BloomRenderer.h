#pragma once

#include "Renderer/Frame/RendererFrameCoordinator.h"
#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"

#include <array>
#include <functional>

namespace SasamiEngine::Render
{
    // Physically based bloom as a mip chain blur of the HDR SceneColor -- Jimenez, "Next
    // Generation Post Processing in Call of Duty: Advanced Warfare" (SIGGRAPH 2014), in the
    // formulation of learnopengl.com "Physically Based Bloom" (2022):
    //   down: SceneColor -> mip0 (half res, Karis average) -> mip1 -> ... with a 13-tap filter,
    //   up:   mipN-1 -> ... -> mip0 with a 3x3 tent, added (ONE/ONE) onto the larger mip.
    // mip0 then holds the bloom that ToneMap_PS lerps in before exposure (u_toneMapParams.w).
    // No threshold: every pixel contributes, the small blend weight keeps only bright ones visible.
    //
    // Not a render-graph pass: it runs inside Renderer::ToneMapSceneColor, where the graph has
    // already put SceneColor in PIXEL_SHADER_RESOURCE. The mip chain is private, so this class
    // owns its barriers (rest state PIXEL_SHADER_RESOURCE, RENDER_TARGET only while drawn into).
    //
    // DX12-only, like the rest of the tone map path. The native DX11/Vulkan/GL paths have no bloom.
    class BloomRenderer
    {
    public:
        static constexpr UINT MAX_MIPS = 6;
        static constexpr DXGI_FORMAT MIP_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;

        using SrvAllocFn = std::function<bool(UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu)>;

        // (Re)creates the chain for a sceneWidth x sceneHeight SceneColor. Cheap when unchanged.
        bool EnsureResources(GraphicsDevice& device, const SrvAllocFn& allocateSrv, UINT sceneWidth, UINT sceneHeight);
        bool IsReady() const { return m_mipCount > 0; }

        // Records the whole chain. Expects the shared root signature and the SRV heap to be bound;
        // leaves viewport, render target and pipeline state changed, so the caller re-binds them.
        // `radius` is the tent radius in UV units. Returns false (and records nothing) when not ready.
        bool Execute(CommandList& cmdList,
                     GpuDescriptorHandle sceneColorSrv,
                     RenderPipelineStateCache& pipelineStateCache,
                     RendererFrameCoordinator& frameCoordinator,
                     RendererFrameCoordinator::FrameContext& frame,
                     float radius) const;

        // The bloom result (mip0) in PIXEL_SHADER_RESOURCE state, valid after Execute.
        GpuDescriptorHandle GetResultSrv() const { return m_mips[0].srvGpu; }
        // Every upsample adds one more level into mip0, so the result holds the sum of m_mipCount
        // blurred images. ToneMap_PS multiplies by this to get their mean before the lerp, which keeps
        // lerp(scene, bloom, w) energy-preserving (learnopengl Phys.-Based-Bloom divides the same way
        // implicitly by mixing a single normalised result).
        float GetResultScale() const { return m_mipCount > 0 ? 1.0f / static_cast<float>(m_mipCount) : 0.0f; }

    private:
        struct Mip
        {
            Resource texture;
            CpuDescriptorHandle srvCpu{};
            GpuDescriptorHandle srvGpu{};
            CpuDescriptorHandle rtv{};
            UINT width = 0;
            UINT height = 0;
        };

        void DrawStep(CommandList& cmdList, const Mip& target, GpuDescriptorHandle source,
                      UINT sourceWidth, UINT sourceHeight, float karis, float radius,
                      RendererFrameCoordinator& frameCoordinator,
                      RendererFrameCoordinator::FrameContext& frame) const;

        std::array<Mip, MAX_MIPS> m_mips{};
        DescriptorHeap m_rtvHeap;
        bool m_srvAllocated = false;
        UINT m_mipCount = 0;
        UINT m_sceneWidth = 0; // SceneColor size the chain was built for
        UINT m_sceneHeight = 0;
    };
}

namespace SasamiEngine { using namespace Render; }
