#pragma once

// =============================================================================
// DeferredLightingRenderPass.h
// ラスタライズパイプラインのライティングパスを表すレンダーグラフノード。
//
// ⚠️ このコメントは書き下ろしである。元は日本語のコメントが入っていたが文字エンコーディングの
//    事故で壊れており（git HEAD の時点で既に破損）、逆変換でも一部の文字が復元できなかった。
//    そのため「復元」ではなく、2026-09-25 時点のコードを読んで実際の動作を記述し直したものである。
//    復元できた断片には既に存在しない PBR_PS.hlsl への参照も含まれており、内容自体も古かった。
//
// 【役割】
//  - GBuffer（アルベド / 法線 / マテリアル / エミッシブ / スペキュラワークフロー）と深度・
//    各種シャドウマップを読み、PBR ライティングを計算して SceneColor に書く
//  - ディレクショナル / ポイント / スポットの直接光、IBL、遮蔽、反射を合成する
//
// 【グラフ上の依存】（Setup の宣言そのまま）
//  Read:  ShadowMap / SceneDepth / GBufferAlbedo / GBufferNormal / GBufferMaterial /
//         GBufferEmissive / GBufferSpecularWorkflow / SSAOBlur / SSRReflection /
//         SoftwareReflection
//  Write: SceneColor
//  注: スポット/ポイント/VSM のシャドウマップは LightSystem/ShadowMapManager が所有していて
//      レンダーグラフに import されていないため、ここでは宣言できず暗黙の依存になっている
//      （既知の課題として追跡中）。
//
// 【ディスクリプタテーブル】ルートパラメータ番号は Execute の SetGraphicsDescriptorTable に
//  直接書かれている（4=ディレクショナル影 / 6=ライトテーブル / 7=IBL / 8=スクリーンスペースAO /
//  9=反射 / 10=スポット影 / 11=VSM / 17=ポイント影 / 18=IBLスペキュラ）。
//  HLSL 側のレジスタ番号とは別物なので、シェーダのバインディングは
//  Shaders/Raster/Lighting/Deferred/DeferredLighting_PS.hlsl の宣言を直接参照すること。
// =============================================================================

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Passes/Core/RenderPassSetupContext.h"

#include <functional>

namespace SasamiEngine::Render
{
    // =========================================================================
    // DeferredLightingRenderPass
    // =========================================================================
    class DeferredLightingRenderPass : public IRenderPass
    {
    public:
        // Render graph identification.
        std::string_view Tag() const override { return "Lighting"; }
        std::string_view PhaseTag() const override { return "Scene"; }

        // Declares resources and services used by this pass.
        void BuildRequirements(RenderPassRequirementBuilder& builder) const override;

        // Declares render graph resource dependencies.
        void Setup(RenderGraphBuilder& builder) const override;

        // Executes through the render graph context.
        bool Execute(const RenderPassContextView& context) const override;

        // Fullscreen deferred lighting combine. GBufferRenderPass owns
        // the mesh GBuffer/depth draw; this pass reads those resources and
        // writes SceneColor.
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
