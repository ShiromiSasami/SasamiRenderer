#pragma once
#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Frame/RendererFrameCoordinator.h"
#include "Renderer/Scene/DeferredUploadSink.h"
#include "Renderer/Scene/MeshletBuffer.h"
#include "Renderer/Scene/RenderProxy.h"
#include "Renderer/Scene/SurfaceMaterial.h"
#include "Renderer/Structures/Texture.h"
#include "Renderer/Structures/Mesh.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace SasamiRenderer
{
    class MeshBuffer;
    class SkinnedMeshBuffer;
    struct RayTracingScene;
    class DxrRayTracer;

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
            bool           usesMetallicRoughnessTexture = false;
            SurfaceMaterial material{};
            bool           transparent      = false;
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

        // Meshlet data for the (not yet wired) mesh-shader path. SubmitRenderProxies only
        // marks this stale -- it never builds/uploads meshlets itself -- so scenes that
        // never call this pay nothing extra at load time. The first caller that actually
        // needs meshlet data (e.g. a future mesh-shader render pass) pays the one-time
        // Build()+Upload() cost; later calls are a no-op until the mesh list changes again.
        const MeshletBuffer& EnsureMeshletsBuilt();

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
                                            CommandList* cmdList,
                                            std::vector<Resource>& uploads);
        Texture* ResolveSceneTexture(const std::shared_ptr<const CpuTextureRgba8>& textureData,
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
        std::vector<Mesh>                           m_meshes;
        std::vector<MeshLocalBounds>                 m_meshLocalBoundsCache;
        MeshletBuffer                               m_meshletBuffer;
        bool                                         m_meshletsDirty     = true;
        std::vector<SkinnedDrawItem>                m_skinnedDrawItems;
        std::vector<SkinnedMesh>                    m_skinnedMeshes;
        std::vector<uint64_t>                       m_residentSkinnedMeshIds;
        std::vector<std::unique_ptr<Texture>>       m_sceneTextures;
        std::unordered_map<uint64_t, Texture*>      m_textureCache;
    };
}
