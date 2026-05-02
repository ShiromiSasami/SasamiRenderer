// =============================================================================
// LightCB.hlsli
// ライティング定数バッファの HLSL 定義。
//
// C++ 側の LightCBData.h（LightCBLayout::LightCBData）と
// バイト単位で一致している必要がある。
//
// 全てのライティングシェーダー（PBR_PS.hlsl, BasicShader.hlsl, SWRT_*, DXR など）
// がこのヘッダーをインクルードして照明データにアクセスする。
// =============================================================================

#ifndef SASAMI_LIGHT_CB_LAYOUT_INCLUDED
#define SASAMI_LIGHT_CB_LAYOUT_INCLUDED

// 拡散照明用球面調和係数の本数（L0 + L1 + L2 = 9 本）
#define DIFFUSE_SH_COEFFICIENT_COUNT 9
#define DIRECTIONAL_SHADOW_CASCADE_COUNT 4

// b1 レジスタに割り当てられるライティング定数バッファ。
// LightSystem::UpdateFrameLighting() が毎フレーム書き込む。
cbuffer LightCB : register(b1)
{
    // シャドウマップ生成時のライト視点ビュープロジェクション行列。
    // ピクセルシェーダーで頂点をライトクリップ空間へ変換し、
    // シャドウマップ深度と比較して遮蔽判定を行う。
    row_major float4x4 u_lightVP[DIRECTIONAL_SHADOW_CASCADE_COUNT];

    // ディレクショナルライト方向と強度。
    // xyz: ワールド空間でのライト方向（ライトへ向かう正規化ベクトル）
    // w:   強度スカラー（ライトカラーに乗算される）
    float4 u_dirDir;      // xyz: forward dir, w: intensity

    // ディレクショナルライト色。
    // rgb: 線形 RGB カラー
    // w:   未使用
    float4 u_dirColor;    // rgb: color

    // ライト数カウント。
    // x: ポイントライト数（StructuredBuffer の要素数）
    // y: スポットライト数
    // zw: 未使用
    float4 u_lightCounts; // x: pointCount, y: spotCount

    // カメラのワールド座標。
    // xyz: カメラ位置（視線ベクトル・反射方向の計算に使用）
    // w:   未使用
    float4 u_cameraPos;   // xyz: camera world position
    row_major float4x4 u_cameraPV;

    // Image-Based Lighting（IBL）パラメータ。
    // x: IBL 有効フラグ（1.0 = 有効、キューブマップ照明を適用する）
    // y: IBL 強度スカラー
    // z: プリフィルタキューブマップの最大 Mip レベル（粗さに応じたサンプリング用）
    // w: 拡散 IBL に球面調和を使うか（1.0 = SH 使用）
    float4 u_iblParams;   // x: IBL enable, y: intensity, z: prefilter max mip, w: diffuse SH enable

    // デバッグ表示モード。
    // x: 0=通常ライティング, 1=アルベド, 2=法線, 3=粗さ, 4=メタリック, 5=AO, 6=シャドウ
    float4 u_debugParams; // x: gbuffer debug view mode

    // シャドウマップのテクセルサイズ（PCF サンプリングオフセット計算用）。
    // x: 1.0 / シャドウマップ幅
    // y: 1.0 / シャドウマップ高さ
    float4 u_shadowParams; // x/y reserved, z: PCF disk radius (texels), w: AO min occlusion
    float4 u_shadowCascadeSplits;
    float4 u_shadowCascadeTexelSize[DIRECTIONAL_SHADOW_CASCADE_COUNT];
    float4 u_shadowCascadeParams; // x: shadow distance, y: blend fraction, z: normal bias, w: cascade count
    float4 u_contactShadowParams; // x: enable, y: march distance, z: depth thickness, w: step count

    // リフレクションテクスチャのパラメータ。
    // x: リフレクション有効フラグ
    // y: 反射強度スカラー
    // z: 描画幅（スクリーン座標 → UV 変換用）
    // w: 描画高さ
    float4 u_reflectionParams; // x: reflection enable, y: strength, z: screen width, w: screen height

    // 拡散照明用の球面調和係数（9 本）。
    // xyz に係数値を格納（w は未使用）。
    // PBR_PS.hlsl の EvaluateDiffuseIrradianceFromSh() で参照される。
    float4 u_diffuseSh[DIFFUSE_SH_COEFFICIENT_COUNT];

    // スポットライト #0 のシャドウ用ビュープロジェクション行列。
    row_major float4x4 u_spotLightVP;

    // スポットシャドウのパラメータ。
    // x: 深度バイアス, y: ニアプレーン, z: 有効フラグ(1=有効), w: マップサイズ
    float4 u_spotShadowParams;
}

#endif
