// RendererClusterFrame.cpp
// Renderer's two calls into ClusterFrameUpdate -- the once-per-frame preparation of the
// GPU-driven virtual-geometry path -- and nothing else. Its single responsibility is binding
// Renderer's subsystems to ClusterFrameUpdateInputs; the step's own logic and state live in
// Renderer/VirtualGeometry/ClusterFrameUpdate.cpp, and the decision of *when* in the frame to
// call these lives in RendererFrameGraph.cpp (Renderer::Render), which owns frame ordering.
//
// This used to be inlined in the directional-shadow cluster draw service
// (RendererPassServices.cpp), gated on render.clustergpucull being on and on the cascade index
// being 0. The pass order -- Shadow before OpaqueGBuffer before MeshShader -- made that
// accidentally sufficient for the only GPU-driven view there was. It is not sufficient for a
// second one, and it breaks outright with the CPU-driven cluster path or with no shadow pass.
#include "Renderer/Runtime/Renderer.h"

#include "Renderer/VirtualGeometry/ClusterDagGpuBuffer.h"

namespace SasamiEngine::Render
{
    void Renderer::BeginClusterFrameUpdate(CommandList* cmdList, UINT backIndex, bool clusterPathActive)
    {
        // clusterPathActive is the cluster path's own gate (RenderPassExecutionPolicy::useMeshShader):
        // with no mesh-shader pipeline nothing draws clusters at all, and EnsureClusterDagGpuBuilt
        // below would build and upload a scene-wide DAG no pass would ever read. It is deliberately
        // NOT the GPU-culling toggle: the CPU-driven cluster path draws the same clusters and must
        // leave the frame's instance list, resident set and offset table in the same state.
        if (!clusterPathActive || !cmdList || !m_device) {
            return;
        }

        ClusterFrameUpdateInputs in{};
        in.device             = m_device.get();
        in.cmdList            = cmdList;
        in.srvAllocator       = &m_srvAllocator;
        in.meshBuffer         = &m_meshBuffer;
        in.drawItems          = &m_sceneSubmitter.GetDrawItems();
        in.clusterBuffer      = &m_sceneSubmitter.EnsureClusterDagGpuBuilt();
        in.pageStore          = &m_sceneSubmitter.GetClusterPageStore();
        in.instances          = &m_clusterInstanceBuffer;
        in.materials          = &m_clusterMaterialTable;
        in.materialDefaults   = { m_defaultAlbedoTexture, m_defaultOcclusionTexture, m_defaultNormalTexture, m_defaultEmissiveTexture };
        in.aoMode             = GetEffectiveAmbientOcclusionMode();
        in.residency          = &m_clusterResidency;
        in.pagePool           = &m_clusterPagePool;
        in.feedback           = &m_clusterStreamingFeedback;
        in.frameIndex         = backIndex;
        in.frameCounter       = m_gpuProfilerFrameCounter;
        in.streamBudgetGroups = m_settings.clusterStreamBudgetGroups;
        m_clusterFrameUpdate.Begin(in);
    }

    void Renderer::EndClusterFrameUpdate()
    {
        m_clusterFrameUpdate.End(m_clusterStreamingFeedback, m_clusterResidency, m_gpuProfilerFrameCounter);
    }
}
