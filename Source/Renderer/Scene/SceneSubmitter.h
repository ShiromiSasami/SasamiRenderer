#pragma once
#include "Renderer/Core/GraphicsDevice.h"
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
        };

        using SrvAllocFn  = std::function<bool(UINT count, CpuDescriptorHandle& outCpu, GpuDescriptorHandle& outGpu)>;
        using SrvIndexFn  = std::function<UINT(GpuDescriptorHandle handle)>;

        struct InitParams
        {
            IRHIDevice*      device          = nullptr;
            MeshBuffer*      meshBuffer      = nullptr;
            RayTracingScene* rayTracingScene = nullptr;
            DxrRayTracer*    dxrRayTracer    = nullptr;
            SrvAllocFn       srvAllocFn;
            SrvIndexFn       srvIndexFn;
        };

        void Initialize(const InitParams& params);

        void SubmitRenderProxies(std::vector<RenderProxy>&& proxies);
        void ClearSubmittedRenderProxies();

        const std::vector<DrawItem>& GetDrawItems() const { return m_drawItems; }
        const std::vector<Mesh>&     GetMeshes()    const { return m_meshes; }

    private:
        Texture* CreateTextureFromRgba8Data(const CpuTextureRgba8& src,
                                            CommandList* cmdList,
                                            std::vector<Resource>& uploads);
        Texture* ResolveSceneTexture(const std::shared_ptr<const CpuTextureRgba8>& textureData);

        IRHIDevice*      m_device          = nullptr;
        MeshBuffer*      m_meshBuffer      = nullptr;
        RayTracingScene* m_rayTracingScene = nullptr;
        DxrRayTracer*    m_dxrRayTracer    = nullptr;
        SrvAllocFn       m_srvAllocFn;
        SrvIndexFn       m_srvIndexFn;

        std::vector<DrawItem>                       m_drawItems;
        std::vector<Mesh>                           m_meshes;
        std::vector<std::unique_ptr<Texture>>       m_sceneTextures;
        std::unordered_map<uint64_t, Texture*>      m_textureCache;
    };
}
