// RendererPassSequence.cpp
// Which passes exist and in what order: node and pass registration, the render-node presets, and
// the Ensure*PassInserted / Reinsert* calls that add an optional pass at the right point once its
// feature is switched on.
//
// Split from Renderer.cpp because it edits the pass list rather than executing it -- these run in
// response to a setting changing, never inside a frame.
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Passes/Debug/DebugProbeGridRenderPass.h"
#include "Foundation/Tools/DebugOutput.h"

#include <memory>
#include <vector>

namespace SasamiEngine::Render
{
    Renderer::NodeHandle Renderer::AddNode(const std::shared_ptr<IRenderNode>& renderNode)
    {
        return m_passRegistry.AddNode(renderNode);
    }

    void Renderer::SetRenderNodePreset(std::shared_ptr<IRenderNode> renderNode)
    {
        m_passRegistry.SetRenderNodePreset(std::move(renderNode));
    }

    void Renderer::UseDefaultRenderNodePreset()
    {
        m_passRegistry.UseDefaultRenderNodePreset();
        EnsureDebugProbeGridPassInserted();
        EnsureFluidSurfacePassInserted();
        EnsureParticlePassInserted();
        EnsureVolumetricCloudPassInserted();
        EnsureMeshShaderPassInserted();
        EnsureCrowdPassInserted();
        EnsureWireframeOverlayPassInserted();
    }

    void Renderer::UseToonRenderNodePreset()
    {
        m_passRegistry.UseToonRenderNodePreset();
        // Same follow-up insertions as the default preset: this preset differs only by the
        // outline pass, so omitting these would silently drop the probe grid, fluid, particle
        // and cloud passes for any app that picks the toon path.
        EnsureDebugProbeGridPassInserted();
        EnsureFluidSurfacePassInserted();
        EnsureParticlePassInserted();
        EnsureVolumetricCloudPassInserted();
        EnsureMeshShaderPassInserted();
        EnsureCrowdPassInserted();
        EnsureWireframeOverlayPassInserted();
    }

    Renderer::PassHandle Renderer::AddPass(const std::shared_ptr<IRenderPass>& renderPass)
    {
        return m_passRegistry.AddPass(renderPass);
    }

    Renderer::PassHandle Renderer::AddPassBefore(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass)
    {
        return m_passRegistry.AddPassBefore(targetTag, renderPass);
    }

    Renderer::PassHandle Renderer::AddPassAfter(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass)
    {
        return m_passRegistry.AddPassAfter(targetTag, renderPass);
    }

    bool Renderer::ReplacePass(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass)
    {
        return m_passRegistry.ReplacePass(targetTag, renderPass);
    }

    bool Renderer::HasRenderPass(std::string_view tag) const
    {
        const auto& passes = m_passRegistry.GetPasses();
        return std::any_of(passes.begin(), passes.end(),
                           [tag](const std::shared_ptr<IRenderPass>& pass) {
                               return pass && pass->Tag() == tag;
                           });
    }

    void Renderer::EnsureVolumetricCloudPassInserted()
    {
        if (!m_volumetricCloudRenderPass ||
            !m_settings.volumetricCloudEnabled ||
            HasRenderPass("VolumetricCloud")) {
            return;
        }

        m_volumetricCloudRenderPass->SetEnabled(true);
        if (AddPassAfter("Skybox", m_volumetricCloudRenderPass).IsValid()) return;
        if (AddPassAfter("ProceduralSky", m_volumetricCloudRenderPass).IsValid()) return;
        if (AddPassAfter("Lighting", m_volumetricCloudRenderPass).IsValid()) return;
        AddPass(m_volumetricCloudRenderPass);
    }

    void Renderer::EnsureMeshShaderPassInserted()
    {
        if (!m_meshShaderRenderPass ||
            !m_pipelineStateCache.GetMeshShaderPipelineState().Get() ||
            !m_pipelineStateCache.GetMeshShaderRootSignature().Get() ||
            HasRenderPass("MeshShader")) {
            return;
        }

        AddPassAfter("OpaqueGBuffer", m_meshShaderRenderPass);
    }

