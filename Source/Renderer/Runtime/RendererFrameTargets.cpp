// RendererFrameTargets.cpp
// Single responsibility: ensure this frame's render targets exist at the current viewport
// size and declare them (scene color, transmission copy, OIT, optional SSR, scene depth,
// GBuffer, SSAO, and the back buffer) to the render graph as external resources. This
// reserves and publishes resources but records no commands, which is why it is separate
// from RendererFrameGraph.cpp. m_renderGraph.Clear() stays with the caller because Render()
// owns the graph's per-frame lifecycle.
#include "Renderer/Runtime/Renderer.h"

#include <cstdint>

#include "Foundation/Tools/DebugOutput.h"

// windows.h (pulled in transitively by the D3D12 headers) defines min/max as macros. Same
// guard as RendererFrameGraph.cpp, kept so this file stays safe if std::min/std::max are
// ever used here.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace SasamiEngine::Render
{
    namespace
    {
        // What these imported textures are genuinely in. They are created through the RHI as
        // ShaderResource, which it realises as NON_PIXEL|PIXEL (0xC0); declaring PIXEL alone here
        // made the graph's very first barrier on each one claim a Before state it had never held.
        // That is why the debug layer reported exactly one id=527 per imported target.
        constexpr D3D12_RESOURCE_STATES kImportedShaderReadState =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    bool Renderer::EnsureAndImportFrameTargets(UINT backIndex)
    {
        const auto* backBuffer = m_renderTargetPool.GetBackBufferResource(backIndex);
        if (!backBuffer || !m_renderTargetPool.GetDepth().IsValid() || !m_renderTargetPool.GetDepthDsv().ptr) {
            return false;
        }
        const UINT sceneColorW = static_cast<UINT>(m_viewport.Width);
        const UINT sceneColorH = static_cast<UINT>(m_viewport.Height);
        if (!m_renderTargetPool.EnsureSceneColor(*m_device, sceneColorW, sceneColorH) ||
            !m_renderTargetPool.GetSceneColorTexture().IsValid()) {
            return false;
        }
        if (!m_renderTargetPool.EnsureTransmissionSceneColorCopy(*m_device, sceneColorW, sceneColorH)) {
            return false;
        }
        if (!m_renderTargetPool.EnsureTransparentBackfaceDistance(*m_device, sceneColorW, sceneColorH)) {
            return false;
        }
        if (!m_renderTargetPool.EnsureTransparentOit(*m_device, sceneColorW, sceneColorH)) {
            return false;
        }
        // This predicate decides whether to *allocate* the SSR render targets, whereas the
        // same-named predicate in Renderer.cpp decides whether to *execute* the SSR pass. It
        // intentionally omits IsValid() checks on the SSR targets themselves: this is the code
        // that creates them, so requiring them to already be valid would mean they could never
        // be created. When software ray traced reflections take over the frame, the SSR targets
        // are unused, so skip allocating them to avoid wasting VRAM.
        const bool softwareRayTracedReflectionsActive =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterSoftwareRayTracedReflectionEnabled &&
            m_renderTargetPool.GetSWRTReflectionTexture().IsValid();
        const bool needsScreenSpaceReflectionTargets =
            (m_settings.renderPathMode == RenderPathMode::Raster) &&
            m_settings.rasterScreenSpaceReflectionEnabled &&
            !softwareRayTracedReflectionsActive;
        if (needsScreenSpaceReflectionTargets &&
            !m_renderTargetPool.EnsureScreenSpaceReflection(*m_device, sceneColorW, sceneColorH)) {
            DebugLog("Renderer::Render: failed to prepare screen-space reflection resources. Disabling SSR for this frame.\n");
        }

        ExternalRenderGraphResourceDesc sceneColorDesc{};
        sceneColorDesc.resource = m_renderTargetPool.GetSceneColorTexture().Get();
        sceneColorDesc.initialState = kImportedShaderReadState;
        sceneColorDesc.finalState = kImportedShaderReadState;
        sceneColorDesc.transitionToFinalState = true;
        sceneColorDesc.rtv = m_renderTargetPool.GetSceneColorRtv();
        sceneColorDesc.hasRtv = true;
        sceneColorDesc.clearColorOnFirstUse = true;
        sceneColorDesc.clearColor[0] = 0.2f;
        sceneColorDesc.clearColor[1] = 0.2f;
        sceneColorDesc.clearColor[2] = 0.2f;
        sceneColorDesc.clearColor[3] = 1.0f;
        m_renderGraph.ImportExternalResource("SceneColor", sceneColorDesc);

        if (m_renderTargetPool.GetTransmissionSceneColorCopyTexture().IsValid()) {
            // Import TransmissionSceneColor so the graph's Read("TransmissionSceneColor")
            // (TransparentCompositeRenderPass) actually transitions it instead of being a
            // no-op dependency edge. TransparentSceneColorCopyRenderPass still writes it via
            // a manual CopyResource (CopySceneColorForTransmission) that already restores
            // PIXEL_SHADER_RESOURCE afterward, so this import never needs to issue its own
            // barrier -- it just lets the graph's bookkeeping agree with reality.
            ExternalRenderGraphResourceDesc transmissionSceneColorDesc{};
            transmissionSceneColorDesc.resource = m_renderTargetPool.GetTransmissionSceneColorCopyTexture().Get();
            transmissionSceneColorDesc.initialState = kImportedShaderReadState;
            transmissionSceneColorDesc.finalState = kImportedShaderReadState;
            transmissionSceneColorDesc.transitionToFinalState = true;
            transmissionSceneColorDesc.gpuSrv = m_renderTargetPool.GetTransmissionSceneColorCopySrv();
            transmissionSceneColorDesc.hasSrv = true;
            m_renderGraph.ImportExternalResource("TransmissionSceneColor", transmissionSceneColorDesc);
        }

        if (m_renderTargetPool.GetSSRSceneColorCopyTexture().IsValid() &&
            m_renderTargetPool.GetSSRReflectionTexture().IsValid()) {
            ExternalRenderGraphResourceDesc ssrSceneColorCopyDesc{};
            ssrSceneColorCopyDesc.resource = m_renderTargetPool.GetSSRSceneColorCopyTexture().Get();
            ssrSceneColorCopyDesc.initialState = kImportedShaderReadState;
            ssrSceneColorCopyDesc.finalState = kImportedShaderReadState;
            ssrSceneColorCopyDesc.transitionToFinalState = true;
            ssrSceneColorCopyDesc.gpuSrv = m_renderTargetPool.GetSSRSceneColorCopySrv();
            ssrSceneColorCopyDesc.hasSrv = true;
            m_renderGraph.ImportExternalResource("SSRSceneColorCopy", ssrSceneColorCopyDesc);

            ExternalRenderGraphResourceDesc ssrReflectionDesc{};
            ssrReflectionDesc.resource = m_renderTargetPool.GetSSRReflectionTexture().Get();
            ssrReflectionDesc.initialState = kImportedShaderReadState;
            ssrReflectionDesc.finalState = kImportedShaderReadState;
            ssrReflectionDesc.transitionToFinalState = true;
            ssrReflectionDesc.gpuSrv = m_renderTargetPool.GetSSRReflectionSrv();
            ssrReflectionDesc.hasSrv = true;
            m_renderGraph.ImportExternalResource("SSRReflection", ssrReflectionDesc);
        }

        ExternalRenderGraphResourceDesc backBufferDesc{};
        backBufferDesc.resource = backBuffer->Get();
        backBufferDesc.initialState = D3D12_RESOURCE_STATE_PRESENT;
        backBufferDesc.finalState = D3D12_RESOURCE_STATE_PRESENT;
        backBufferDesc.transitionToFinalState = true;
        backBufferDesc.rtv = m_renderTargetPool.GetBackBufferRtv(backIndex);
        backBufferDesc.hasRtv = true;
        backBufferDesc.clearColorOnFirstUse = false;
        m_renderGraph.ImportExternalResource("BackBuffer", backBufferDesc);

        ExternalRenderGraphResourceDesc sceneDepthDesc{};
        sceneDepthDesc.resource = m_renderTargetPool.GetDepth().Get();
        sceneDepthDesc.initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        sceneDepthDesc.finalState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        sceneDepthDesc.transitionToFinalState = true;
        sceneDepthDesc.dsv = m_renderTargetPool.GetDepthDsv();
        sceneDepthDesc.hasDsv = true;
        sceneDepthDesc.clearDepthOnFirstUse = true;
        sceneDepthDesc.clearDepth = 0.0f; // Reverse-Z: far plane clears to 0.0, near plane is 1.0
        sceneDepthDesc.clearStencil = 0;
        m_renderGraph.ImportExternalResource("SceneDepth", sceneDepthDesc);

        ExternalRenderGraphResourceDesc transparentBackfaceDistanceDesc{};
        transparentBackfaceDistanceDesc.resource = m_renderTargetPool.GetTransparentBackfaceDistanceTexture().Get();
        transparentBackfaceDistanceDesc.initialState = kImportedShaderReadState;
        transparentBackfaceDistanceDesc.finalState = kImportedShaderReadState;
        transparentBackfaceDistanceDesc.transitionToFinalState = true;
        transparentBackfaceDistanceDesc.rtv = m_renderTargetPool.GetTransparentBackfaceDistanceRtv();
        transparentBackfaceDistanceDesc.hasRtv = true;
        transparentBackfaceDistanceDesc.gpuSrv = m_renderTargetPool.GetTransparentBackfaceDistanceSrv();
        transparentBackfaceDistanceDesc.hasSrv = true;
        transparentBackfaceDistanceDesc.clearColorOnFirstUse = true;
        m_renderGraph.ImportExternalResource("TransparentBackfaceDistance", transparentBackfaceDistanceDesc);

        ExternalRenderGraphResourceDesc transparentOitAccumDesc{};
        transparentOitAccumDesc.resource = m_renderTargetPool.GetTransparentOitAccumTexture().Get();
        transparentOitAccumDesc.initialState = kImportedShaderReadState;
        transparentOitAccumDesc.finalState = kImportedShaderReadState;
        transparentOitAccumDesc.transitionToFinalState = true;
        transparentOitAccumDesc.rtv = m_renderTargetPool.GetTransparentOitAccumRtv();
        transparentOitAccumDesc.hasRtv = true;
        transparentOitAccumDesc.gpuSrv = m_renderTargetPool.GetTransparentOitAccumSrv();
        transparentOitAccumDesc.hasSrv = true;
        transparentOitAccumDesc.clearColorOnFirstUse = true;
        m_renderGraph.ImportExternalResource("TransparentOitAccum", transparentOitAccumDesc);

        ExternalRenderGraphResourceDesc transparentOitRevealageDesc{};
        transparentOitRevealageDesc.resource = m_renderTargetPool.GetTransparentOitRevealageTexture().Get();
        transparentOitRevealageDesc.initialState = kImportedShaderReadState;
        transparentOitRevealageDesc.finalState = kImportedShaderReadState;
        transparentOitRevealageDesc.transitionToFinalState = true;
        transparentOitRevealageDesc.rtv = m_renderTargetPool.GetTransparentOitRevealageRtv();
        transparentOitRevealageDesc.hasRtv = true;
        transparentOitRevealageDesc.gpuSrv = m_renderTargetPool.GetTransparentOitRevealageSrv();
        transparentOitRevealageDesc.hasSrv = true;
        transparentOitRevealageDesc.clearColorOnFirstUse = true;
        transparentOitRevealageDesc.clearColor[0] = 1.0f;
        transparentOitRevealageDesc.clearColor[1] = 1.0f;
        transparentOitRevealageDesc.clearColor[2] = 1.0f;
        transparentOitRevealageDesc.clearColor[3] = 1.0f;
        m_renderGraph.ImportExternalResource("TransparentOitRevealage", transparentOitRevealageDesc);

        // Import GBuffer resources so GBufferRenderPass can bind them as MRTs.
        const UINT gbufferW = static_cast<UINT>(m_viewport.Width);
        const UINT gbufferH = static_cast<UINT>(m_viewport.Height);
        m_renderTargetPool.EnsureGBuffer(*m_device, gbufferW, gbufferH);
        if (m_renderTargetPool.GetGBufferAlbedo().IsValid()) {
            const auto importGBuffer = [&](const char* name, Resource& res, CpuDescriptorHandle rtv, GpuDescriptorHandle srv) {
                ExternalRenderGraphResourceDesc d{};
                d.resource = res.Get();
                d.initialState = kImportedShaderReadState;
                d.finalState = kImportedShaderReadState;
                d.transitionToFinalState = true;
                d.rtv = rtv;
                d.hasRtv = true;
                d.gpuSrv = srv;
                d.hasSrv = true;
                d.clearColorOnFirstUse = true;
                // clearColor defaults to 0,0,0,0
                m_renderGraph.ImportExternalResource(name, d);
            };
            importGBuffer("GBufferAlbedo",   m_renderTargetPool.GetGBufferAlbedo(),   m_renderTargetPool.GetGBufferAlbedoRtv(),   m_renderTargetPool.GetGBufferAlbedoSrv());
            importGBuffer("GBufferNormal",   m_renderTargetPool.GetGBufferNormal(),   m_renderTargetPool.GetGBufferNormalRtv(),   m_renderTargetPool.GetGBufferNormalSrv());
            importGBuffer("GBufferMaterial", m_renderTargetPool.GetGBufferMaterial(), m_renderTargetPool.GetGBufferMaterialRtv(), m_renderTargetPool.GetGBufferMaterialSrv());
            importGBuffer("GBufferEmissive", m_renderTargetPool.GetGBufferEmissive(), m_renderTargetPool.GetGBufferEmissiveRtv(), m_renderTargetPool.GetGBufferEmissiveSrv());
            importGBuffer("GBufferSpecularWorkflow", m_renderTargetPool.GetGBufferSpecularWorkflow(), m_renderTargetPool.GetGBufferSpecularWorkflowRtv(), m_renderTargetPool.GetGBufferSpecularWorkflowSrv());

            // Import SSAO / SSAOBlur so the graph owns their RTV<->SRV transitions instead of
            // hand-written barriers in SSAORenderPass.
            if (m_renderTargetPool.GetSSAOTexture().IsValid()) {
                importGBuffer("SSAO", m_renderTargetPool.GetSSAOTexture(), m_renderTargetPool.GetSSAORtv(), m_renderTargetPool.GetSSAOSrv());
            }
            if (m_renderTargetPool.GetSSAOBlurTexture().IsValid()) {
                importGBuffer("SSAOBlur", m_renderTargetPool.GetSSAOBlurTexture(), m_renderTargetPool.GetSSAOBlurRtv(), m_renderTargetPool.GetSSAOBlurSrv());
            }
        }

        return true;
    }
}
