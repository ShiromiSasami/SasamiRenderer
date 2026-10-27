#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Frame/RendererFrameCoordinator.h"
#include "Renderer/Scene/CameraState.h"
#include "Renderer/Scene/MeshBuffer.h"
#include "Renderer/Scene/SceneSubmitter.h"
#include "Renderer/Structures/RendererEnums.h"
#include "Renderer/Structures/Texture.h"
#include "Renderer/VirtualGeometry/ClusterDagGpuBuffer.h"
#include "Renderer/VirtualGeometry/ClusterGpuCulling.h"
#include "Renderer/VirtualGeometry/ClusterInstanceBuffer.h"
#include "Renderer/VirtualGeometry/ClusterMaterialTable.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Render
{
    class RenderPipelineStateCache;

    // SDF face shadow for the mesh-shader GBuffer PSs (ToonFaceShadow.hlsli). Copied into
    // CameraCB extra3..extra5, because those root signatures have no LightCB.
    struct ClusterToonFaceShadow
    {
        bool enabled = false;
        float toLight[3] = { 0.0f, 1.0f, 0.0f }; // unit vector towards the directional light
        float forward[3] = { 0.0f, 0.0f, 1.0f };
        float right[3] = { 1.0f, 0.0f, 0.0f };
        float softness = 0.0f;
        GpuDescriptorHandle mapSrv{};   // direct path, root [12]; 0 = fall back to defaultEmissive / null
        uint32_t mapSrvIndex = 0xFFFFFFFFu; // indirect path, bindless index; 0xFFFFFFFF = null texture
    };

    struct ClusterDrawInputs
    {
        IRhiCommandEncoder* encoder = nullptr;
        CommandList* cmdList = nullptr;
        RenderPipelineStateCache* pipelineStateCache = nullptr;
        DescriptorHeap* srvHeap = nullptr;
        const Viewport* viewport = nullptr;
        const Rect* scissorRect = nullptr;
        RendererFrameCoordinator* frameCoordinator = nullptr;
        RendererFrameCoordinator::FrameContext* frame = nullptr;
        const CameraState* camera = nullptr;
        const std::vector<SceneSubmitter::DrawItem>* drawItems = nullptr;
        const MeshBuffer* meshBuffer = nullptr;
        const ClusterDagGpuBuffer* clusterBuffer = nullptr;
        RendererEnums::AmbientOcclusionMode effectiveAoMode = RendererEnums::AmbientOcclusionMode::MaterialOnly;
        const Texture* defaultAlbedo = nullptr;
        const Texture* defaultOcclusion = nullptr;
        const Texture* defaultNormal = nullptr;
        const Texture* defaultEmissive = nullptr;
        GpuDescriptorHandle nullTextureSrv{};
        ClusterToonFaceShadow toonFace{};
        float lodThresholdPx = 1.0f;
        // Virtual-geometry debug colouring (0 = off). Main view only; shadow views have no PS.
        uint32_t debugMode = 0u;
    };

    struct ClusterDrawStats
    {
        uint32_t itemsConsidered = 0;
        uint32_t itemsDrawn = 0;
        uint32_t clustersDispatched = 0;
    };

    // Depth-only counterpart of ClusterDrawInputs for a single directional-shadow cascade.
    // The caller (LightSystem, via RenderPassExecutionServices::drawClusterShadowItems) has
    // already set the cascade DSV, viewport and scissor before invoking DrawShadow.
    struct ClusterShadowDrawInputs
    {
        IRhiCommandEncoder* encoder = nullptr;
        CommandList* cmdList = nullptr;
        RenderPipelineStateCache* pipelineStateCache = nullptr;
        RendererFrameCoordinator* frameCoordinator = nullptr;
        RendererFrameCoordinator::FrameContext* frame = nullptr;
        const std::vector<SceneSubmitter::DrawItem>* drawItems = nullptr;
        const MeshBuffer* meshBuffer = nullptr;
        const ClusterDagGpuBuffer* clusterBuffer = nullptr;
        const float* lightViewProjection = nullptr; // row-major float[16] of the active cascade
        float texelsPerWorldUnit = 0.0f;             // shadow-map texels per world unit for this cascade
        float lodThresholdPx = 1.0f;                 // in shadow-map texels
    };

    // GPU-driven counterpart of ClusterShadowDrawInputs for a single directional-shadow
    // cascade: the compute cluster-selection pass (ClusterGpuCulling) replaces the CPU
    // frustum-cull/DispatchMesh loop, and an ExecuteIndirect call replaces the per-item
    // DispatchMesh submission. One instance/view slot per cascade.
    struct ClusterShadowIndirectInputs
    {
        CommandList* cmdList = nullptr;
        RenderPipelineStateCache* pipelineStateCache = nullptr;
        DescriptorHeap* srvHeap = nullptr;
        RendererFrameCoordinator* frameCoordinator = nullptr;
        RendererFrameCoordinator::FrameContext* frame = nullptr;
        ClusterGpuCulling* culling = nullptr;
        const ClusterInstanceBuffer* instances = nullptr;
        const ClusterDagGpuBuffer* clusterBuffer = nullptr;
        uint32_t frameIndex = 0;
        uint32_t viewSlot = 0;
        const float* lightViewProjection = nullptr; // row-major float[16] of the active cascade
        float texelsPerWorldUnit = 0.0f;             // shadow-map texels per world unit for this cascade
        float lodThresholdPx = 1.0f;                 // in shadow-map texels
        D3D12_GPU_VIRTUAL_ADDRESS pageOffsetTableVA = 0; // per-group page byte offset in the pool; 0xFFFFFFFF = not resident
        D3D12_GPU_VIRTUAL_ADDRESS pagePoolVA = 0;        // GPU page pool backing the resident cluster/vertex/triangle data
        bool selectOnly = false;
        bool collectStats = false;  // opt in to the select-CS traversal counters (render.clusterselectstats)                         // diagnostic: select, but skip the draw (see RenderSettings)
    };

    // GPU-driven counterpart of ClusterDrawInputs for the camera's GBuffer view: the same
    // ClusterGpuCulling select pass the cascades use (viewSlot ClusterGpuCulling::kMainViewSlot,
    // perspective LOD/frustum test), then one ExecuteIndirect DispatchMesh with the GBuffer MS/PS.
    // Per-instance materials come from ClusterMaterialTable (parallel to the instance list) and
    // textures through the bindless heap, instead of per-draw CBs and descriptor tables.
    // The caller has already bound the GBuffer render targets and depth (MeshShaderRenderPass).
    struct ClusterMainIndirectInputs
    {
        IRhiCommandEncoder* encoder = nullptr;
        CommandList* cmdList = nullptr;
        RenderPipelineStateCache* pipelineStateCache = nullptr;
        DescriptorHeap* srvHeap = nullptr;
        const Viewport* viewport = nullptr;
        const Rect* scissorRect = nullptr;
        RendererFrameCoordinator* frameCoordinator = nullptr;
        RendererFrameCoordinator::FrameContext* frame = nullptr;
        const CameraState* camera = nullptr;
        ClusterGpuCulling* culling = nullptr;
        const ClusterInstanceBuffer* instances = nullptr;
        const ClusterMaterialTable* materials = nullptr;
        const ClusterDagGpuBuffer* clusterBuffer = nullptr;
        uint32_t frameIndex = 0;
        uint32_t viewSlot = ClusterGpuCulling::kMainViewSlot;
        float lodThresholdPx = 1.0f;                     // in screen pixels
        uint32_t debugMode = 0u;                         // render.clusterdebug, as in ClusterDrawInputs
        uint32_t nullTextureSrvIndex = 0;                // bindless heap index of the null Texture2D SRV
        ClusterToonFaceShadow toonFace{};
        D3D12_GPU_VIRTUAL_ADDRESS pageOffsetTableVA = 0; // per-group page byte offset in the pool; 0xFFFFFFFF = not resident
        D3D12_GPU_VIRTUAL_ADDRESS pagePoolVA = 0;        // GPU page pool backing the resident cluster/vertex/triangle data
        bool collectStats = false;                       // render.clusterselectstats
    };

    // Per-draw submission loop for the virtual-geometry mesh-shader path. Frustum-culls the
    // static opaque DrawItems, packs the same MaterialConstants layout the VS OpaqueGBuffer
    // loop uses into the per-draw CameraCB (root [5]), binds the cluster/vertex buffers of the
    // mesh-shader root signature (root [0]/[1]/[2]/[9]/[10]) and the per-draw model/cluster-range
    // root constants (root [3]), then dispatches one amplification-shader threadgroup per 64
    // clusters via DispatchMesh.
    class ClusterDrawSubmitter
    {
    public:
        static ClusterDrawStats Draw(const ClusterDrawInputs& in);

        // Depth-only variant used by the directional-shadow cascades. Same cluster culling and
        // DispatchMesh submission as Draw, but with the shadow AS/MS PSO (no PS, no textures, no
        // material CB) and the cascade's light-space view-projection instead of the camera's.
        static ClusterDrawStats DrawShadow(const ClusterShadowDrawInputs& in);

        // GPU-driven variant of DrawShadow: one ClusterGpuCulling::RecordSelect compute
        // dispatch followed by one ExecuteIndirect DispatchMesh call, instead of the CPU
        // frustum-cull loop and one DispatchMesh per DrawItem.
        static ClusterDrawStats DrawShadowIndirect(const ClusterShadowIndirectInputs& in);

        // GPU-driven variant of Draw for the main view (see ClusterMainIndirectInputs).
        static ClusterDrawStats DrawMainIndirect(const ClusterMainIndirectInputs& in);
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