    void Renderer::EnsureCrowdPassInserted()
    {
        if (!m_crowdRenderPass ||
            !m_crowdRenderPass->IsInitialized() ||
            HasRenderPass("Crowd")) {
            return;
        }

        if (HasRenderPass("MeshShader") && AddPassAfter("MeshShader", m_crowdRenderPass).IsValid()) return;
        AddPassAfter("OpaqueGBuffer", m_crowdRenderPass);
    }

    void Renderer::EnsureWireframeOverlayPassInserted()
    {
        // Inserted only while enabled, so a renderer that never turns it on keeps its graph as is.
        if (!m_wireframeOverlayRenderPass ||
            !m_wireframeOverlayRenderPass->IsEnabled() ||
            HasRenderPass("WireframeOverlay")) {
            return;
        }

        // Before tone mapping, so the edges land in HDR SceneColor next to the lit image.
        if (HasRenderPass("PostProcess") && AddPassBefore("PostProcess", m_wireframeOverlayRenderPass).IsValid()) return;
        AddPass(m_wireframeOverlayRenderPass);
    }

    bool Renderer::AddPhaseCompletionNode(std::string_view phaseTag,
                                          std::string_view nodeName,
                                          const PhaseCompletionCallback& execute,
                                          PhaseCompletionMode mode,
                                          const RenderPassRequirements& requirements)
    {
        return m_passRegistry.AddPhaseCompletionNode(phaseTag, nodeName, execute, mode, requirements);
    }

    void Renderer::ClearPhaseCompletionNodes()
    {
        m_passRegistry.ClearPhaseCompletionNodes();
    }

    void Renderer::ClearPasses()
    {
        WaitForGPU();
        m_passRegistry.ClearPasses();
    }


    void Renderer::SetRenderPassSequence(const std::vector<RenderPassType>& sequence)
    {
        // Deferred lighting has a hard dependency on OpaqueGBuffer: Lighting samples the
        // GBuffer targets unconditionally, so a sequence without it renders from targets
        // nothing wrote. A caller-supplied sequence (the pass-builder UI) can legitimately
        // omit passes it does not expose, so restore this one rather than trusting the
        // list verbatim.
        std::vector<RenderPassType> effectiveSequence = sequence;
        const bool hasGBuffer = std::find(effectiveSequence.begin(), effectiveSequence.end(),
                                          RenderPassType::OpaqueGBuffer) != effectiveSequence.end();
        if (!hasGBuffer) {
            // Restore it at its default slot: before the passes that read it.
            const auto lightingIt = std::find(effectiveSequence.begin(), effectiveSequence.end(),
                                              RenderPassType::Lighting);
            const auto insertAt = (lightingIt != effectiveSequence.end()) ? lightingIt : effectiveSequence.begin();
            effectiveSequence.insert(insertAt, RenderPassType::OpaqueGBuffer);
            DebugLog("Renderer::SetRenderPassSequence: OpaqueGBuffer was missing from the "
                     "requested sequence and has been restored (deferred lighting requires it).\n");
        }

        m_passRegistry.SetRenderPassSequence(effectiveSequence);

        // Re-insert non-builtin nodes that were previously added via AddPassBefore/AddPassAfter.
        // SetRenderPassSequence calls ClearPasses() internally, which removes them.
        // Keep the probe grid after Lighting/Skybox so deferred PBR has already produced SceneColor.
        EnsureDebugProbeGridPassInserted();
        EnsureFluidSurfacePassInserted();
        EnsureParticlePassInserted();
        EnsureVolumetricCloudPassInserted();
        EnsureMeshShaderPassInserted();
        EnsureCrowdPassInserted();
        EnsureWireframeOverlayPassInserted();
    }

    void Renderer::EnsureDebugProbeGridPassInserted()
    {
        auto pass = m_debugVisualization.GetPass();
        if (!pass ||
            !pass->IsInitialized() ||
            HasRenderPass("DebugProbeGrid")) {
            return;
        }
        // After Lighting/Skybox so deferred PBR has already produced SceneColor.
        if (AddPassAfter("Skybox", pass).IsValid()) return;
        if (AddPassAfter("Lighting", pass).IsValid()) return;
        AddPass(pass);
    }

