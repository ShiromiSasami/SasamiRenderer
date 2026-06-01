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

        // Null SRV で仮登録
        D3D12_SHADER_RESOURCE_VIEW_DESC spotNullSrvDesc = {};
        spotNullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        spotNullSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        spotNullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        spotNullSrvDesc.Texture2D.MipLevels = 1;
        Resource spotNullResource;
        m_device->CreateShaderResourceView(spotNullResource, &spotNullSrvDesc, spotShadowCpu);

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
        if (m_shadowMap.IsValid() && m_dsvHeapShadow.Get()) {
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

        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = m_device->CreateCommittedResource(&heap,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &smDesc,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, // 初期状態は SRV として読める状態
                                                       &clear,
                                                       m_shadowMap);
        if (FAILED(hr)) {
            DebugLogDialog("ShadowMapManager::EnsureShadowResources: Shadow map resource creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        // --- DSV（Depth Stencil View）ヒープを作成 ---
        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
        dsvDesc.NumDescriptors = kDirectionalCascadeCount;
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // DSV はシェーダーから直接参照しない
        hr = m_device->CreateDescriptorHeap(dsvDesc, m_dsvHeapShadow);
        if (FAILED(hr)) {
            m_shadowMap.Reset();
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
            m_device->CreateDepthStencilView(m_shadowMap, &dsv, dsvHandle);
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
        m_device->CreateShaderResourceView(m_shadowMap, &srvDesc, m_shadowSrvCpu);

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
    // スポットライトシャドウマップを初回使用時に遅延生成する。
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
        smDesc.DepthOrArraySize = 1;
        smDesc.MipLevels = 1;
        smDesc.Format = DXGI_FORMAT_R16_TYPELESS;
        smDesc.SampleDesc.Count = 1;
        smDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        smDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_D16_UNORM;
        clear.DepthStencil.Depth = 1.0f;

        CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = m_device->CreateCommittedResource(&heap,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &smDesc,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                       &clear,
                                                       m_spotShadowMap);
        if (FAILED(hr)) {
            DebugLogDialog("ShadowMapManager::EnsureSpotShadowResources: texture creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
        dsvDesc.NumDescriptors = 1;
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = m_device->CreateDescriptorHeap(dsvDesc, m_spotDsvHeap);
        if (FAILED(hr)) {
            m_spotShadowMap.Reset();
            DebugLogDialog("ShadowMapManager::EnsureSpotShadowResources: DSV heap creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D16_UNORM;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsv.Flags = D3D12_DSV_FLAG_NONE;
        m_device->CreateDepthStencilView(m_spotShadowMap, &dsv, m_spotDsvHeap->GetCPUDescriptorHandleForHeapStart());

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R16_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_spotShadowMap, &srvDesc, m_spotShadowSrvCpu);

        m_spotShadowResourcesReady = true;
        return true;
    }

    CpuDescriptorHandle ShadowMapManager::GetSpotDsv() const
    {
        return m_spotDsvHeap->GetCPUDescriptorHandleForHeapStart();
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
        HRESULT hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &vsmDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &vsmClear, m_vsmMap);
        if (FAILED(hr)) {
            DebugLogDialog("ShadowMapManager::EnsureVsmResources: vsmMap creation failed.\n",
                           L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        // --- vsmMapTemp: 同フォーマット, UAV のみ (ブラーのピンポンバッファ) ---
        D3D12_RESOURCE_DESC vsmTempDesc = vsmDesc;
        vsmTempDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &vsmTempDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, m_vsmMapTemp);
        if (FAILED(hr)) {
            DebugLogDialog("ShadowMapManager::EnsureVsmResources: vsmMapTemp creation failed.\n",
                           L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            m_vsmMap.Reset();
            return false;
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
            return false;
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format                     = DXGI_FORMAT_R32G32_FLOAT;
        rtvDesc.ViewDimension              = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.MipSlice    = 0;
        rtvDesc.Texture2DArray.ArraySize   = 1;
        const UINT rtvInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        CpuDescriptorHandle rtvHandle = m_vsmRtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t ci = 0; ci < kDirectionalCascadeCount; ++ci) {
            rtvDesc.Texture2DArray.FirstArraySlice = ci;
            m_device->CreateRenderTargetView(m_vsmMap, &rtvDesc, rtvHandle);
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
        m_device->CreateShaderResourceView(m_vsmMap, &srvDesc, blurCpu);
        blurCpu.ptr += srvUavInc;

        // slot 1: UAV of vsmMapTemp
        dev->CreateUnorderedAccessView(m_vsmMapTemp.Get(), nullptr, &uavDesc, blurCpu);
        blurCpu.ptr += srvUavInc;

        // slot 2: SRV of vsmMapTemp
        m_device->CreateShaderResourceView(m_vsmMapTemp, &srvDesc, blurCpu);
        blurCpu.ptr += srvUavInc;

        // slot 3: UAV of vsmMap
        dev->CreateUnorderedAccessView(m_vsmMap.Get(), nullptr, &uavDesc, blurCpu);

        // --- メイン SRV (t13 ライティングパス用) ---
        m_device->CreateShaderResourceView(m_vsmMap, &srvDesc, m_vsmSrvCpu);

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
