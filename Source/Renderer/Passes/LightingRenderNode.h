#pragma once

// =============================================================================
// LightingRenderNode.h
// ラスタライズパイプラインのライティングパスを表すレンダーグラフノード。
//
// 【役割】
//  - GBuffer の法線・深度・シャドウマップを読み込む
//  - PBR ライティング（ディレクショナル・ポイント・スポット・IBL）を計算して SceneColor に書く
//  - ポストプロセス前の最終シェーディング結果を出力する
//
// 【リソース依存】
//  Read:  "ShadowMap"（シャドウマップ深度 SRV）
//  Read:  "SceneDepth"（GBuffer 深度）
//  Write: "SceneColor"（ライティング結果出力先）
//
// 【シェーダー】
//  - 標準パス: PBR_PS.hlsl / BasicShader.hlsl（PSO に依存）
//  - テッセレーションパス: 専用の PSO を使用
//
// 【ルートシグネチャのディスクリプタテーブル割り当て】
//  b3: LightCB（ライティング定数バッファ）
//  t1: ShadowMap SRV
//  t4: Point/SpotLight StructuredBuffer（ライトテーブル）
//  t5: IBL キューブマップ SRV テーブル
//  t6: Runtime AO テクスチャ SRV（SSAO/RTAO など）
//  t7: Reflection テクスチャ SRV
// =============================================================================

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Passes/IRenderNode.h"
#include "Renderer/Passes/RenderNodeSetupContext.h"

#include <functional>

namespace SasamiRenderer
{
    // =========================================================================
    // LightingRenderNode
    // =========================================================================
    class LightingRenderNode : public IRenderNode
    {
    public:
        // レンダーグラフ上でこのノードを識別するタグ
        std::string_view Tag() const override { return "Lighting"; }
        // フェーズタグ：同フェーズ内のノード順序制御に使う
        std::string_view PhaseTag() const override { return "Scene"; }

        // レンダーグラフ構築時に呼ばれる。グラフィクスベース要件を宣言する。
        void BuildRequirements(RenderNodeRequirementBuilder& builder) const override;

        // リソース依存を宣言する。
        // ShadowMap / SceneDepth を読み、SceneColor に書き込む。
        void Setup(RenderGraphBuilder& builder) const override;

        // レンダーグラフの Execute フェーズで呼ばれる。
        // コンテキストから必要な入力を取り出して下の Execute() オーバーロードを呼ぶ。
        bool Execute(const RenderNodeContextView& context) const override;

        // ライティングパスの実際の GPU コマンドを発行する。
        // 外部から直接呼ぶことも可能（テスト・デバッグ用途等）。
        //
        // @param cmdList          コマンドリスト
        // @param pipelineStateCache PSO を取得するキャッシュ
        // @param srvHeap          GPU 可視ディスクリプタヒープ
        // @param viewport         描画ビューポート
        // @param scissorRect      シザー矩形
        // @param useTessellation          テッセレーションパスを使うか
        // @param useTessellationWireframe テッセレーション結果をワイヤーフレームで表示するか
        // @param useMeshletDebugView      メッシュレット単位でカラーリングするデバッグ表示を使うか
        // @param shadowSrv                シャドウマップ SRV GPU ハンドル（t1）
        // @param lightSrvTable    ポイント/スポットライト SRV テーブル先頭（t4）
        // @param iblSrvTable      IBL SRV テーブル先頭（t5）
        // @param aoSrv            Runtime AO テクスチャ SRV（t6）
        // @param reflectionSrv    リフレクションテクスチャ SRV（t7）
        // @param lightCbGpu       LightCB の GPU 仮想アドレス（b3）
        // @param drawCallback     メッシュドローコールを発行するコールバック
        void Execute(IRhiCommandEncoder* enc,
                     RenderPipelineStateCache& pipelineStateCache,
                     DescriptorHeap& srvHeap,
                     const Viewport& viewport,
                     const Rect& scissorRect,
                     bool useTessellation,
                     bool useTessellationWireframe,
                     bool useTessellationDebugColors,
                     bool useMeshletDebugView,
                     GpuDescriptorHandle shadowSrv,
                     GpuDescriptorHandle spotShadowSrv,
                     GpuDescriptorHandle vsmSrv,
                     GpuDescriptorHandle lightSrvTable,
                     GpuDescriptorHandle iblSrvTable,
                     GpuDescriptorHandle aoSrv,
                     GpuDescriptorHandle reflectionSrv,
                     GpuDescriptorHandle depthSrv,
                     GpuDescriptorHandle transparentBackfaceDistanceSrv,
                     D3D12_GPU_VIRTUAL_ADDRESS lightCbGpu,
                     const std::function<void()>& drawCallback) const;
    };
}