    void Renderer::ReinsertDebugProbeGrid()
    {
        EnsureDebugProbeGridPassInserted();
    }

    void Renderer::EnsureFluidSurfacePassInserted()
    {
        if (!m_fluidSurfaceRenderPass ||
            !m_fluidSurfaceRenderPass->IsInitialized() ||
            HasRenderPass("FluidSurface")) {
            return;
        }
        // After Lighting/Skybox so deferred PBR has already produced SceneColor.
        if (AddPassAfter("Skybox", m_fluidSurfaceRenderPass).IsValid()) {
            EnsureFluidBackdropCopyInserted();
            return;
        }
        if (AddPassAfter("Lighting", m_fluidSurfaceRenderPass).IsValid()) {
            EnsureFluidBackdropCopyInserted();
            return;
        }
        AddPass(m_fluidSurfaceRenderPass);
        EnsureFluidBackdropCopyInserted();
    }

    void Renderer::EnsureSmokeVolumePassInserted()
    {
        if (!m_smokeVolumeRenderPass ||
            !m_smokeVolumeRenderPass->IsInitialized() ||
            HasRenderPass("SmokeVolume")) {
            return;
        }
        // After the water so smoke drawn above a pool composites over it, and after Skybox for the
        // same reason the water needs it: SceneColor has to already hold the scene the volume
        // blends against.
        if (AddPassAfter("FluidSurface", m_smokeVolumeRenderPass).IsValid()) return;
        if (AddPassAfter("Skybox", m_smokeVolumeRenderPass).IsValid()) return;
        if (AddPassAfter("Lighting", m_smokeVolumeRenderPass).IsValid()) return;
        AddPass(m_smokeVolumeRenderPass);
    }

    void Renderer::EnsureParticleVolumePassInserted()
    {
        if (!m_particleVolumeRenderPass ||
            !m_particleVolumeRenderPass->IsInitialized() ||
            HasRenderPass("ParticleVolume")) {
            return;
        }
        // Same slot as the smoke volume: after the opaque scene and sky, before tone mapping.
        if (AddPassAfter("SmokeVolume", m_particleVolumeRenderPass).IsValid()) return;
        if (AddPassAfter("FluidSurface", m_particleVolumeRenderPass).IsValid()) return;
        if (AddPassAfter("Skybox", m_particleVolumeRenderPass).IsValid()) return;
        if (AddPassAfter("Lighting", m_particleVolumeRenderPass).IsValid()) return;
        AddPass(m_particleVolumeRenderPass);
    }

    void Renderer::EnsureFluidBackdropCopyInserted()
    {
        if (HasRenderPass("FluidBackdropCopy")) {
            return;
        }
        if (!m_fluidBackdropCopyRenderPass) {
            m_fluidBackdropCopyRenderPass = std::make_shared<FluidBackdropCopyRenderPass>();
        }
        // Immediately before the water, so the snapshot is the scene the water will refract and
        // nothing drawn after it leaks into the backdrop.
        if (AddPassBefore("FluidSurface", m_fluidBackdropCopyRenderPass).IsValid()) {
            return;
        }
        DebugLog("Renderer::EnsureFluidBackdropCopyInserted: could not place the copy before FluidSurface; refraction stays off.\n");
    }

    void Renderer::ReinsertFluidSurface()
    {
        EnsureFluidSurfacePassInserted();
    }

    void Renderer::EnsureParticlePassInserted()
    {
        if (!m_particleRenderPass ||
            !m_particleRenderPass->IsInitialized() ||
            HasRenderPass("Particles")) {
            return;
        }
        if (AddPassAfter("FluidSurface", m_particleRenderPass).IsValid()) return;
        if (AddPassAfter("Skybox", m_particleRenderPass).IsValid()) return;
        if (AddPassAfter("Lighting", m_particleRenderPass).IsValid()) return;
        AddPass(m_particleRenderPass);
    }

    void Renderer::ReinsertParticles()
    {
        EnsureParticlePassInserted();
    }
}
