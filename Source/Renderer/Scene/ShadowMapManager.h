#pragma once
#include "Renderer/RHI/GraphicsDevice.h"
#include <functional>

namespace SasamiRenderer
{
    class ShadowMapManager
    {
    public:
        static constexpr uint32_t kDirectionalCascadeCount = 4u;
        static constexpr uint32_t kPointShadowFaceCount = 6u;
        // 影を落とせるスポット/ポイントライトの最大数。LightCBLayout::kMaxSpotShadows /
        // kMaxPointShadows（LightCBData.h）と一致させる必要がある。
        static constexpr uint32_t kMaxSpotShadows = 8u;
        static constexpr uint32_t kMaxPointShadows = 4u;

        using AllocateSrvRangeCallback = std::function<bool(UINT count,
                                                            CpuDescriptorHandle& outCpu,
                                                            GpuDescriptorHandle& outGpu)>;

        ~ShadowMapManager();

        bool Initialize(IRHIDevice& device, const AllocateSrvRangeCallback& allocateSrvRange);

        bool EnsureShadowResources();
        bool EnsureSpotShadowResources();
        bool EnsurePointShadowResources();
        bool EnsureVsmResources();

        GpuDescriptorHandle GetShadowSrv()      const { return m_shadowSrv; }
        GpuDescriptorHandle GetSpotShadowSrv()  const { return m_spotShadowSrv; }
        GpuDescriptorHandle GetPointShadowSrv() const { return m_pointShadowSrv; }
        GpuDescriptorHandle GetVsmSrv()         const { return m_vsmSrv; }

        CpuDescriptorHandle GetDirectionalCascadeDsv(uint32_t cascadeIndex) const;
        CpuDescriptorHandle GetVsmRtv(uint32_t cascadeIndex) const;
        CpuDescriptorHandle GetSpotDsv(uint32_t shadowIndex) const;
        CpuDescriptorHandle GetPointFaceDsv(uint32_t shadowIndex, uint32_t face) const;

        UINT GetShadowMapSize()      const { return m_shadowMapSize; }
        UINT GetSpotShadowMapSize()  const { return m_spotShadowMapSize; }
        UINT GetPointShadowMapSize() const { return m_pointShadowMapSize; }
        const Viewport& GetShadowViewport() const { return m_shadowViewport; }
        const Rect&     GetShadowScissor()  const { return m_shadowScissor; }

        // Raw resource access (needed by LightSystem::ExecuteShadowPass for barriers)
        Resource& GetShadowMap()      { return *GetShadowMapResource(); }
        Resource& GetSpotShadowMap()  { return *GetSpotShadowMapResource(); }
        Resource& GetPointShadowMap() { return *GetPointShadowMapResource(); }
        Resource& GetVsmMap()        { return *GetVsmMapResource(); }
        Resource& GetVsmMapTemp()    { return *GetVsmMapTempResource(); }
        DescriptorHeap& GetVsmBlurDescHeap() { return m_vsmBlurDescHeap; }

    private:
        Resource* GetShadowMapResource() const;
        Resource* GetSpotShadowMapResource() const;
        Resource* GetPointShadowMapResource() const;
        Resource* GetVsmMapResource() const;
        Resource* GetVsmMapTempResource() const;

        IRHIDevice* m_device = nullptr;

        Resource m_shadowMap;
        RhiTextureHandle m_shadowMapHandle{};
        Resource* m_shadowMapCompat = nullptr;
        DescriptorHeap m_dsvHeapShadow;
        CpuDescriptorHandle m_shadowSrvCpu{};
        GpuDescriptorHandle m_shadowSrv{};
        UINT m_shadowMapSize = 4096;
        Viewport m_shadowViewport{};
        Rect m_shadowScissor{};

        Resource m_spotShadowMap;   // Texture2DArray, kMaxSpotShadows スライス（1 ライトにつき 1 スライス）
        RhiTextureHandle m_spotShadowMapHandle{};
        Resource* m_spotShadowMapCompat = nullptr;
        DescriptorHeap m_spotDsvHeap;  // DSV heap: kMaxSpotShadows 個（スライスごとに 1 つ）
        CpuDescriptorHandle m_spotShadowSrvCpu{};
        GpuDescriptorHandle m_spotShadowSrv{};
        UINT m_spotShadowMapSize = 2048;
        bool m_spotShadowResourcesReady = false;

        Resource m_pointShadowMap;   // Texture2DArray, kMaxPointShadows * kPointShadowFaceCount スライス
                                      // （ライト単位でまとまり、ライト i は面 i*6..i*6+5 を占有する）
        RhiTextureHandle m_pointShadowMapHandle{};
        Resource* m_pointShadowMapCompat = nullptr;
        DescriptorHeap m_pointDsvHeap;  // DSV heap: kMaxPointShadows * kPointShadowFaceCount 個
        CpuDescriptorHandle m_pointShadowSrvCpu{};
        GpuDescriptorHandle m_pointShadowSrv{};
        UINT m_pointShadowMapSize = 2048;
        bool m_pointShadowResourcesReady = false;

        Resource m_vsmMap;          // R32G32_FLOAT, kShadowMapSize x kShadowMapSize, kDirectionalCascadeCount slices
        Resource m_vsmMapTemp;      // same format, ping-pong for blur
        RhiTextureHandle m_vsmMapHandle{};
        RhiTextureHandle m_vsmMapTempHandle{};
        Resource* m_vsmMapCompat = nullptr;
        Resource* m_vsmMapTempCompat = nullptr;
        DescriptorHeap m_vsmRtvHeap;       // RTV heap: kDirectionalCascadeCount RTVs for vsmMap
        DescriptorHeap m_vsmBlurDescHeap;  // shader-visible: 4 slots (SRV+UAV for H-pass, SRV+UAV for V-pass)
        CpuDescriptorHandle m_vsmSrvCpu{};  // CPU handle in main SRV heap (for lighting pass t13)
        GpuDescriptorHandle m_vsmSrv{};     // GPU handle for t13 binding
        bool m_vsmResourcesReady = false;
    };
}
