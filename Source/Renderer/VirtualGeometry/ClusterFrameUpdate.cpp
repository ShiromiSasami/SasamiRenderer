#define NOMINMAX
#include "Renderer/VirtualGeometry/ClusterFrameUpdate.h"

#include "Renderer/Scene/MeshBuffer.h"
#include "Renderer/VirtualGeometry/ClusterDagGpuBuffer.h"
#include "Renderer/VirtualGeometry/ClusterInstanceBuffer.h"
#include "Renderer/VirtualGeometry/ClusterPagePool.h"
#include "Renderer/VirtualGeometry/ClusterPageStore.h"
#include "Renderer/VirtualGeometry/ClusterResidencyManager.h"
#include "Renderer/VirtualGeometry/ClusterStreamingFeedback.h"

namespace SasamiEngine::Render
{
    void ClusterFrameUpdate::Begin(const ClusterFrameUpdateInputs& in)
    {
        if (!in.device || !in.cmdList || !in.srvAllocator || !in.meshBuffer || !in.drawItems ||
            !in.clusterBuffer || !in.pageStore || !in.instances || !in.residency || !in.pagePool ||
            !in.feedback) {
            return;
        }
        // A validity check, not a mode check: there is nothing to write into before the buffers
        // exist. Deliberately not gated on render.clustergpucull, on the indirect shadow PSO or on
        // a cascade index -- those decide how a *view* is drawn, not whether the frame needs
        // preparing, and gating on them is exactly what tied every GPU-driven view to the
        // directional-shadow pass.
        if (!in.instances->IsValid() || !in.pagePool->IsValid()) {
            return;
        }

        // The instance list and its vertex SRVs do not depend on any view, so they are refreshed
        // once per frame. The guard keeps that true even if Begin is called more than once in a
        // frame: rewriting the slot the GPU may still be reading is the thing to avoid.
        const uint32_t frameCounter32 = static_cast<uint32_t>(in.frameCounter);
        if (m_instanceFrame != frameCounter32) {
            in.instances->EnsureVertexSrvs(*in.device, *in.srvAllocator, *in.meshBuffer);
            in.instances->Update(in.frameIndex, *in.drawItems, *in.clusterBuffer);
            if (in.materials && in.materials->IsValid()) {
                in.materials->Update(in.frameIndex, *in.drawItems, in.instances->GetSourceItemIndices(),
                                     *in.srvAllocator, in.materialDefaults, in.aoMode);
            }
            m_instanceFrame = frameCounter32;
        }

        // (Re)pin the terminal/root groups when the DAG has been rebuilt, then apply this frame's
        // resident set to the page pool (copying in newly-resident pages and refreshing the offset
        // table) so every view below reads the same table. The set itself is decided once per
        // frame, by End().
        const uint32_t totalGroups = in.clusterBuffer->GetTotalGroupCount();
        if (m_residencyGroupCount != totalGroups) {
            in.residency->Reset(totalGroups, in.clusterBuffer->BuildTerminalGroupBits());
            in.residency->SetBudget(in.streamBudgetGroups);
            m_residencyGroupCount = totalGroups;
            in.pagePool->Reset(totalGroups);
        }
        in.pagePool->ApplyResidency(*in.cmdList, in.frameIndex, in.residency->GetResidentBits(), *in.pageStore);
        m_pageOffsetTableVA = in.pagePool->GetOffsetTableVA(in.frameIndex);

        in.feedback->BeginFrame(totalGroups);
        // The instance buffer grows itself to fit the draw list, so this only fires when the growth
        // could not be satisfied. Routed into overflowFlags rather than left in the log, so
        // render.streamstats reports a short instance list the same way it reports a full visible
        // list. Read straight after Update above, whose result it describes.
        if (in.instances->HasOverflowed() || (in.materials && in.materials->HasOverflowed())) {
            in.feedback->AccumulateCpuOverflow(ClusterStreamingFeedback::kOverflowInstanceCapacity);
        }
        m_frameOpen = true;
    }

    void ClusterFrameUpdate::End(ClusterStreamingFeedback& feedback,
                                 ClusterResidencyManager& residency,
                                 uint64_t frameCounter)
    {
        if (!m_frameOpen) {
            return;
        }
        m_frameOpen = false;

        feedback.EndFrame();
        // The resulting resident set only takes effect next frame (it is uploaded by the next
        // Begin); with clusterStreamBudgetGroups == 0 every group stays resident, so the shader
        // behaves exactly as before this feature existed.
        residency.Update(feedback.GetWantedBits(), frameCounter);
    }
}
