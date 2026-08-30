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

        // How many spot/point lights can cast shadows at once. Lights beyond these
        // counts still light the scene, they just do not occlude. Raising them costs
        // shadow-map memory (spot: 1024^2 D16 each; point: 512^2 D16 x 6 faces each)
        // and constant-buffer space, and must be mirrored in Shaders/Shared/Common/LightCB.hlsli.
        constexpr size_t kMaxSpotShadows = 8u;
        constexpr size_t kMaxPointShadows = 4u;
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
            // w:   ディレクショナルシャドウの供給元モード（LightSystem::UpdateFrameLighting が設定）。
            //      0 = ラスタライズカスケード（既存の複数カスケード選択/ブレンド処理をそのまま使う）
            //      1 = SWRT（ソフトウェアレイトレース）の安定カスケード1枚をそのままサンプルする
            //          （LightSystem::BuildStableDirectionalShadowPassContext によりカスケード数は
            //          1 に設定されるため、既存のカスケード選択ループがそのまま cascadeIndex=0 を
            //          選ぶ。シェーダー側の分岐は不要）
            //      2 = SWRT が有効だが今フレームの結果が信頼できない（未レンダリングのフォールバック
            //          テクスチャが束縛されている）。PBR_Shadow.hlsli 側でサンプルせず 1.0 を返す
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
            float invCameraPV[16];

            // Image-Based Lighting（IBL）パラメータ。
            // x: IBL 有効フラグ（1.0 = 有効）
            // y: IBL 全体強度スカラー
            // z: プリフィルタキューブマップの最大 Mip レベル数（粗さマッピングに使用）
            // w: 拡散 IBL に SH を使うか（1.0 = SH 使用、0.0 = キューブマップ直接サンプリング）
            float iblParams[4];

            // デバッグ表示モード。
            // x: 0=通常ライティング, 1=アルベド, 2=法線, 3=粗さ, 4=メタリック, 5=AO, 6=シャドウ
            // y: 直接光マイクロシャドウイング強度 [0,1]（Chan 2018 式、AO から光方向依存の遮蔽を近似）
            // zw: 未使用
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

            // シャドウを持つスポットライトのビュープロジェクション行列（行優先 4x4）。
            // 添字は SpotLightGPU::params.y の shadowIndex（-1 は影なし）と対応する。
            float spotLightVP[kMaxSpotShadows][16];

            // スポットシャドウのパラメータ。
            // x: 深度バイアス（セルフシャドウ防止）
            // y: ニアプレーン（参照用）
            // z: 有効フラグ（1.0 = シャドウ有効）
            // w: シャドウマップの一辺のサイズ（テクセルサイズ計算用）
            float spotShadowParams[4];

            // シャドウを持つポイントライトのキューブ用ビュープロジェクション行列（行優先 4x4 × 6面）。
            // 第1添字は PointLightGPU::params.x の shadowIndex（-1 は影なし）。
            // 面インデックスの規約: 0:+X, 1:-X, 2:+Y, 3:-Y, 4:+Z, 5:-Z（Math::BuildCubeFaceViewProjection と一致）。
            float pointLightVP[kMaxPointShadows][6][16];

            // ポイントシャドウのパラメータ。
            // x: 深度バイアス（セルフシャドウ防止）
            // y: ニアプレーン（参照用）
            // z: 有効フラグ（1.0 = シャドウ有効）
            // w: シャドウマップの一辺のサイズ（テクセルサイズ計算用）
            float pointShadowParams[4];

            // VSM シャドウのパラメータ。
            // x: シャドウモード（0/1=CSM系, 2=VSM 1cascade, 3=VSM 4cascade）
            // y: ライトブリーディング低減値 [0,1]（デフォルト 0.2）
            // z: 最小バリアンス（デフォルト 0.00002）
            // w: ブラー有効フラグ（1.0 = 有効）
            float vsmParams[4];
        };
    }
}
