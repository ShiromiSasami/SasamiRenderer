#pragma once
#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Frame/RendererFrameCoordinator.h"
#include "Renderer/Scene/DeferredUploadSink.h"
#include "Renderer/Scene/RenderProxy.h"
#include "Renderer/Scene/SurfaceMaterial.h"
#include "Renderer/Structures/Texture.h"
#include "Renderer/Structures/Mesh.h"
#include "Renderer/VirtualGeometry/ClusterDagGpuBuffer.h"
#include "Renderer/VirtualGeometry/ClusterDagStore.h"
#include "Renderer/VirtualGeometry/ClusterPageStore.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace SasamiEngine::Render
{
    class MeshBuffer;
    class SkinnedMeshBuffer;
    struct RayTracingScene;
    class DxrRayTracer;

    // Whether an 8-bit texture's RGB stores sRGB-encoded values that the sampler must decode,
    // or linear data that must be read verbatim. Base colour is authored in sRGB; normal,
    // metallic-roughness and occlusion maps are linear measurements and must never be decoded.
    enum class TextureColorSpace { Linear, Srgb };

    // Owns the per-frame draw list, mesh array, and GPU texture cache.
    // Translates application-facing RenderProxy submissions into DrawItems
    // and RayTracingScene entries.
    class SceneSubmitter
    {
    public:
        // Render-thread draw item: resolved texture pointers + material + mesh index.
        struct DrawItem
        {
            size_t         meshIndex        = 0;
            Texture*       texture          = nullptr;
            Texture*       occlusionTexture = nullptr;
            Texture*       normalTexture    = nullptr;
            Texture*       emissiveTexture  = nullptr;
            bool           usesMetallicRoughnessTexture = false;
            SurfaceMaterial material{};
            bool           transparent      = false;
            bool           dynamicVertices  = false; // see RenderProxy::dynamicVertices
            std::string    debugLabel;
            float          model[16] = {
                1,0,0,0,
                0,1,0,0,
                0,0,1,0,
                0,0,0,1,
            };

            // World-space AABB of this item, used for frustum culling.
            // Always populated, independent of whether ray tracing is enabled.
            float worldBoundsMin[3] = { 0.0f, 0.0f, 0.0f };
            float worldBoundsMax[3] = { 0.0f, 0.0f, 0.0f };
            // False when the mesh has no vertices (AABB undefined). Culling code must
            // treat such items as always-visible rather than degrading them to a
            // zero-size box at the origin, which would wrongly cull them away.
            bool  hasWorldBounds    = false;
        };

        using SrvAllocFn  = std::function<bool(UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu)>;
        using SrvIndexFn  = std::function<UINT(GpuDescriptorHandle handle)>;

        // Per-frame GPU draw item for a skinned mesh
        struct SkinnedDrawItem
        {
            size_t          meshIndex           = 0;
            Texture*        texture             = nullptr;
            Texture*        occlusionTexture    = nullptr;
            Texture*        normalTexture       = nullptr;
            Texture*        emissiveTexture     = nullptr;
            SurfaceMaterial material{};
            bool            transparent         = false;
            RhiGpuAddress   boneMatricesCbGpu   = 0; // GPU VA of the per-draw bone CB slot (DX12 CB-upload path)
            std::vector<float> boneMatricesNative; // Skeleton::kMaxBones * 16 floats (native backend path, no CB indirection)
            float           model[16] = {
                1,0,0,0,
                0,1,0,0,
                0,0,1,0,
                0,0,0,1,
            };
        };

        struct InitParams
        {
            IRHIDevice*       device             = nullptr;
            MeshBuffer*       meshBuffer         = nullptr;
            SkinnedMeshBuffer* skinnedMeshBuffer = nullptr;
            RayTracingScene*  rayTracingScene    = nullptr;
            DxrRayTracer*     dxrRayTracer       = nullptr;
            SrvAllocFn        srvAllocFn;
            SrvIndexFn        srvIndexFn;
            // Optional: receives executed texture-upload batches for fence-based retirement.
            // When absent, the submitter falls back to a blocking WaitForGPU per batch.
            DeferredUploadSink deferredUploadSink;
        };

        ~SceneSubmitter();

        void Initialize(const InitParams& params);
        void ReleaseTextures();

        void SubmitRenderProxies(std::vector<RenderProxy>&& proxies);
        void ClearSubmittedRenderProxies();

        // Upload skinned proxies: evaluates bone matrices and builds the SkinnedDrawItem list.
        // Call once per frame after AnimationController::Update().
        // frameCoord/frame are non-null on the DX12 feature-render-pass path (bone matrices are
        // uploaded to a per-frame CB ring buffer); pass nullptr for both on native backends
        // (OpenGL/Vulkan/DX11), where bone matrices are instead copied into
        // SkinnedDrawItem::boneMatricesNative for the caller to bind directly.
        void SubmitSkinnedRenderProxies(std::vector<SkinnedRenderProxy>&& proxies,
                                        RendererFrameCoordinator* frameCoord = nullptr,
                                        RendererFrameCoordinator::FrameContext* frame = nullptr);
        void ClearSkinnedRenderProxies();

        const std::vector<DrawItem>&        GetDrawItems()        const { return m_drawItems; }
        const std::vector<Mesh>&            GetMeshes()           const { return m_meshes; }
        const std::vector<SkinnedDrawItem>& GetSkinnedDrawItems() const { return m_skinnedDrawItems; }
        const std::vector<SkinnedMesh>&     GetSkinnedMeshes()    const { return m_skinnedMeshes; }

        // Lazily builds the cluster DAGs (virtual geometry LOD hierarchies) for the submitted
        // meshes, cached under cacheDirectory. Builds lazily on first access and is
        // invalidated whenever the submitted geometry changes.
        const ClusterDagStore& EnsureClusterDagsBuilt(const std::filesystem::path& cacheDirectory);

        // Same as above, using the directory set via SetClusterDagCacheDirectory (empty by
        // default, which ClusterDagCache treats as an always-miss cache).
        const ClusterDagStore& EnsureClusterDagsBuilt();

        void SetClusterDagCacheDirectory(std::filesystem::path directory) { m_clusterDagCacheDirectory = std::move(directory); }
        const std::filesystem::path& GetClusterDagCacheDirectory() const { return m_clusterDagCacheDirectory; }

        // Lazily packs and uploads the cluster DAGs into GPU buffers for the mesh-shader
        // virtual-geometry path. Builds the cluster DAGs first (EnsureClusterDagsBuilt with the
        // stored cache directory). Same lazy build-once-and-cache lifecycle as above.
        const ClusterDagGpuBuffer& EnsureClusterDagGpuBuilt();
        // Group pages (streaming unit) of the current cluster data; valid after EnsureClusterDagGpuBuilt().
        const ClusterPageStore& GetClusterPageStore() const { return m_clusterPageStore; }

    private:
        // One command list shared by all texture uploads of a single SubmitRenderProxies
        // call; opened lazily on the first cache miss, executed once after the proxy loop.
        struct TextureUploadBatch
        {
            CommandAllocator      allocator;
            CommandList           commandList;
            std::vector<Resource> uploads;
            bool                  open = false;
        };

        Texture* CreateTextureFromRgba8Data(const CpuTextureRgba8& src,
                                            TextureColorSpace colorSpace,
                                            CommandList* cmdList,
                                            std::vector<Resource>& uploads);
        Texture* ResolveSceneTexture(const std::shared_ptr<const CpuTextureRgba8>& textureData,
                                     TextureColorSpace colorSpace,
                                     TextureUploadBatch& batch);
        void FlushTextureUploadBatch(TextureUploadBatch& batch);

        // Local-space AABB of a single mesh, cached in lock-step with m_meshes (same
        // index) so repeated draw items referencing the same mesh don't re-walk its
        // vertex data. Invalidated together with m_meshes.
        struct MeshLocalBounds
        {
            float min[3] = { 0.0f, 0.0f, 0.0f };
            float max[3] = { 0.0f, 0.0f, 0.0f };
            bool  valid  = false;
        };

        IRHIDevice*       m_device             = nullptr;
        MeshBuffer*       m_meshBuffer         = nullptr;
        SkinnedMeshBuffer* m_skinnedMeshBuffer = nullptr;
        RayTracingScene*  m_rayTracingScene    = nullptr;
        DxrRayTracer*     m_dxrRayTracer       = nullptr;
        SrvAllocFn        m_srvAllocFn;
        SrvIndexFn        m_srvIndexFn;
        DeferredUploadSink m_deferredUploadSink;

        std::vector<DrawItem>                       m_drawItems;
        // Extent of the submitted object placements, reported once per submission. Backend
        // agnostic on purpose: it is the reference point when two backends disagree about the
        // world they are drawing.
        float m_submittedTranslationMin[3] = { 1e30f, 1e30f, 1e30f };
        float m_submittedTranslationMax[3] = { -1e30f, -1e30f, -1e30f };
        std::vector<Mesh>                           m_meshes;
        std::vector<MeshLocalBounds>                 m_meshLocalBoundsCache;
        std::vector<uint8_t>                         m_meshDynamic;   // lock-step with m_meshes; 1 = RenderProxy::dynamicVertices
        ClusterDagStore                             m_clusterDagStore;
        bool                                         m_clusterDagsDirty  = true;
        ClusterDagGpuBuffer                         m_clusterDagGpuBuffer;
        ClusterPageStore                            m_clusterPageStore;   // group pages built from the GPU buffer's CPU arrays
        bool                                         m_clusterDagGpuDirty = true;
        std::filesystem::path                       m_clusterDagCacheDirectory;
        std::vector<SkinnedDrawItem>                m_skinnedDrawItems;
        std::vector<SkinnedMesh>                    m_skinnedMeshes;
        std::vector<uint64_t>                       m_residentSkinnedMeshIds;
        std::vector<std::unique_ptr<Texture>>       m_sceneTextures;
        std::unordered_map<uint64_t, Texture*>      m_textureCache;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
