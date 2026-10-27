// RendererPostProcessPasses.cpp
//
// Single responsibility: push this frame's settings into the screen-space post-process passes
// (light shafts, lens flare, FXAA) and insert each pass into the pass registry on first use.
//
// Configuring passes from settings is a different responsibility from importing render-graph
// resources, which is why it lives in its own file rather than being wedged between two
// resource imports as it used to be.
//
// The two are not disjoint in the state they touch: RendererFrameTargets.cpp and this file both
// reach into m_renderTargetPool. What keeps them independent is that this file's use of the pool
// is read-only -- a single GetBackBufferResource() -- and what it reads is the swap chain's back
// buffer, not any of the targets RendererFrameTargets.cpp's Ensure* calls allocate. It also never
// touches m_renderGraph. If this file ever starts calling an Ensure* of its own, or reads a
// target those imports allocate, that independence is gone; see the call site in
// RendererFrameGraph.cpp for the ordering consequence.

#include "Renderer/Runtime/Renderer.h"

#include <memory>

#include "Renderer/Passes/PostProcess/FxaaRenderPass.h"
#include "Renderer/Passes/PostProcess/LightShaftRenderPass.h"
#include "Renderer/Passes/PostProcess/LensFlareRenderPass.h"

// windows.h (pulled in transitively by the D3D12 headers) defines min/max as macros. Same
// guard as RendererFrameGraph.cpp.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace SasamiEngine::Render
{
    void Renderer::ConfigurePostProcessPasses(UINT backIndex)
    {
        // EnsureAndImportFrameTargets() has already validated this back buffer for the frame;
        // the guard is here so this function is safe to read on its own.
        const auto* backBuffer = m_renderTargetPool.GetBackBufferResource(backIndex);
        if (!backBuffer) {
            return;
        }
        const UINT sceneColorW = static_cast<UINT>(m_viewport.Width);
        const UINT sceneColorH = static_cast<UINT>(m_viewport.Height);

        // Bloom runs inside ToneMapSceneColor rather than as a graph pass; its chain follows
        // SceneColor's size. Built only once it is turned on, so a default run allocates nothing.
        if (m_settings.bloomEnabled) {
            m_bloomRenderer.EnsureResources(*m_device,
                [this](UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu) {
                    return m_srvAllocator.Allocate(count, outCpu, outGpu);
                },
                sceneColorW, sceneColorH);
        }

        // Registered before FXAA so the shafts are antialiased along with everything else
        // rather than being laid over an already-resolved image.
        static std::shared_ptr<LightShaftRenderPass> s_lightShaftPass = std::make_shared<LightShaftRenderPass>();
        static bool s_lightShaftPassRegistered = false;
        if (!s_lightShaftPassRegistered) {
            m_passRegistry.AddPassAfter("PostProcess", s_lightShaftPass);
            s_lightShaftPassRegistered = true;
        }
        s_lightShaftPass->SetEnabled(m_settings.lightShaftEnabled);
        s_lightShaftPass->SetIntensity(m_settings.lightShaftIntensity);
        s_lightShaftPass->SetBackBufferResource(backBuffer->Get());
        s_lightShaftPass->EnsureResources(*m_device,
                                          [this](UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu) {
                                              return m_srvAllocator.Allocate(count, outCpu, outGpu);
                                          },
                                          sceneColorW, sceneColorH);

        // After the shafts so the flare's visibility probe sees them (a light already glowing
        // through god rays should flare too), and before FXAA so the ghost edges get resolved.
        static std::shared_ptr<LensFlareRenderPass> s_lensFlarePass = std::make_shared<LensFlareRenderPass>();
        static bool s_lensFlarePassRegistered = false;
        if (!s_lensFlarePassRegistered) {
            m_passRegistry.AddPassAfter("PostProcess", s_lensFlarePass);
            s_lensFlarePassRegistered = true;
        }
        s_lensFlarePass->SetEnabled(m_settings.lensFlareEnabled);
        s_lensFlarePass->SetIntensity(m_settings.lensFlareIntensity);
        s_lensFlarePass->SetBladeCount(m_settings.lensFlareBladeCount);
        s_lensFlarePass->SetMode(static_cast<LensFlareMode>(m_settings.lensFlareMode));
        s_lensFlarePass->SetBackBufferResource(backBuffer->Get());
        s_lensFlarePass->EnsureResources(*m_device,
                                         [this](UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu) {
                                             return m_srvAllocator.Allocate(count, outCpu, outGpu);
                                         },
                                         sceneColorW, sceneColorH);

        static std::shared_ptr<FxaaRenderPass> s_fxaaPass = std::make_shared<FxaaRenderPass>();
        static bool s_fxaaPassRegistered = false;
        if (!s_fxaaPassRegistered) {
            m_passRegistry.AddPassAfter("PostProcess", s_fxaaPass);
            s_fxaaPassRegistered = true;
        }
        s_fxaaPass->SetEnabled(m_settings.fxaaEnabled);
        s_fxaaPass->SetBackBufferResource(backBuffer->Get());
        s_fxaaPass->EnsureResources(*m_device,
                                    [this](UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu) {
                                        return m_srvAllocator.Allocate(count, outCpu, outGpu);
                                    },
                                    sceneColorW, sceneColorH);
    }
}
