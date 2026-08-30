// =============================================================================
// ShadowMapManager.cpp
// GPU シャドウマップリソース（CSM 深度、スポットシャドウ、VSM）の管理実装。
//
// 【責務】
//   - シャドウマップテクスチャの遅延生成（EnsureXxx メソッド）
//   - DSV / RTV / SRV ディスクリプタヒープの所有と管理
//   - LightSystem が使用するリソースハンドルの提供
// =============================================================================

#include "Renderer/Scene/ShadowMapManager.h"

#include <cmath>

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "d3dx12.h"

namespace SasamiRenderer
{
    namespace
    {
        // DSV-only descriptor heap desc shared by the three shadow-resource Ensure*
        // methods below (only NumDescriptors differs between call sites).
        D3D12_DESCRIPTOR_HEAP_DESC MakeDsvHeapDesc(UINT numDescriptors)
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.NumDescriptors = numDescriptors;
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            return desc;
        }
    }

    ShadowMapManager::~ShadowMapManager()
    {
        if (m_device && m_shadowMapHandle.IsValid()) {
            m_device->DestroyRhiResource(m_shadowMapHandle);
        }
        if (m_device && m_spotShadowMapHandle.IsValid()) {
            m_device->DestroyRhiResource(m_spotShadowMapHandle);
        }
        if (m_device && m_vsmMapHandle.IsValid()) {
            m_device->DestroyRhiResource(m_vsmMapHandle);
        }
        if (m_device && m_vsmMapTempHandle.IsValid()) {
            m_device->DestroyRhiResource(m_vsmMapTempHandle);
        }
        if (m_device && m_pointShadowMapHandle.IsValid()) {
            m_device->DestroyRhiResource(m_pointShadowMapHandle);
        }
        m_shadowMapHandle = {};
        m_spotShadowMapHandle = {};
        m_vsmMapHandle = {};
        m_vsmMapTempHandle = {};
        m_pointShadowMapHandle = {};
    }

    Resource* ShadowMapManager::GetShadowMapResource() const
    {
        return m_shadowMapCompat ? m_shadowMapCompat : const_cast<Resource*>(&m_shadowMap);
    }

    Resource* ShadowMapManager::GetSpotShadowMapResource() const
    {
        return m_spotShadowMapCompat ? m_spotShadowMapCompat : const_cast<Resource*>(&m_spotShadowMap);
    }

    Resource* ShadowMapManager::GetVsmMapResource() const
    {
        return m_vsmMapCompat ? m_vsmMapCompat : const_cast<Resource*>(&m_vsmMap);
    }

    Resource* ShadowMapManager::GetVsmMapTempResource() const
    {
        return m_vsmMapTempCompat ? m_vsmMapTempCompat : const_cast<Resource*>(&m_vsmMapTemp);
    }

    Resource* ShadowMapManager::GetPointShadowMapResource() const
    {
        return m_pointShadowMapCompat ? m_pointShadowMapCompat : const_cast<Resource*>(&m_pointShadowMap);
    }

    // =========================================================================
    // Initialize
    // アプリ起動時に 1 回だけ呼ぶ。
    // シャドウマップ SRV のディスクリプタスロットを確保する。
    // まだテクスチャは作らない（EnsureShadowResources() で遅延生成）。
    // =========================================================================
    bool ShadowMapManager::Initialize(IRHIDevice& device, const AllocateSrvRangeCallback& allocateSrvRange)
    {
        m_device = &device;

        // --- シャドウマップ SRV スロットを予約 ---
        // まだテクスチャが存在しないので null リソースで SRV を登録し、
        // EnsureShadowResources() 呼び出し時に実際のリソースで上書きする。
        CpuDescriptorHandle shadowCpu{};
        GpuDescriptorHandle shadowGpu{};
        if (!allocateSrvRange || !allocateSrvRange(1, shadowCpu, shadowGpu)) {
            DebugLogDialog("ShadowMapManager::Initialize: SRV allocation failed for shadow map.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }
        m_shadowSrvCpu = shadowCpu;
        m_shadowSrv = shadowGpu;

        // Null リソースで仮登録（シェーダーアクセス時はサンプリングされないが
        // ディスクリプタが未初期化のままだと D3D12 バリデーションエラーになる）
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.ArraySize = kDirectionalCascadeCount;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        Resource nullResource;
        m_device->CreateShaderResourceView(nullResource, &srvDesc, shadowCpu);

        // --- スポットライトシャドウマップ SRV スロットを予約 ---
        CpuDescriptorHandle spotShadowCpu{};
        GpuDescriptorHandle spotShadowGpu{};
        if (!allocateSrvRange(1, spotShadowCpu, spotShadowGpu)) {
            DebugLogDialog("ShadowMapManager::Initialize: SRV allocation failed for spot shadow map.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }
        m_spotShadowSrvCpu = spotShadowCpu;
        m_spotShadowSrv = spotShadowGpu;

        // Null SRV で仮登録（R16_UNORM Texture2DArray、kMaxSpotShadows スライス分）
        // 実体のスポットシャドウマップは R16_TYPELESS/D16_UNORM で作られ SRV は R16_UNORM
        // （下の CreateSpotShadowMap を参照）。ここが R32_FLOAT だと、シャドウマップ未生成時の
        // プレースホルダと実体とでフォーマットが食い違う。
        D3D12_SHADER_RESOURCE_VIEW_DESC spotNullSrvDesc = {};
        spotNullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        spotNullSrvDesc.Format = DXGI_FORMAT_R16_UNORM;
        spotNullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        spotNullSrvDesc.Texture2DArray.MipLevels = 1;
        spotNullSrvDesc.Texture2DArray.ArraySize = kMaxSpotShadows;
        spotNullSrvDesc.Texture2DArray.FirstArraySlice = 0;
        Resource spotNullResource;
        m_device->CreateShaderResourceView(spotNullResource, &spotNullSrvDesc, spotShadowCpu);

        // --- ポイントライトシャドウキューブマップ SRV スロットを予約 ---
        CpuDescriptorHandle pointShadowCpu{};
        GpuDescriptorHandle pointShadowGpu{};
        if (!allocateSrvRange(1, pointShadowCpu, pointShadowGpu)) {
            DebugLogDialog("ShadowMapManager::Initialize: SRV allocation failed for point shadow map.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }
        m_pointShadowSrvCpu = pointShadowCpu;
        m_pointShadowSrv = pointShadowGpu;

        // Null SRV で仮登録（R16_UNORM Texture2DArray、kMaxPointShadows * 6 面分）
        D3D12_SHADER_RESOURCE_VIEW_DESC pointNullSrvDesc = {};
        pointNullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        pointNullSrvDesc.Format = DXGI_FORMAT_R16_UNORM;
        pointNullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        pointNullSrvDesc.Texture2DArray.MipLevels = 1;
        pointNullSrvDesc.Texture2DArray.ArraySize = kMaxPointShadows * kPointShadowFaceCount;
        pointNullSrvDesc.Texture2DArray.FirstArraySlice = 0;
        Resource pointNullResource;
        m_device->CreateShaderResourceView(pointNullResource, &pointNullSrvDesc, pointShadowCpu);

        // --- VSM シャドウマップ SRV スロットを予約（t13 用）---
        CpuDescriptorHandle vsmSrvCpu{};
        GpuDescriptorHandle vsmSrvGpu{};
        if (!allocateSrvRange(1, vsmSrvCpu, vsmSrvGpu)) {
            DebugLogDialog("ShadowMapManager::Initialize: SRV allocation failed for VSM shadow map.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }
        m_vsmSrvCpu = vsmSrvCpu;
        m_vsmSrv = vsmSrvGpu;

        // Null SRV で仮登録（R32G32_FLOAT Texture2DArray）
        D3D12_SHADER_RESOURCE_VIEW_DESC vsmNullSrvDesc = {};
        vsmNullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        vsmNullSrvDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
        vsmNullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        vsmNullSrvDesc.Texture2DArray.MipLevels = 1;
        vsmNullSrvDesc.Texture2DArray.ArraySize = kDirectionalCascadeCount;
        vsmNullSrvDesc.Texture2DArray.FirstArraySlice = 0;
        Resource vsmNullResource;
        m_device->CreateShaderResourceView(vsmNullResource, &vsmNullSrvDesc, vsmSrvCpu);

        // シャドウ描画用のビューポートとシザー矩形を設定
        m_shadowViewport = { 0.0f, 0.0f, static_cast<float>(m_shadowMapSize), static_cast<float>(m_shadowMapSize), 0.0f, 1.0f };
        m_shadowScissor = { 0, 0, static_cast<LONG>(m_shadowMapSize), static_cast<LONG>(m_shadowMapSize) };
        return true;
    }

    // =========================================================================
    // EnsureShadowResources
    // シャドウマップテクスチャと DSV を初回アクセス時に遅延生成する。
    // 既に生成済みなら何もしない。
    // =========================================================================
    bool ShadowMapManager::EnsureShadowResources()
    {
        if (!m_device) {
            return false;
        }

        // 既に有効なリソースがあればスキップ
        Resource* existingShadowMap = GetShadowMapResource();
        if (existingShadowMap && existingShadowMap->IsValid() && m_dsvHeapShadow.Get()) {
            return true;
        }

        ScopedPerfTimer perfTimer("ShadowMapManager::EnsureShadowResources");

        // --- シャドウマップテクスチャを作成 ---
        // DXGI_FORMAT_R32_TYPELESS で作り、DSV では D32_FLOAT、SRV では R32_FLOAT として使う。
        // ALLOW_DEPTH_STENCIL フラグが必要。
        D3D12_RESOURCE_DESC smDesc = {};
        smDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        smDesc.Width = m_shadowMapSize;
        smDesc.Height = m_shadowMapSize;
        smDesc.DepthOrArraySize = static_cast<UINT16>(kDirectionalCascadeCount);
        smDesc.MipLevels = 1;
        smDesc.Format = DXGI_FORMAT_R32_TYPELESS;  // DSV と SRV で異なるフォーマットを使うため
        smDesc.SampleDesc.Count = 1;
        smDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        smDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        // 最適クリア値を指定することで GPU ドライバが最適化できる
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f; // 遠方（未書き込み）は深度 1.0
        clear.DepthStencil.Stencil = 0;

        m_shadowMapHandle = {};
        m_shadowMapCompat = nullptr;
        if (m_device->GetCapabilities().supportsRhiResourceCreation) {
            RhiTextureDesc rhiDesc{};
            rhiDesc.dimension = RhiResourceDimension::Texture2D;
            rhiDesc.extent = { m_shadowMapSize, m_shadowMapSize, 1u };
            rhiDesc.mipLevels = 1u;
            rhiDesc.arrayLayers = kDirectionalCascadeCount;
            rhiDesc.format = RhiFormat::R32Typeless;
            rhiDesc.usage = RhiTextureUsageFlags::ShaderResource | RhiTextureUsageFlags::DepthStencil;
            rhiDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
            rhiDesc.initialState = RhiResourceState::ShaderResource;
            m_shadowMapHandle = m_device->CreateRhiTexture(rhiDesc);
            m_shadowMapCompat = m_device->GetD3D12CompatibilityResource(m_shadowMapHandle);
        }

        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = S_OK;
        if (!m_shadowMapCompat) {
            if (m_shadowMapHandle.IsValid()) {
                m_device->DestroyRhiResource(m_shadowMapHandle);
                m_shadowMapHandle = {};
            }
            hr = m_device->CreateCommittedResource(&heap,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &smDesc,
                                                       // RHI 経由生成パス(RhiResourceState::ShaderResource)は常に
                                                       // PIXEL_SHADER_RESOURCE|NON_PIXEL_SHADER_RESOURCE の結合状態を
                                                       // 生成時状態とするため、フォールバックパスもこれに合わせて
                                                       // 初期状態を統一する(LightSystemShadow.cpp 側の最初の
                                                       // バリアの StateBefore 前提と一致させるため)。
                                                       static_cast<D3D12_RESOURCE_STATES>(
                                                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                                       &clear,
                                                       m_shadowMap);
            if (FAILED(hr)) {
                DebugLogDialog("ShadowMapManager::EnsureShadowResources: Shadow map resource creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
                return false;
            }
        }

        Resource* shadowMap = GetShadowMapResource();
        if (!shadowMap || !shadowMap->IsValid()) {
            DebugLogDialog("ShadowMapManager::EnsureShadowResources: Shadow map compatibility resource lookup failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            m_shadowMap.Reset();
            if (m_shadowMapHandle.IsValid()) {
                m_device->DestroyRhiResource(m_shadowMapHandle);
            }
            m_shadowMapHandle = {};
            m_shadowMapCompat = nullptr;
            return false;
        }

        // --- DSV（Depth Stencil View）ヒープを作成 ---
        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = MakeDsvHeapDesc(kDirectionalCascadeCount); // DSV はシェーダーから直接参照しない
        hr = m_device->CreateDescriptorHeap(dsvDesc, m_dsvHeapShadow);
        if (FAILED(hr)) {
            m_shadowMap.Reset();
            if (m_shadowMapHandle.IsValid()) {
                m_device->DestroyRhiResource(m_shadowMapHandle);
            }
            m_shadowMapHandle = {};
            m_shadowMapCompat = nullptr;
            DebugLogDialog("ShadowMapManager::EnsureShadowResources: Shadow DSV heap creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        // DSV（深度書き込み用ビュー）を作成
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Flags = D3D12_DSV_FLAG_NONE;
        dsv.Texture2DArray.ArraySize = 1;
        const UINT dsvInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        auto dsvHandle = m_dsvHeapShadow->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t cascadeIndex = 0; cascadeIndex < kDirectionalCascadeCount; ++cascadeIndex) {
            dsv.Texture2DArray.FirstArraySlice = cascadeIndex;
            m_device->CreateDepthStencilView(*shadowMap, &dsv, dsvHandle);
            dsvHandle.ptr += dsvInc;
        }

        // SRV（ライティングシェーダー読み取り用ビュー）を作成・登録
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT; // R チャンネルで深度値を読む
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.ArraySize = kDirectionalCascadeCount;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        m_device->CreateShaderResourceView(*shadowMap, &srvDesc, m_shadowSrvCpu);

        return true;
    }

    CpuDescriptorHandle ShadowMapManager::GetDirectionalCascadeDsv(uint32_t cascadeIndex) const
    {
        CpuDescriptorHandle handle = m_dsvHeapShadow->GetCPUDescriptorHandleForHeapStart();
        if (!m_device) {
            return handle;
        }
        const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        handle.ptr += static_cast<SIZE_T>(inc) * static_cast<SIZE_T>(cascadeIndex);
        return handle;
    }

    // =========================================================================
    // EnsureSpotShadowResources
    // スポットライトシャドウマップ（Texture2DArray, kMaxSpotShadows スライス）を
    // 初回使用時に遅延生成する。ライト i のシャドウはスライス i に描画される。
    // =========================================================================
    bool ShadowMapManager::EnsureSpotShadowResources()
    {
        if (!m_device) {
            return false;
        }
        if (m_spotShadowResourcesReady) {
            return true;
        }

        ScopedPerfTimer perfTimer("ShadowMapManager::EnsureSpotShadowResources");

        // R16_TYPELESS: DSV は D16_UNORM、SRV は R16_UNORM として使う
        D3D12_RESOURCE_DESC smDesc = {};
        smDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        smDesc.Width  = m_spotShadowMapSize;
        smDesc.Height = m_spotShadowMapSize;
        smDesc.DepthOrArraySize = static_cast<UINT16>(kMaxSpotShadows);
        smDesc.MipLevels = 1;
        smDesc.Format = DXGI_FORMAT_R16_TYPELESS;
        smDesc.SampleDesc.Count = 1;
        smDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        smDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_D16_UNORM;
        clear.DepthStencil.Depth = 1.0f;

        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = S_OK;
        m_spotShadowMapHandle = {};
        m_spotShadowMapCompat = nullptr;
        if (m_device->GetCapabilities().supportsRhiResourceCreation) {
            RhiTextureDesc rhiDesc{};
            rhiDesc.dimension = RhiResourceDimension::Texture2D;
            rhiDesc.extent = { m_spotShadowMapSize, m_spotShadowMapSize, 1u };
            rhiDesc.mipLevels = 1u;
            rhiDesc.arrayLayers = kMaxSpotShadows;
            rhiDesc.format = RhiFormat::R16Typeless;
            rhiDesc.usage = RhiTextureUsageFlags::ShaderResource | RhiTextureUsageFlags::DepthStencil;
            rhiDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
            rhiDesc.initialState = RhiResourceState::ShaderResource;
            m_spotShadowMapHandle = m_device->CreateRhiTexture(rhiDesc);
            m_spotShadowMapCompat = m_device->GetD3D12CompatibilityResource(m_spotShadowMapHandle);
        }
        if (!m_spotShadowMapCompat) {
            if (m_spotShadowMapHandle.IsValid()) {
                m_device->DestroyRhiResource(m_spotShadowMapHandle);
                m_spotShadowMapHandle = {};
            }
            hr = m_device->CreateCommittedResource(&heap,
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &smDesc,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                   &clear,
                                                   m_spotShadowMap);
            if (FAILED(hr)) {
                DebugLogDialog("ShadowMapManager::EnsureSpotShadowResources: texture creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
                return false;
            }
        }

        Resource* spotShadowMap = GetSpotShadowMapResource();
        if (!spotShadowMap || !spotShadowMap->IsValid()) {
            DebugLogDialog("ShadowMapManager::EnsureSpotShadowResources: texture compatibility resource lookup failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            m_spotShadowMap.Reset();
            if (m_spotShadowMapHandle.IsValid()) {
                m_device->DestroyRhiResource(m_spotShadowMapHandle);
            }
            m_spotShadowMapHandle = {};
            m_spotShadowMapCompat = nullptr;
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = MakeDsvHeapDesc(kMaxSpotShadows);
        hr = m_device->CreateDescriptorHeap(dsvDesc, m_spotDsvHeap);
        if (FAILED(hr)) {
            m_spotShadowMap.Reset();
            if (m_spotShadowMapHandle.IsValid()) {
                m_device->DestroyRhiResource(m_spotShadowMapHandle);
            }
            m_spotShadowMapHandle = {};
            m_spotShadowMapCompat = nullptr;
            DebugLogDialog("ShadowMapManager::EnsureSpotShadowResources: DSV heap creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        // スライスごとに DSV を作成（ライト i のシャドウはスライス i に対応）
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D16_UNORM;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Flags = D3D12_DSV_FLAG_NONE;
        dsv.Texture2DArray.ArraySize = 1;
        const UINT spotDsvInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        auto spotDsvHandle = m_spotDsvHeap->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t shadowIndex = 0; shadowIndex < kMaxSpotShadows; ++shadowIndex) {
            dsv.Texture2DArray.FirstArraySlice = shadowIndex;
            m_device->CreateDepthStencilView(*spotShadowMap, &dsv, spotDsvHandle);
            spotDsvHandle.ptr += spotDsvInc;
        }

        // SRV はすべてのスライスをまとめて 1 つの Texture2DArray として公開する
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R16_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.ArraySize = kMaxSpotShadows;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        m_device->CreateShaderResourceView(*spotShadowMap, &srvDesc, m_spotShadowSrvCpu);

        m_spotShadowResourcesReady = true;
        return true;
    }

    CpuDescriptorHandle ShadowMapManager::GetSpotDsv(uint32_t shadowIndex) const
    {
        if (shadowIndex >= kMaxSpotShadows) {
            return CpuDescriptorHandle{};
        }
        CpuDescriptorHandle handle = m_spotDsvHeap->GetCPUDescriptorHandleForHeapStart();
        if (!m_device) {
            return handle;
        }
        const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        handle.ptr += static_cast<SIZE_T>(inc) * static_cast<SIZE_T>(shadowIndex);
        return handle;
    }

    // =========================================================================
    // EnsurePointShadowResources
    // ポイントライトのキューブシャドウマップ（Texture2DArray、
    // kMaxPointShadows * kPointShadowFaceCount スライス）を初回使用時に遅延生成する。
    // ライト単位でスライスをまとめ、ライト i は面インデックス i*6 〜 i*6+5 を占有する。
    // =========================================================================
    bool ShadowMapManager::EnsurePointShadowResources()
    {
        if (!m_device) {
            return false;
        }
        if (m_pointShadowResourcesReady) {
            return true;
        }

        ScopedPerfTimer perfTimer("ShadowMapManager::EnsurePointShadowResources");

        // R16_TYPELESS: DSV は D16_UNORM、SRV は R16_UNORM として使う
        D3D12_RESOURCE_DESC smDesc = {};
        smDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        smDesc.Width  = m_pointShadowMapSize;
        smDesc.Height = m_pointShadowMapSize;
        smDesc.DepthOrArraySize = static_cast<UINT16>(kMaxPointShadows * kPointShadowFaceCount);
        smDesc.MipLevels = 1;
        smDesc.Format = DXGI_FORMAT_R16_TYPELESS;
        smDesc.SampleDesc.Count = 1;
        smDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        smDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_D16_UNORM;
        clear.DepthStencil.Depth = 1.0f;

        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = S_OK;
        m_pointShadowMapHandle = {};
        m_pointShadowMapCompat = nullptr;
        if (m_device->GetCapabilities().supportsRhiResourceCreation) {
            RhiTextureDesc rhiDesc{};
            rhiDesc.dimension = RhiResourceDimension::Texture2D;
            rhiDesc.extent = { m_pointShadowMapSize, m_pointShadowMapSize, 1u };
            rhiDesc.mipLevels = 1u;
            rhiDesc.arrayLayers = kMaxPointShadows * kPointShadowFaceCount;
            rhiDesc.format = RhiFormat::R16Typeless;
            rhiDesc.usage = RhiTextureUsageFlags::ShaderResource | RhiTextureUsageFlags::DepthStencil;
            rhiDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
            rhiDesc.initialState = RhiResourceState::ShaderResource;
            m_pointShadowMapHandle = m_device->CreateRhiTexture(rhiDesc);
            m_pointShadowMapCompat = m_device->GetD3D12CompatibilityResource(m_pointShadowMapHandle);
        }
        if (!m_pointShadowMapCompat) {
            if (m_pointShadowMapHandle.IsValid()) {
                m_device->DestroyRhiResource(m_pointShadowMapHandle);
                m_pointShadowMapHandle = {};
            }
            hr = m_device->CreateCommittedResource(&heap,
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &smDesc,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                   &clear,
                                                   m_pointShadowMap);
            if (FAILED(hr)) {
                DebugLogDialog("ShadowMapManager::EnsurePointShadowResources: texture creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
                return false;
            }
        }

        Resource* pointShadowMap = GetPointShadowMapResource();
        if (!pointShadowMap || !pointShadowMap->IsValid()) {
            DebugLogDialog("ShadowMapManager::EnsurePointShadowResources: texture compatibility resource lookup failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            m_pointShadowMap.Reset();
            if (m_pointShadowMapHandle.IsValid()) {
                m_device->DestroyRhiResource(m_pointShadowMapHandle);
            }
            m_pointShadowMapHandle = {};
            m_pointShadowMapCompat = nullptr;
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = MakeDsvHeapDesc(kMaxPointShadows * kPointShadowFaceCount);
        hr = m_device->CreateDescriptorHeap(dsvDesc, m_pointDsvHeap);
        if (FAILED(hr)) {
            m_pointShadowMap.Reset();
            if (m_pointShadowMapHandle.IsValid()) {
                m_device->DestroyRhiResource(m_pointShadowMapHandle);
            }
            m_pointShadowMapHandle = {};
            m_pointShadowMapCompat = nullptr;
            DebugLogDialog("ShadowMapManager::EnsurePointShadowResources: DSV heap creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        // スライスごとに DSV を作成（ライト i の面 face はスライス i*6+face に対応）
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D16_UNORM;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Flags = D3D12_DSV_FLAG_NONE;
        dsv.Texture2DArray.ArraySize = 1;
        dsv.Texture2DArray.MipSlice = 0;
        const UINT dsvInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        auto dsvHandle = m_pointDsvHeap->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t slice = 0; slice < kMaxPointShadows * kPointShadowFaceCount; ++slice) {
            dsv.Texture2DArray.FirstArraySlice = slice;
            m_device->CreateDepthStencilView(*pointShadowMap, &dsv, dsvHandle);
            dsvHandle.ptr += dsvInc;
        }

        // SRV はすべてのライト・面のスライスをまとめて 1 つの Texture2DArray として公開する
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R16_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = kMaxPointShadows * kPointShadowFaceCount;
        srvDesc.Texture2DArray.PlaneSlice = 0;
        srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
        m_device->CreateShaderResourceView(*pointShadowMap, &srvDesc, m_pointShadowSrvCpu);

        m_pointShadowResourcesReady = true;
        return true;
    }

    CpuDescriptorHandle ShadowMapManager::GetPointFaceDsv(uint32_t shadowIndex, uint32_t face) const
    {
        if (shadowIndex >= kMaxPointShadows || face >= kPointShadowFaceCount) {
            return CpuDescriptorHandle{};
        }
        CpuDescriptorHandle handle = m_pointDsvHeap->GetCPUDescriptorHandleForHeapStart();
        if (!m_device) {
            return handle;
        }
        const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        const uint32_t slice = shadowIndex * kPointShadowFaceCount + face;
        handle.ptr += static_cast<SIZE_T>(inc) * static_cast<SIZE_T>(slice);
        return handle;
    }

    // =========================================================================
    // EnsureVsmResources
    // VSM シャドウマップテクスチャ・RTV・ブラー用ディスクリプタヒープを初回生成する。
    // =========================================================================
    bool ShadowMapManager::EnsureVsmResources()
    {
        if (!m_device) return false;
        if (m_vsmResourcesReady) return true;

        ScopedPerfTimer perfTimer("ShadowMapManager::EnsureVsmResources");
        ID3D12Device* dev = m_device->GetDevice();
        if (!dev) return false;

        // --- vsmMap: R32G32_FLOAT, 4 スライス, RTV + UAV 可 ---
        D3D12_RESOURCE_DESC vsmDesc = {};
        vsmDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        vsmDesc.Width            = m_shadowMapSize;
        vsmDesc.Height           = m_shadowMapSize;
        vsmDesc.DepthOrArraySize = static_cast<UINT16>(kDirectionalCascadeCount);
        vsmDesc.MipLevels        = 1;
        vsmDesc.Format           = DXGI_FORMAT_R32G32_FLOAT;
        vsmDesc.SampleDesc.Count = 1;
        vsmDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        vsmDesc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12_CLEAR_VALUE vsmClear = {};
        vsmClear.Format     = DXGI_FORMAT_R32G32_FLOAT;
        vsmClear.Color[0]   = 1.0f;
        vsmClear.Color[1]   = 1.0f;
        vsmClear.Color[2]   = 1.0f;
        vsmClear.Color[3]   = 1.0f;

        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = S_OK;
        m_vsmMapHandle = {};
        m_vsmMapCompat = nullptr;
        if (m_device->GetCapabilities().supportsRhiResourceCreation) {
            RhiTextureDesc rhiDesc{};
            rhiDesc.dimension = RhiResourceDimension::Texture2D;
            rhiDesc.extent = { m_shadowMapSize, m_shadowMapSize, 1u };
            rhiDesc.mipLevels = 1u;
            rhiDesc.arrayLayers = kDirectionalCascadeCount;
            rhiDesc.format = RhiFormat::R32G32Float;
            rhiDesc.usage = RhiTextureUsageFlags::ShaderResource |
                            RhiTextureUsageFlags::RenderTarget |
                            RhiTextureUsageFlags::UnorderedAccess;
            rhiDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
            rhiDesc.initialState = RhiResourceState::ShaderResource;
            m_vsmMapHandle = m_device->CreateRhiTexture(rhiDesc);
            m_vsmMapCompat = m_device->GetD3D12CompatibilityResource(m_vsmMapHandle);
        }
        if (!m_vsmMapCompat) {
            if (m_vsmMapHandle.IsValid()) {
                m_device->DestroyRhiResource(m_vsmMapHandle);
                m_vsmMapHandle = {};
            }
            hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &vsmDesc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &vsmClear, m_vsmMap);
            if (FAILED(hr)) {
                DebugLogDialog("ShadowMapManager::EnsureVsmResources: vsmMap creation failed.\n",
                               L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
                return false;
            }
        }

        // --- vsmMapTemp: 同フォーマット, UAV のみ (ブラーのピンポンバッファ) ---
        D3D12_RESOURCE_DESC vsmTempDesc = vsmDesc;
        vsmTempDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        m_vsmMapTempHandle = {};
        m_vsmMapTempCompat = nullptr;
        if (m_device->GetCapabilities().supportsRhiResourceCreation) {
            RhiTextureDesc rhiTempDesc{};
            rhiTempDesc.dimension = RhiResourceDimension::Texture2D;
            rhiTempDesc.extent = { m_shadowMapSize, m_shadowMapSize, 1u };
            rhiTempDesc.mipLevels = 1u;
            rhiTempDesc.arrayLayers = kDirectionalCascadeCount;
            rhiTempDesc.format = RhiFormat::R32G32Float;
            rhiTempDesc.usage = RhiTextureUsageFlags::ShaderResource |
                                RhiTextureUsageFlags::UnorderedAccess;
            rhiTempDesc.memoryUsage = RhiMemoryUsage::GpuOnly;
            rhiTempDesc.initialState = RhiResourceState::UnorderedAccess;
            m_vsmMapTempHandle = m_device->CreateRhiTexture(rhiTempDesc);
            m_vsmMapTempCompat = m_device->GetD3D12CompatibilityResource(m_vsmMapTempHandle);
        }
        if (!m_vsmMapTempCompat) {
            if (m_vsmMapTempHandle.IsValid()) {
                m_device->DestroyRhiResource(m_vsmMapTempHandle);
                m_vsmMapTempHandle = {};
            }
            hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &vsmTempDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, m_vsmMapTemp);
            if (FAILED(hr)) {
                DebugLogDialog("ShadowMapManager::EnsureVsmResources: vsmMapTemp creation failed.\n",
                               L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
                m_vsmMap.Reset();
                m_vsmMapTemp.Reset();
                if (m_vsmMapHandle.IsValid()) {
                    m_device->DestroyRhiResource(m_vsmMapHandle);
                }
                if (m_vsmMapTempHandle.IsValid()) {
                    m_device->DestroyRhiResource(m_vsmMapTempHandle);
                }
                m_vsmMapHandle = {}; m_vsmMapTempHandle = {};
                m_vsmMapCompat = nullptr; m_vsmMapTempCompat = nullptr;
                return false;
            }
        }

        // --- RTV ヒープ (kDirectionalCascadeCount 個) ---
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = kDirectionalCascadeCount;
        rtvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = m_device->CreateDescriptorHeap(rtvHeapDesc, m_vsmRtvHeap);
        if (FAILED(hr)) {
            DebugLogDialog("ShadowMapManager::EnsureVsmResources: RTV heap creation failed.\n",
                           L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            m_vsmMap.Reset(); m_vsmMapTemp.Reset();
            if (m_vsmMapHandle.IsValid()) {
                m_device->DestroyRhiResource(m_vsmMapHandle);
            }
            if (m_vsmMapTempHandle.IsValid()) {
                m_device->DestroyRhiResource(m_vsmMapTempHandle);
            }
            m_vsmMapHandle = {}; m_vsmMapTempHandle = {};
            m_vsmMapCompat = nullptr; m_vsmMapTempCompat = nullptr;
            return false;
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format                     = DXGI_FORMAT_R32G32_FLOAT;
        rtvDesc.ViewDimension              = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.MipSlice    = 0;
        rtvDesc.Texture2DArray.ArraySize   = 1;
        const UINT rtvInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        CpuDescriptorHandle rtvHandle = m_vsmRtvHeap->GetCPUDescriptorHandleForHeapStart();
        Resource* vsmMap = GetVsmMapResource();
        Resource* vsmMapTemp = GetVsmMapTempResource();
        if (!vsmMap || !vsmMap->IsValid() || !vsmMapTemp || !vsmMapTemp->IsValid()) {
            DebugLogDialog("ShadowMapManager::EnsureVsmResources: VSM compatibility resource lookup failed.\n",
                           L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            m_vsmMap.Reset(); m_vsmMapTemp.Reset(); m_vsmRtvHeap = {};
            if (m_vsmMapHandle.IsValid()) {
                m_device->DestroyRhiResource(m_vsmMapHandle);
            }
            if (m_vsmMapTempHandle.IsValid()) {
                m_device->DestroyRhiResource(m_vsmMapTempHandle);
            }
            m_vsmMapHandle = {}; m_vsmMapTempHandle = {};
            m_vsmMapCompat = nullptr; m_vsmMapTempCompat = nullptr;
            return false;
        }
        for (uint32_t ci = 0; ci < kDirectionalCascadeCount; ++ci) {
            rtvDesc.Texture2DArray.FirstArraySlice = ci;
            m_device->CreateRenderTargetView(*vsmMap, &rtvDesc, rtvHandle);
            rtvHandle.ptr += rtvInc;
        }

        // --- ブラー用シェーダー可視ディスクリプタヒープ (4 スロット) ---
        // [0] SRV vsmMap    (H-pass 入力)
        // [1] UAV vsmMapTemp (H-pass 出力)
        // [2] SRV vsmMapTemp (V-pass 入力)
        // [3] UAV vsmMap    (V-pass 出力)
        D3D12_DESCRIPTOR_HEAP_DESC blurHeapDesc = {};
        blurHeapDesc.NumDescriptors = 4;
        blurHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        blurHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = m_device->CreateDescriptorHeap(blurHeapDesc, m_vsmBlurDescHeap);
        if (FAILED(hr)) {
            DebugLogDialog("ShadowMapManager::EnsureVsmResources: blur descriptor heap creation failed.\n",
                           L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            m_vsmMap.Reset(); m_vsmMapTemp.Reset(); m_vsmRtvHeap = {};
            if (m_vsmMapHandle.IsValid()) {
                m_device->DestroyRhiResource(m_vsmMapHandle);
            }
            if (m_vsmMapTempHandle.IsValid()) {
                m_device->DestroyRhiResource(m_vsmMapTempHandle);
            }
            m_vsmMapHandle = {}; m_vsmMapTempHandle = {};
            m_vsmMapCompat = nullptr; m_vsmMapTempCompat = nullptr;
            return false;
        }

        const UINT srvUavInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        CpuDescriptorHandle blurCpu = m_vsmBlurDescHeap->GetCPUDescriptorHandleForHeapStart();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format                          = DXGI_FORMAT_R32G32_FLOAT;
        srvDesc.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MipLevels        = 1;
        srvDesc.Texture2DArray.ArraySize        = kDirectionalCascadeCount;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format                            = DXGI_FORMAT_R32G32_FLOAT;
        uavDesc.ViewDimension                     = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        uavDesc.Texture2DArray.ArraySize          = kDirectionalCascadeCount;

        // slot 0: SRV of vsmMap
        m_device->CreateShaderResourceView(*vsmMap, &srvDesc, blurCpu);
        blurCpu.ptr += srvUavInc;

        // slot 1: UAV of vsmMapTemp
        dev->CreateUnorderedAccessView(vsmMapTemp->Get(), nullptr, &uavDesc, blurCpu);
        blurCpu.ptr += srvUavInc;

        // slot 2: SRV of vsmMapTemp
        m_device->CreateShaderResourceView(*vsmMapTemp, &srvDesc, blurCpu);
        blurCpu.ptr += srvUavInc;

        // slot 3: UAV of vsmMap
        dev->CreateUnorderedAccessView(vsmMap->Get(), nullptr, &uavDesc, blurCpu);

        // --- メイン SRV (t13 ライティングパス用) ---
        m_device->CreateShaderResourceView(*vsmMap, &srvDesc, m_vsmSrvCpu);

        m_vsmResourcesReady = true;
        return true;
    }

    CpuDescriptorHandle ShadowMapManager::GetVsmRtv(uint32_t cascadeIndex) const
    {
        CpuDescriptorHandle handle = m_vsmRtvHeap->GetCPUDescriptorHandleForHeapStart();
        if (!m_device) return handle;
        const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        handle.ptr += static_cast<SIZE_T>(inc) * static_cast<SIZE_T>(cascadeIndex);
        return handle;
    }
}
