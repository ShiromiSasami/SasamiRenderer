#pragma once

// =============================================================================
// LightCBData.h
// GPU ライティング定数バッファのメモリレイアウト定義。
//
// シェーダー側の LightCB.hlsli（cbuffer LightCB : register(b1)）と
// バイト単位で完全一致する必要がある。
//
// LightSystem::UpdateFrameLighting() が毎フレームこの構造体を
// アップロードバッファに書き込み、GPU へ転送する。
// =============================================================================

#include <cstddef>

namespace SasamiRenderer
{
    namespace LightCBLayout
    {
        // 拡散照明用球面調和（Spherical Harmonics）係数の本数。
        // L0 + L1 の 1+3=4 本ではなく L2 まで含む 9 本（SH2 次）を使用。
        constexpr size_t kDirectionalCascadeCount = 4u;
        constexpr size_t kDiffuseShCoefficientCount = 9u;

        // =============================================================================
        // LightCBData
        // シェーダーの cbuffer LightCB（b1 レジスタ）に対応する CPU 側構造体。
        //
        // 【注意】D3D12 の定数バッファは 256 バイトアライン必須。
        //         LightSystem 側でアライン済みのアップロードバッファを確保している。
        // =============================================================================
        struct LightCBData
        {
            // シャドウマップ生成時に使用するライト視点のビュープロジェクション行列（列優先 4x4）。
            // ピクセルシェーダーでワールド座標をライトクリップ空間に変換し、
            // シャドウマップと深度比較するために使う。
            float lightVP[kDirectionalCascadeCount][16];

            // ディレクショナルライトの方向と強度。
            // xyz: ライト方向ベクトル（ワールド空間、正規化済み・ライトへ向かう方向）
            // w:   強度スカラー（ライトカラーに乗算される）
            float dirDir[4];

            // ディレクショナルライトの色。
            // rgb: 線形 RGB カラー
            // w:   未使用
            float dirColor[4];

            // ライト数カウント。
            // x: ポイントライトの数（最大 16）
            // y: スポットライトの数（最大 16）
            // zw: 未使用
            float lightCounts[4];

            // カメラのワールド空間座標。
            // xyz: ワールド座標
            // w:   未使用
            // シェーダーで視線ベクトルや反射方向の計算に使う。
            float cameraPos[4];
            float cameraPV[16];

            // Image-Based Lighting（IBL）パラメータ。
            // x: IBL 有効フラグ（1.0 = 有効）
            // y: IBL 全体強度スカラー
            // z: プリフィルタキューブマップの最大 Mip レベル数（粗さマッピングに使用）
            // w: 拡散 IBL に SH を使うか（1.0 = SH 使用、0.0 = キューブマップ直接サンプリング）
            float iblParams[4];

            // デバッグ表示モード。
            // x: 0=通常ライティング, 1=アルベド, 2=法線, 3=粗さ, 4=メタリック, 5=AO, 6=シャドウ
            // yzw: 未使用
            float debugParams[4];

            // シャドウマップのテクセルサイズ（PCF 用）。
            // x: 1 / シャドウマップ幅
            // y: 1 / シャドウマップ高さ
            // PCF（Percentage Closer Filtering）でサンプルオフセットを計算するために使う。
            float shadowParams[4];
            float shadowCascadeSplits[4];
            float shadowCascadeTexelSize[kDirectionalCascadeCount][4];
            float shadowCascadeParams[4];
            float contactShadowParams[4];

            // リフレクションテクスチャのパラメータ。
            // x: リフレクション有効フラグ（1.0 = 有効）
            // y: 反射強度スカラー
            // z: 描画幅（リフレクションテクスチャの UV 計算用）
            // w: 描画高さ
            float reflectionParams[4];

            // 拡散照明用の球面調和係数（9 本）。
            // 各係数は float4 で格納され xyz を使用（w は未使用）。
            // EvaluateDiffuseIrradianceFromSh() でスカイライトの拡散照明を効率的に評価する。
            float diffuseSh[kDiffuseShCoefficientCount][4];

            // スポットライト #0 のシャドウマップ用ビュープロジェクション行列（行優先 4x4）。
            // ピクセルシェーダーでワールド座標をスポットライトクリップ空間に変換する。
            float spotLightVP[16];

            // スポットシャドウのパラメータ。
            // x: 深度バイアス（セルフシャドウ防止）
            // y: ニアプレーン（参照用）
            // z: 有効フラグ（1.0 = シャドウ有効）
            // w: シャドウマップの一辺のサイズ（テクセルサイズ計算用）
            float spotShadowParams[4];
        };
    }
}
