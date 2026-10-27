#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Scene/SceneSubmitter.h"
#include "Renderer/Structures/RendererEnums.h"
#include "Renderer/VirtualGeometry/ClusterMaterialTable.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render
{
    class ClusterDagGpuBuffer;
    class ClusterInstanceBuffer;
    class ClusterPagePool;
    class ClusterPageStore;
    class ClusterResidencyManager;
    class ClusterStreamingFeedback;
    class MeshBuffer;
    class SrvDescriptorAllocator;

    // Everything ClusterFrameUpdate::Begin reads or writes, as one struct of pointers -- the same
    // shape ClusterDrawInputs / ClusterShadowIndirectInputs in this directory already use, so the
    // per-frame step is wired up the same way a draw submission is.
    struct ClusterFrameUpdateInputs
    {
        GraphicsDevice* device = nullptr;
        CommandList*    cmdList = nullptr;          // the page-pool copies are recorded here
        SrvDescriptorAllocator* srvAllocator = nullptr;
        const MeshBuffer* meshBuffer = nullptr;
        const std::vector<SceneSubmitter::DrawItem>* drawItems = nullptr;
        const ClusterDagGpuBuffer* clusterBuffer = nullptr;
        const ClusterPageStore*    pageStore = nullptr;
        ClusterInstanceBuffer*   instances = nullptr;
        ClusterMaterialTable*    materials = nullptr;   // optional: per-instance material table
        ClusterMaterialDefaults  materialDefaults{};
        RendererEnums::AmbientOcclusionMode aoMode{};
        ClusterResidencyManager* residency = nullptr;
        ClusterPagePool*         pagePool = nullptr;
        ClusterStreamingFeedback* feedback = nullptr;
        uint32_t frameIndex = 0;          // back-buffer slot: picks the per-frame instance and offset-table buffers
        uint64_t frameCounter = 0;        // monotonic frame counter (once-per-frame guard, residency timestamps)
        uint32_t streamBudgetGroups = 0;  // RenderSettings::clusterStreamBudgetGroups
    };

    // The per-frame half of the GPU-driven cluster path: the work that is a property of the frame
    // rather than of a view, and so has to happen exactly once, before any view is drawn.
    //
    // Begin refreshes the instance list, keeps the residency manager and the page pool sized to the
    // current DAG, uploads this frame's resident set into the pool and publishes the offset table
    // every view then reads; End closes the frame's streaming feedback and hands the merged request
    // set to the residency manager. Each mechanism stays in its own class (ClusterInstanceBuffer,
    // ClusterResidencyManager, ClusterPagePool, ClusterStreamingFeedback); what this class owns is
    // the once-per-frame sequencing and the small amount of state that sequencing needs.
    //
    // This used to sit inside the directional-shadow cluster draw, under "render.clustergpucull is
    // on" and "this is cascade 0". It therefore only ran if a shadow pass ran with GPU culling
    // enabled, which the pass order (Shadow -> OpaqueGBuffer -> MeshShader) made true by accident.
    // Any other GPU-driven view -- the main view, next -- would have read a stale offset table.
    class ClusterFrameUpdate
    {
    public:
        // Idempotent within a frame: the instance refresh is guarded by in.frameCounter, so calling
        // Begin twice in one frame does not rewrite the slot the GPU may already be reading.
        void Begin(const ClusterFrameUpdateInputs& in);

        // Pairs with Begin and does nothing if Begin did not open this frame (the cluster path is
        // off, or a required subsystem is not initialized), so the residency set is only ever
        // re-decided from a feedback frame that was actually opened.
        void End(ClusterStreamingFeedback& feedback, ClusterResidencyManager& residency, uint64_t frameCounter);

        // Valid for the whole frame after Begin: every view's cluster-select dispatch binds this.
        D3D12_GPU_VIRTUAL_ADDRESS GetPageOffsetTableVA() const { return m_pageOffsetTableVA; }

    private:
        D3D12_GPU_VIRTUAL_ADDRESS m_pageOffsetTableVA = 0;
        uint32_t m_instanceFrame = UINT32_MAX;   // frameCounter of the last instance-list refresh
        uint32_t m_residencyGroupCount = 0;      // group count the residency manager and pool are sized for
        bool     m_frameOpen = false;            // Begin ran this frame, so End has something to close
    };
}

namespace SasamiEngine { using namespace Render; }
