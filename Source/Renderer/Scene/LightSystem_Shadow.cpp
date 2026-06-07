// LightSystem_Shadow.cpp
// Shadow pass context building and execution.
// =============================================================================
// LightSystem.cpp
// ライトデータの管理と GPU バッファへのアップロード実装。
//
// 【データフロー】
//   CPU (m_pointLights etc.)
//     → EnsureLightBuffers() でアップロードバッファを確保・拡張
//     → UpdateFrameLighting() でライトデータを書き込み
//     → ExecuteShadowPass() でシャドウマップを描画
//     → LightingRenderPass が SRV を参照してライティングを実行
// =============================================================================

#include "Renderer/Scene/LightSystem.h"
#include "Renderer/Scene/ShadowMapManager.h"

#include <cmath>
#include <cstring>
#include <limits>

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Scene/LightCBData.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "Foundation/Tools/ScopedPerfTimer.h"
#include "d3dx12.h"

namespace
{
    // =========================================================================
    // GPU 転送用ライト構造体
    // シェーダー側の StructuredBuffer レイアウトと一致する必要がある。
    // RayTracing.hlsl の PointLightData / SpotLightData と対応。
    // =========================================================================

    // ポイントライト GPU レイアウト（32 バイト）
    struct PointLightGPU
    {
        float posRange[4];       // xyz: ワールド座標, w: 有効半径
        float colorIntensity[4]; // rgb: 色（線形）, w: 強度スカラー
    };

    // スポットライト GPU レイアウト（64 バイト）
    struct SpotLightGPU
    {
        float posRange[4];       // xyz: ワールド座標, w: 有効半径
        float dirCosInner[4];    // xyz: 照射方向（正規化）, w: コーン内角のコサイン値
        float colorIntensity[4]; // rgb: 色（線形）, w: 強度スカラー
        float params[4];         // x: コーン外角のコサイン値, yzw: 未使用
    };

    inline float Dot3(const float a[3], const float b[3])
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    inline void TransformPoint(const float m[16], const float p[3], float out[3])
    {
        const float x = p[0] * m[0] + p[1] * m[4] + p[2] * m[8]  + m[12];
        const float y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9]  + m[13];
        const float z = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
        const float w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];

        if (std::fabs(w) > 1e-7f) {
            out[0] = x / w;
            out[1] = y / w;
            out[2] = z / w;
        } else {
            out[0] = x;
            out[1] = y;
            out[2] = z;
        }
    }

    inline float RoundToMultiple(float value, float step)
    {
        if (step <= 1e-7f) {
            return value;
        }
        return std::floor(value / step + 0.5f) * step;
    }

    inline float Clamp01(float v)
    {
        if (v < 0.0f) return 0.0f;
        if (v > 1.0f) return 1.0f;
        return v;
    }

    inline void Lerp3(const float a[3], const float b[3], float t, float out[3])
    {
        out[0] = a[0] + (b[0] - a[0]) * t;
        out[1] = a[1] + (b[1] - a[1]) * t;
        out[2] = a[2] + (b[2] - a[2]) * t;
    }
}


namespace SasamiRenderer
{
    void LightSystem::BuildShadowPassContext(ShadowPassContext& outContext,
                                             const float cameraPos[3],
                                             const float cameraPV[16],
                                             uint32_t shadowMapWidth,
                                             uint32_t shadowMapHeight) const
    {
        std::memset(&outContext, 0, sizeof(outContext));

        float lightForward[3] = {};
        Math::DirectionFromYawPitch(m_lightYaw, m_lightPitch, lightForward);

        const uint32_t cascadeCount =
            (m_shadowMode == DirectionalShadowMode::Csm4 ||
             m_shadowMode == DirectionalShadowMode::Vsm4)
            ? kDirectionalCascadeCount
            : 1u;
        outContext.cascadeCount = cascadeCount;
        outContext.cascadeBlendFraction = m_cascadeBlendFraction;

        float invCameraPV[16] = {};
        if (cameraPV == nullptr ||
            cameraPos == nullptr ||
            shadowMapWidth == 0u ||
            shadowMapHeight == 0u ||
            !Math::Invert4x4(cameraPV, invCameraPV)) {
            Math::BuildDirectionalLightViewProjection(m_lightYaw,
                                                      m_lightPitch,
                                                      m_lightDistance,
                                                      m_lightOrthoHalf,
                                                      m_lightNear,
                                                      m_lightFar,
                                                      outContext.lightViewProjection,
                                                      lightForward);
            std::memcpy(outContext.cascadeLightViewProjection[0],
                        outContext.lightViewProjection,
                        sizeof(outContext.lightViewProjection));
            outContext.cascadeSplitDepths[0] = 1.0f;
            outContext.cascadeTexelSize[0][0] = (shadowMapWidth > 0u) ? (1.0f / static_cast<float>(shadowMapWidth)) : 0.0f;
            outContext.cascadeTexelSize[0][1] = (shadowMapHeight > 0u) ? (1.0f / static_cast<float>(shadowMapHeight)) : 0.0f;
            return;
        }

        float referenceUp[3] = { 0.0f, 1.0f, 0.0f };
        if (std::fabs(lightForward[1]) > 0.99f) {
            referenceUp[0] = 0.0f;
            referenceUp[1] = 0.0f;
            referenceUp[2] = 1.0f;
        }

        float lightRight[3] = {};
        Math::Cross(lightForward, referenceUp, lightRight);
        Math::Normalize(lightRight);

        float lightUp[3] = {};
        Math::Cross(lightRight, lightForward, lightUp);
        Math::Normalize(lightUp);

        float nearCorners[4][3] = {};
        float farCorners[4][3] = {};
        const float clipCorners[4][2] = {
            { -1.0f, -1.0f },
            {  1.0f, -1.0f },
            { -1.0f,  1.0f },
            {  1.0f,  1.0f },
        };
        for (int i = 0; i < 4; ++i) {
            const float nearClipPoint[3] = { clipCorners[i][0], clipCorners[i][1], 0.0f };
            const float farClipPoint[3]  = { clipCorners[i][0], clipCorners[i][1], 1.0f };
            TransformPoint(invCameraPV, nearClipPoint, nearCorners[i]);
            TransformPoint(invCameraPV, farClipPoint, farCorners[i]);
        }

        float nearCenter[3] = {};
        float farCenter[3] = {};
        for (int i = 0; i < 4; ++i) {
            nearCenter[0] += nearCorners[i][0];
            nearCenter[1] += nearCorners[i][1];
            nearCenter[2] += nearCorners[i][2];
            farCenter[0] += farCorners[i][0];
            farCenter[1] += farCorners[i][1];
            farCenter[2] += farCorners[i][2];
        }
        nearCenter[0] *= 0.25f; nearCenter[1] *= 0.25f; nearCenter[2] *= 0.25f;
        farCenter[0] *= 0.25f;  farCenter[1] *= 0.25f;  farCenter[2] *= 0.25f;

        const float nearDx = nearCenter[0] - cameraPos[0];
        const float nearDy = nearCenter[1] - cameraPos[1];
        const float nearDz = nearCenter[2] - cameraPos[2];
        const float farDx = farCenter[0] - cameraPos[0];
        const float farDy = farCenter[1] - cameraPos[1];
        const float farDz = farCenter[2] - cameraPos[2];
        const float fullNearDistance = (std::max)(0.001f, std::sqrt(nearDx * nearDx + nearDy * nearDy + nearDz * nearDz));
        const float fullFarDistance = (std::max)(fullNearDistance + 0.01f, std::sqrt(farDx * farDx + farDy * farDy + farDz * farDz));
        const float shadowDistance = (std::min)(m_shadowDistance, fullFarDistance);
        const float splitRange = (std::max)(0.001f, fullFarDistance - fullNearDistance);

        float splitDistances[kDirectionalCascadeCount] = {};
        for (uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex) {
            const float p = static_cast<float>(cascadeIndex + 1u) / static_cast<float>(cascadeCount);
            const float splitRatio = std::pow(p, (std::max)(1.0f, m_cascadeDistributionExponent));
            splitDistances[cascadeIndex] = fullNearDistance + (shadowDistance - fullNearDistance) * splitRatio;
        }
        for (uint32_t cascadeIndex = cascadeCount; cascadeIndex < kDirectionalCascadeCount; ++cascadeIndex) {
            splitDistances[cascadeIndex] = shadowDistance;
        }

        for (uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex) {
            const float cascadeNearDistance = (cascadeIndex == 0u)
                ? fullNearDistance
                : splitDistances[cascadeIndex - 1u];
            const float cascadeFarDistance = splitDistances[cascadeIndex];
            const float nearT = Clamp01((cascadeNearDistance - fullNearDistance) / splitRange);
            const float farT = Clamp01((cascadeFarDistance - fullNearDistance) / splitRange);

            float cascadeCorners[8][3] = {};
            for (int cornerIndex = 0; cornerIndex < 4; ++cornerIndex) {
                Lerp3(nearCorners[cornerIndex], farCorners[cornerIndex], nearT, cascadeCorners[cornerIndex + 0]);
                Lerp3(nearCorners[cornerIndex], farCorners[cornerIndex], farT,  cascadeCorners[cornerIndex + 4]);
            }

            float frustumCenter[3] = {};
            for (const auto& corner : cascadeCorners) {
                frustumCenter[0] += corner[0];
                frustumCenter[1] += corner[1];
                frustumCenter[2] += corner[2];
            }
            frustumCenter[0] *= 0.125f;
            frustumCenter[1] *= 0.125f;
            frustumCenter[2] *= 0.125f;

            float radiusSq = 0.0f;
            for (const auto& corner : cascadeCorners) {
                const float dx = corner[0] - frustumCenter[0];
                const float dy = corner[1] - frustumCenter[1];
                const float dz = corner[2] - frustumCenter[2];
                const float distSq = dx * dx + dy * dy + dz * dz;
                if (distSq > radiusSq) {
                    radiusSq = distSq;
                }
            }

            const float frustumRadius = std::sqrt(radiusSq);
            const float depthMargin = (std::max)(2.0f, frustumRadius * 0.1f);
            const float lightOffset = (std::max)(m_lightDistance, frustumRadius + depthMargin);
            const float lightPosition[3] = {
                frustumCenter[0] - lightForward[0] * lightOffset,
                frustumCenter[1] - lightForward[1] * lightOffset,
                frustumCenter[2] - lightForward[2] * lightOffset
            };

            const float lightView[16] = {
                lightRight[0],   lightUp[0],   lightForward[0], 0.0f,
                lightRight[1],   lightUp[1],   lightForward[1], 0.0f,
                lightRight[2],   lightUp[2],   lightForward[2], 0.0f,
                -Dot3(lightPosition, lightRight),
                -Dot3(lightPosition, lightUp),
                -Dot3(lightPosition, lightForward),
                1.0f
            };

            float lightSpaceCenter[3] = {};
            TransformPoint(lightView, frustumCenter, lightSpaceCenter);

            const float xyMargin = (std::max)(1.0f, frustumRadius * 0.05f);
            const float stableHalfExtent = (std::max)(m_lightOrthoHalf, frustumRadius + xyMargin);
            const float halfWidth = stableHalfExtent;
            const float halfHeight = stableHalfExtent;
            const float texelWorldX = (halfWidth * 2.0f) / static_cast<float>((std::max)(1u, shadowMapWidth));
            const float texelWorldY = (halfHeight * 2.0f) / static_cast<float>((std::max)(1u, shadowMapHeight));
            const float centerX = RoundToMultiple(lightSpaceCenter[0], texelWorldX);
            const float centerY = RoundToMultiple(lightSpaceCenter[1], texelWorldY);
            const float minX = centerX - halfWidth;
            const float maxX = centerX + halfWidth;
            const float minY = centerY - halfHeight;
            const float maxY = centerY + halfHeight;

            const float minimumDepthSpan = (std::max)(0.001f, m_lightFar - m_lightNear);
            const float stableDepthHalf = (std::max)(minimumDepthSpan * 0.5f, frustumRadius + depthMargin);
            float depthCenter = RoundToMultiple(lightSpaceCenter[2], (std::max)(0.5f, stableDepthHalf * 0.05f));
            const float minZ = (std::max)(0.001f, depthCenter - stableDepthHalf);
            const float maxZ = minZ + stableDepthHalf * 2.0f;

            if (maxX <= minX || maxY <= minY || maxZ <= minZ) {
                Math::BuildDirectionalLightViewProjection(m_lightYaw,
                                                          m_lightPitch,
                                                          m_lightDistance,
                                                          m_lightOrthoHalf,
                                                          m_lightNear,
                                                          m_lightFar,
                                                          outContext.cascadeLightViewProjection[cascadeIndex],
                                                          lightForward);
            } else {
                const float lightProjection[16] = {
                    2.0f / (maxX - minX), 0.0f,                 0.0f,                0.0f,
                    0.0f,                 2.0f / (maxY - minY),0.0f,                0.0f,
                    0.0f,                 0.0f,                 1.0f / (maxZ - minZ),0.0f,
                    -(maxX + minX) / (maxX - minX),
                    -(maxY + minY) / (maxY - minY),
                    -minZ / (maxZ - minZ),
                    1.0f
                };
                Math::Mul4x4(lightView, lightProjection, outContext.cascadeLightViewProjection[cascadeIndex]);
            }

            float splitCenter[3] = {};
            for (int i = 4; i < 8; ++i) {
                splitCenter[0] += cascadeCorners[i][0];
                splitCenter[1] += cascadeCorners[i][1];
                splitCenter[2] += cascadeCorners[i][2];
            }
            splitCenter[0] *= 0.25f;
            splitCenter[1] *= 0.25f;
            splitCenter[2] *= 0.25f;
            float splitClip[3] = {};
            TransformPoint(cameraPV, splitCenter, splitClip);
            outContext.cascadeSplitDepths[cascadeIndex] = Clamp01(splitClip[2]);
            outContext.cascadeTexelSize[cascadeIndex][0] = texelWorldX;
            outContext.cascadeTexelSize[cascadeIndex][1] = texelWorldY;
        }

        std::memcpy(outContext.lightViewProjection,
                    outContext.cascadeLightViewProjection[0],
                    sizeof(outContext.lightViewProjection));
        outContext.activeCascadeIndex = 0u;
    }

    void LightSystem::BuildStableDirectionalShadowPassContext(ShadowPassContext& outContext,
                                                              uint32_t shadowMapWidth,
                                                              uint32_t shadowMapHeight) const
    {
        float lightForward[3] = {};

        const float stableOrthoHalf = (std::max)(m_lightOrthoHalf, m_shadowDistance * 0.25f);
        const float stableNear = (std::max)(0.001f,
                                            (std::min)(m_lightNear, m_lightDistance - m_shadowDistance));
        const float stableFar = (std::max)((std::max)(m_lightFar, stableNear + 1.0f),
                                           m_lightDistance + m_shadowDistance);

        Math::BuildDirectionalLightViewProjection(m_lightYaw,
                                                  m_lightPitch,
                                                  m_lightDistance,
                                                  stableOrthoHalf,
                                                  stableNear,
                                                  stableFar,
                                                  outContext.lightViewProjection,
                                                  lightForward);

        outContext.cascadeCount = 1u;
        outContext.activeCascadeIndex = 0u;
        outContext.cascadeBlendFraction = 0.0f;

        const float texelWorld = (stableOrthoHalf * 2.0f) /
            static_cast<float>((std::max)(1u, (std::max)(shadowMapWidth, shadowMapHeight)));
        for (uint32_t cascadeIndex = 0; cascadeIndex < kDirectionalCascadeCount; ++cascadeIndex) {
            std::memcpy(outContext.cascadeLightViewProjection[cascadeIndex],
                        outContext.lightViewProjection,
                        sizeof(outContext.lightViewProjection));
            outContext.cascadeSplitDepths[cascadeIndex] = 1.0f;
            outContext.cascadeTexelSize[cascadeIndex][0] = texelWorld;
            outContext.cascadeTexelSize[cascadeIndex][1] = texelWorld;
        }
    }

    // =========================================================================
    // UpdateFrameLighting
    // 毎フレーム呼び、全ライトデータを CPU アップロードバッファに書き込む。
    //
    // 【書き込む内容】
    //  1. シャドウ VP 行列
    //  2. ディレクショナルライトの方向・色・強度
    //  3. ポイントライト配列（StructuredBuffer）
    //  4. スポットライト配列（StructuredBuffer）
    //  5. カメラ座標、IBL パラメータ、デバッグモード、シャドウ解像度、リフレクション設定
    //  6. 拡散 SH 係数（IBL 拡散照明）
    // =========================================================================
    void LightSystem::UpdateFrameLighting(FrameResources& frame,
                                          const float cameraPos[3],
                                          const float cameraPV[16],
                                          bool iblEnabled,
                                          float iblIntensity,
                                          float iblPrefilterMaxMip,
                                          bool hasDiffuseSh,
                                          const float (*diffuseShCoefficients)[3],
                                          RendererEnums::GBufferDebugView debugView,
                                          uint32_t shadowMapWidth,
                                          uint32_t shadowMapHeight,
                                          float reflectionMode,
                                          float reflectionStrength,
                                          uint32_t renderWidth,
                                          uint32_t renderHeight,
                                          bool useStableDirectionalShadowProjection)
    {
        if (!frame.lightCB.IsValid() || frame.lightCBPtr == nullptr) {
            return;
        }

        // --- ステップ1: シャドウ VP 行列を計算 ---
        ShadowPassContext shadowContext{};
        if (useStableDirectionalShadowProjection) {
            BuildStableDirectionalShadowPassContext(shadowContext, shadowMapWidth, shadowMapHeight);
        } else {
            BuildShadowPassContext(shadowContext, cameraPos, cameraPV, shadowMapWidth, shadowMapHeight);
        }

        // --- ステップ2: ディレクショナルライトの方向ベクトルを計算 ---
        // yaw/pitch から単位ベクトルを生成する（MathUtil の汎用関数を使用）
        float lightForward[3] = {};
        Math::DirectionFromYawPitch(m_lightYaw, m_lightPitch, lightForward);

        // --- ステップ3: ライトバッファ容量を確保・拡張 ---
        EnsureLightBuffers(frame, m_pointLights.size(), m_spotLights.size());

        size_t pointCount = m_pointLights.size();
        size_t spotCount  = m_spotLights.size();

        // --- ステップ4: ポイントライトデータをアップロードバッファに書き込む ---
        if (!frame.pointLightBufferPtr) {
            pointCount = 0;
        } else {
            auto* pointDst = reinterpret_cast<PointLightGPU*>(frame.pointLightBufferPtr);
            for (size_t i = 0; i < pointCount; ++i) {
                const auto& pl = m_pointLights[i];
                pointDst[i].posRange[0] = pl.pos[0];
                pointDst[i].posRange[1] = pl.pos[1];
                pointDst[i].posRange[2] = pl.pos[2];
                pointDst[i].posRange[3] = pl.range;          // w = 有効半径
                pointDst[i].colorIntensity[0] = pl.color[0];
                pointDst[i].colorIntensity[1] = pl.color[1];
                pointDst[i].colorIntensity[2] = pl.color[2];
                pointDst[i].colorIntensity[3] = pl.intensity; // w = 強度
            }
        }

        // --- ステップ5: スポットライトデータをアップロードバッファに書き込む ---
        // スポットライトの方向は yaw/pitch から計算し、
        // コーン角度はコサイン値に変換してシェーダーで smoothstep に使う。
        if (!frame.spotLightBufferPtr) {
            spotCount = 0;
        } else {
            auto* spotDst = reinterpret_cast<SpotLightGPU*>(frame.spotLightBufferPtr);
            for (size_t i = 0; i < spotCount; ++i) {
                const auto& sl = m_spotLights[i];
                float spotDir[3] = {};
                Math::DirectionFromYawPitch(sl.yaw, sl.pitch, spotDir);

                // inner > outer になってしまう場合は inner を outer にクランプ
                float inner = sl.innerAngle;
                float outer = sl.outerAngle;
                if (inner > outer) {
                    inner = outer;
                }
                const float cosInner = std::cos(inner); // smoothstep の上端（完全照射）
                const float cosOuter = std::cos(outer); // smoothstep の下端（照射なし）

                spotDst[i].posRange[0] = sl.pos[0];
                spotDst[i].posRange[1] = sl.pos[1];
                spotDst[i].posRange[2] = sl.pos[2];
                spotDst[i].posRange[3] = sl.range;
                spotDst[i].dirCosInner[0] = spotDir[0];
                spotDst[i].dirCosInner[1] = spotDir[1];
                spotDst[i].dirCosInner[2] = spotDir[2];
                spotDst[i].dirCosInner[3] = cosInner;   // w = コーン内角コサイン
                spotDst[i].colorIntensity[0] = sl.color[0];
                spotDst[i].colorIntensity[1] = sl.color[1];
                spotDst[i].colorIntensity[2] = sl.color[2];
                spotDst[i].colorIntensity[3] = sl.intensity;
                spotDst[i].params[0] = cosOuter;         // x = コーン外角コサイン
                spotDst[i].params[1] = 0.0f;
                spotDst[i].params[2] = 0.0f;
                spotDst[i].params[3] = 0.0f;
            }
        }

        // --- ステップ6: 定数バッファ（LightCBData）を構築して書き込む ---
        LightCBLayout::LightCBData cb = {};
        std::memcpy(cb.lightVP, shadowContext.cascadeLightViewProjection, sizeof(cb.lightVP));

        cb.dirDir[0] = lightForward[0];
        cb.dirDir[1] = lightForward[1];
        cb.dirDir[2] = lightForward[2];
        cb.dirDir[3] = m_dirIntensity;

        cb.dirColor[0] = m_dirColor[0];
        cb.dirColor[1] = m_dirColor[1];
        cb.dirColor[2] = m_dirColor[2];
        cb.dirColor[3] = 1.0f;

        cb.lightCounts[0] = static_cast<float>(pointCount);
        cb.lightCounts[1] = static_cast<float>(spotCount);

        cb.cameraPos[0] = cameraPos[0];
        cb.cameraPos[1] = cameraPos[1];
        cb.cameraPos[2] = cameraPos[2];
        cb.cameraPos[3] = 1.0f;
        std::memcpy(cb.cameraPV, cameraPV, sizeof(cb.cameraPV));
        {
            float invCamPV[16] = {};
            if (!Math::Invert4x4(cameraPV, invCamPV)) {
                for (int i = 0; i < 16; ++i) invCamPV[i] = (i % 5 == 0) ? 1.0f : 0.0f;
            }
            std::memcpy(cb.invCameraPV, invCamPV, sizeof(cb.invCameraPV));
        }

        cb.iblParams[0] = iblEnabled ? 1.0f : 0.0f;
        cb.iblParams[1] = iblIntensity;
        cb.iblParams[2] = iblPrefilterMaxMip;
        cb.iblParams[3] = (hasDiffuseSh && diffuseShCoefficients != nullptr) ? 1.0f : 0.0f;

        cb.debugParams[0] = static_cast<float>(static_cast<int>(debugView));
        cb.debugParams[1] = 0.0f;
        cb.debugParams[2] = 0.0f;
        cb.debugParams[3] = 0.0f;

        // シャドウマップの逆解像度（PCF テクスチャ座標オフセット計算用）
        cb.shadowParams[0] = (shadowMapWidth  > 0u) ? (1.0f / static_cast<float>(shadowMapWidth))  : 0.0f;
        cb.shadowParams[1] = (shadowMapHeight > 0u) ? (1.0f / static_cast<float>(shadowMapHeight)) : 0.0f;
        cb.shadowParams[2] = useStableDirectionalShadowProjection ? 1.25f : 2.0f; // SWRTは少し細かくする
        cb.shadowParams[3] = m_aoMinOcclusion;  // AO minimum occlusion (UE-style floor)
        for (uint32_t cascadeIndex = 0; cascadeIndex < kDirectionalCascadeCount; ++cascadeIndex) {
            cb.shadowCascadeSplits[cascadeIndex] = shadowContext.cascadeSplitDepths[cascadeIndex];
            cb.shadowCascadeTexelSize[cascadeIndex][0] = shadowContext.cascadeTexelSize[cascadeIndex][0];
            cb.shadowCascadeTexelSize[cascadeIndex][1] = shadowContext.cascadeTexelSize[cascadeIndex][1];
            const float cascadeFarT = (kDirectionalCascadeCount > 1u)
                ? (static_cast<float>(cascadeIndex) / static_cast<float>(kDirectionalCascadeCount - 1u))
                : 0.0f;
            const float cascadeBiasScale = 1.0f + (m_shadowFarBiasScale - 1.0f) * cascadeFarT;
            cb.shadowCascadeTexelSize[cascadeIndex][2] = (m_shadowDepthBias * 1.0e-6f) * cascadeBiasScale;
            cb.shadowCascadeTexelSize[cascadeIndex][3] = (m_shadowSlopeScaleBias * 5.0e-4f) * cascadeBiasScale;
        }
        cb.shadowCascadeParams[0] = m_shadowDistance;
        cb.shadowCascadeParams[1] = m_cascadeBlendFraction;
        cb.shadowCascadeParams[2] = m_shadowNormalBias;
        cb.shadowCascadeParams[3] = static_cast<float>(shadowContext.cascadeCount);

        cb.contactShadowParams[0] = useStableDirectionalShadowProjection ? 0.0f : 1.0f;
        cb.contactShadowParams[1] = 0.75f;
        cb.contactShadowParams[2] = 0.0025f;
        cb.contactShadowParams[3] = 12.0f;

        cb.reflectionParams[0] = reflectionMode;
        cb.reflectionParams[1] = reflectionStrength;
        cb.reflectionParams[2] = static_cast<float>(renderWidth);
        cb.reflectionParams[3] = static_cast<float>(renderHeight);

        // --- 球面調和係数をコピー（なければ 0 埋め）---
        for (size_t i = 0; i < LightCBLayout::kDiffuseShCoefficientCount; ++i) {
            cb.diffuseSh[i][0] = (hasDiffuseSh && diffuseShCoefficients != nullptr) ? diffuseShCoefficients[i][0] : 0.0f;
            cb.diffuseSh[i][1] = (hasDiffuseSh && diffuseShCoefficients != nullptr) ? diffuseShCoefficients[i][1] : 0.0f;
            cb.diffuseSh[i][2] = (hasDiffuseSh && diffuseShCoefficients != nullptr) ? diffuseShCoefficients[i][2] : 0.0f;
            cb.diffuseSh[i][3] = 0.0f;
        }

        // --- スポットライト #0 のシャドウ VP を計算して書き込む ---
        if (!m_spotLights.empty()) {
            const auto& sl0 = m_spotLights[0];
            const float spotPos[3] = { sl0.pos[0], sl0.pos[1], sl0.pos[2] };
            Math::BuildSpotLightViewProjection(sl0.yaw, sl0.pitch, spotPos,
                                               sl0.outerAngle, 0.1f, sl0.range,
                                               cb.spotLightVP);
            cb.spotShadowParams[0] = 0.0005f;                              // 深度バイアス
            cb.spotShadowParams[1] = 0.1f;                                 // ニアプレーン（参照用）
            cb.spotShadowParams[2] = 1.0f;                                 // 有効
            cb.spotShadowParams[3] = static_cast<float>(m_shadowMapManager.GetSpotShadowMapSize());
        } else {
            std::memset(cb.spotLightVP, 0, sizeof(cb.spotLightVP));
            cb.spotShadowParams[0] = 0.0f;
            cb.spotShadowParams[1] = 0.0f;
            cb.spotShadowParams[2] = 0.0f; // 無効
            cb.spotShadowParams[3] = 0.0f;
        }

        // VSM シャドウパラメータ
        cb.vsmParams[0] = static_cast<float>(static_cast<uint32_t>(m_shadowMode));
        cb.vsmParams[1] = 0.2f;     // light bleeding reduction
        cb.vsmParams[2] = 0.00002f; // min variance
        cb.vsmParams[3] = 1.0f;     // blur enabled (updated by ExecuteShadowPass caller)

        // 定数バッファのアップロードバッファに書き込む（永続 Map 済み）
        std::memcpy(frame.lightCBPtr, &cb, sizeof(cb));
    }

    // =========================================================================
    // ExecuteShadowPass
    // シャドウマップの描画パスを実行する。
    //
    // 【処理フロー】
    //  1. UpdateFrameLighting() でライトデータを更新
    //  2. EnsureShadowResources() でシャドウマップを遅延生成
    //  3. シャドウマップをリソースバリアで DEPTH_WRITE 状態へ遷移
    //  4. パイプラインステート・ディスクリプタヒープ・定数バッファを設定
    //  5. シャドウ DSV をクリアし、drawCallback でオブジェクトを描画
    //  6. シャドウマップを PIXEL_SHADER_RESOURCE 状態に戻す
    // =========================================================================
    void LightSystem::ExecuteShadowPass(CommandList* cmdList,
                                        FrameResources& frame,
                                        RenderPipelineStateCache& pipelineStateCache,
                                        DescriptorHeap& srvHeap,
                                        const float cameraPos[3],
                                        const float cameraPV[16],
                                        bool useTessellationPath,
                                        bool iblEnabled,
                                        float iblIntensity,
                                        float iblPrefilterMaxMip,
                                        bool hasDiffuseSh,
                                        const float (*diffuseShCoefficients)[3],
                                        RendererEnums::GBufferDebugView debugView,
                                        uint32_t shadowMapWidth,
                                        uint32_t shadowMapHeight,
                                        float reflectionMode,
                                        float reflectionStrength,
                                        uint32_t renderWidth,
                                        uint32_t renderHeight,
                                        const DrawShadowCallback& drawCallback,
                                        bool vsmBlurEnabled)
    {
        if (!cmdList || !frame.lightCB.IsValid()) {
            return;
        }

        // --- ステップ1: ライトデータを CPU バッファに書き込む ---
        UpdateFrameLighting(frame,
                            cameraPos,
                            cameraPV,
                            iblEnabled,
                            iblIntensity,
                            iblPrefilterMaxMip,
                            hasDiffuseSh,
                            diffuseShCoefficients,
                            debugView,
                            shadowMapWidth,
                            shadowMapHeight,
                            reflectionMode,
                            reflectionStrength,
                            renderWidth,
                            renderHeight);

        // --- ステップ2: シャドウマップリソースを遅延生成（初回のみ）---
        if (!m_shadowMapManager.EnsureShadowResources()) {
            return;
        }

        // --- ステップ3: シャドウマップを深度書き込み状態へ遷移 ---
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMapManager.GetShadowMap().Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmdList->ResourceBarrier(1, &barrier);

        // --- ステップ4: パイプラインステートと各種設定 ---
        cmdList->SetGraphicsRootSignature(pipelineStateCache.GetRootSignature());
        // テッセレーションパスが有効な場合は専用の PSO を使う
        if (useTessellationPath) {
            cmdList->SetPipelineState(pipelineStateCache.GetTessellationShadowPipelineState());
        } else {
            cmdList->SetPipelineState(pipelineStateCache.GetShadowPipelineState());
        }

        DescriptorHeap* heaps[] = { &srvHeap };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootDescriptorTable(1, m_shadowMapManager.GetShadowSrv()); // シャドウマップ SRV（自身）
        cmdList->SetGraphicsRootDescriptorTable(4, frame.lightSrvTable); // ライト StructuredBuffer SRV

        cmdList->RSSetViewports(1, &m_shadowMapManager.GetShadowViewport());
        cmdList->RSSetScissorRects(1, &m_shadowMapManager.GetShadowScissor());
        // テッセレーションパスはコントロールポイントパッチリストで描画
        cmdList->IASetPrimitiveTopology(useTessellationPath
            ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
            : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // --- ステップ5: 深度バッファをクリアして描画コールバックを呼ぶ ---
        auto dsv = m_shadowMapManager.GetDirectionalCascadeDsv(0);
        cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv); // カラーターゲットなし（深度のみ）
        cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        ShadowPassContext shadowContext{};
        BuildShadowPassContext(shadowContext, cameraPos, cameraPV, shadowMapWidth, shadowMapHeight);

        // 定数バッファを b3 に設定
        cmdList->SetGraphicsRootConstantBufferView(3, frame.lightCB->GetGPUVirtualAddress());

        if (drawCallback) {
            for (uint32_t cascadeIndex = 0; cascadeIndex < shadowContext.cascadeCount; ++cascadeIndex) {
                ShadowPassContext cascadeContext = shadowContext;
                cascadeContext.activeCascadeIndex = cascadeIndex;
                std::memcpy(cascadeContext.lightViewProjection,
                            shadowContext.cascadeLightViewProjection[cascadeIndex],
                            sizeof(cascadeContext.lightViewProjection));
                auto cascadeDsv = m_shadowMapManager.GetDirectionalCascadeDsv(cascadeIndex);
                cmdList->OMSetRenderTargets(0, nullptr, FALSE, &cascadeDsv);
                cmdList->ClearDepthStencilView(cascadeDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                drawCallback(cascadeContext);
            }
            if (false) {
            drawCallback(shadowContext); // 呼び出し元がメッシュを描画する
            }
        }

        // --- ステップ6: シャドウマップをシェーダー参照可能状態に戻す ---
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMapManager.GetShadowMap().Get(),
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);

        // --- ステップ6b: VSM シャドウパス（Vsm / Vsm4 モード）---
        const bool isVsmMode = (m_shadowMode == DirectionalShadowMode::Vsm ||
                                m_shadowMode == DirectionalShadowMode::Vsm4);
        if (isVsmMode && m_shadowMapManager.EnsureVsmResources() && m_shadowMapManager.EnsureShadowResources())
        {
            // vsmMap をレンダーターゲット状態に遷移
            {
                auto vsmBarrier = CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMapManager.GetVsmMap().Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
                cmdList->ResourceBarrier(1, &vsmBarrier);
            }
            // shadowMap を深度書き込み状態に遷移（再度）
            {
                auto depthBarrier = CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMapManager.GetShadowMap().Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE);
                cmdList->ResourceBarrier(1, &depthBarrier);
            }

            cmdList->SetGraphicsRootSignature(pipelineStateCache.GetRootSignature());
            cmdList->SetPipelineState(pipelineStateCache.GetShadowVsmPipelineState());
            DescriptorHeap* heapsVsm[] = { &srvHeap };
            cmdList->SetDescriptorHeaps(1, heapsVsm);
            cmdList->SetGraphicsRootConstantBufferView(3, frame.lightCB->GetGPUVirtualAddress());
            cmdList->RSSetViewports(1, &m_shadowMapManager.GetShadowViewport());
            cmdList->RSSetScissorRects(1, &m_shadowMapManager.GetShadowScissor());
            cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            if (drawCallback) {
                static const float kClearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                for (uint32_t ci = 0; ci < shadowContext.cascadeCount; ++ci) {
                    ShadowPassContext cascadeCtx = shadowContext;
                    cascadeCtx.activeCascadeIndex = ci;
                    std::memcpy(cascadeCtx.lightViewProjection,
                                shadowContext.cascadeLightViewProjection[ci],
                                sizeof(cascadeCtx.lightViewProjection));
                    auto vsmRtv = m_shadowMapManager.GetVsmRtv(ci);
                    auto cascadeDsv = m_shadowMapManager.GetDirectionalCascadeDsv(ci);
                    cmdList->OMSetRenderTargets(1, &vsmRtv, FALSE, &cascadeDsv);
                    cmdList->ClearRenderTargetView(vsmRtv, kClearColor, 0, nullptr);
                    cmdList->ClearDepthStencilView(cascadeDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                    drawCallback(cascadeCtx);
                }
            }

            // vsmMap を適切な次状態へ遷移, shadowMap を PSR へ戻す
            {
                D3D12_RESOURCE_STATES vsmNextState = vsmBlurEnabled
                    ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                    : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                D3D12_RESOURCE_BARRIER postBarriers[2] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMapManager.GetVsmMap().Get(),
                        D3D12_RESOURCE_STATE_RENDER_TARGET, vsmNextState),
                    CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMapManager.GetShadowMap().Get(),
                        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                };
                cmdList->ResourceBarrier(2, postBarriers);
            }

            if (vsmBlurEnabled &&
                pipelineStateCache.GetShadowVsmBlurHPipelineState().Get() &&
                pipelineStateCache.GetShadowVsmBlurVPipelineState().Get())
            {
                DescriptorHeap& vsmBlurDescHeap = m_shadowMapManager.GetVsmBlurDescHeap();
                ID3D12DescriptorHeap* blurHeap = vsmBlurDescHeap.Get();
                cmdList->SetDescriptorHeaps(1, &blurHeap);

                const UINT bInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                D3D12_GPU_DESCRIPTOR_HANDLE blurBase = vsmBlurDescHeap->GetGPUDescriptorHandleForHeapStart();
                D3D12_GPU_DESCRIPTOR_HANDLE srvH = { blurBase.ptr + 0 * bInc }; // vsmMap SRV (H-pass input)
                D3D12_GPU_DESCRIPTOR_HANDLE uavH = { blurBase.ptr + 1 * bInc }; // vsmMapTemp UAV (H-pass output)
                D3D12_GPU_DESCRIPTOR_HANDLE srvV = { blurBase.ptr + 2 * bInc }; // vsmMapTemp SRV (V-pass input)
                D3D12_GPU_DESCRIPTOR_HANDLE uavV = { blurBase.ptr + 3 * bInc }; // vsmMap UAV (V-pass output)

                const UINT shadowMapSize = m_shadowMapManager.GetShadowMapSize();
                const UINT dispatchCount = (shadowMapSize + 7u) / 8u;

                // H-blur: vsmMap → vsmMapTemp
                cmdList->SetComputeRootSignature(pipelineStateCache.GetShadowVsmBlurRootSignature().Get());
                cmdList->SetPipelineState(pipelineStateCache.GetShadowVsmBlurHPipelineState().Get());
                for (uint32_t ci = 0; ci < shadowContext.cascadeCount; ++ci) {
                    uint32_t constants[4] = { shadowMapSize, shadowMapSize, ci, 0u };
                    cmdList->SetComputeRoot32BitConstants(0, 4, constants, 0);
                    cmdList->SetComputeRootDescriptorTable(1, srvH);
                    cmdList->SetComputeRootDescriptorTable(2, uavH);
                    cmdList->Dispatch(dispatchCount, dispatchCount, 1);
                }

                // H→V 間バリア: vsmMap→UAV, vsmMapTemp→NSPS
                {
                    D3D12_RESOURCE_BARRIER hvBarriers[2] = {
                        CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMapManager.GetVsmMapTemp().Get(),
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                        CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMapManager.GetVsmMap().Get(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    };
                    cmdList->ResourceBarrier(2, hvBarriers);
                }

                // V-blur: vsmMapTemp → vsmMap
                cmdList->SetPipelineState(pipelineStateCache.GetShadowVsmBlurVPipelineState().Get());
                for (uint32_t ci = 0; ci < shadowContext.cascadeCount; ++ci) {
                    uint32_t constants[4] = { shadowMapSize, shadowMapSize, ci, 0u };
                    cmdList->SetComputeRoot32BitConstants(0, 4, constants, 0);
                    cmdList->SetComputeRootDescriptorTable(1, srvV);
                    cmdList->SetComputeRootDescriptorTable(2, uavV);
                    cmdList->Dispatch(dispatchCount, dispatchCount, 1);
                }

                // 後始末バリア: vsmMap→PSR, vsmMapTemp→UAV
                {
                    D3D12_RESOURCE_BARRIER finalBarriers[2] = {
                        CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMapManager.GetVsmMap().Get(),
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                        CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMapManager.GetVsmMapTemp().Get(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    };
                    cmdList->ResourceBarrier(2, finalBarriers);
                }

                // メインの SRV ヒープに戻す
                DescriptorHeap* mainHeaps[] = { &srvHeap };
                cmdList->SetDescriptorHeaps(1, mainHeaps);
            }
        }

        // --- ステップ7: スポットライト #0 のシャドウパス ---
        if (!m_spotLights.empty() && m_shadowMapManager.EnsureSpotShadowResources())
        {
            const auto& sl0 = m_spotLights[0];
            const float spotPos[3] = { sl0.pos[0], sl0.pos[1], sl0.pos[2] };
            ShadowPassContext spotContext{};
            Math::BuildSpotLightViewProjection(sl0.yaw, sl0.pitch, spotPos,
                                               sl0.outerAngle, 0.1f, sl0.range,
                                               spotContext.lightViewProjection);

            auto spotBarrier = CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMapManager.GetSpotShadowMap().Get(),
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_DEPTH_WRITE);
            cmdList->ResourceBarrier(1, &spotBarrier);

            const UINT spotShadowMapSize = m_shadowMapManager.GetSpotShadowMapSize();
            Viewport spotVp = { 0.0f, 0.0f,
                static_cast<float>(spotShadowMapSize),
                static_cast<float>(spotShadowMapSize),
                0.0f, 1.0f };
            Rect spotScissor = { 0, 0,
                static_cast<LONG>(spotShadowMapSize),
                static_cast<LONG>(spotShadowMapSize) };

            cmdList->RSSetViewports(1, &spotVp);
            cmdList->RSSetScissorRects(1, &spotScissor);

            auto spotDsv = m_shadowMapManager.GetSpotDsv();
            cmdList->OMSetRenderTargets(0, nullptr, FALSE, &spotDsv);
            cmdList->ClearDepthStencilView(spotDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            if (drawCallback) {
                drawCallback(spotContext);
            }

            spotBarrier = CD3DX12_RESOURCE_BARRIER::Transition(m_shadowMapManager.GetSpotShadowMap().Get(),
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList->ResourceBarrier(1, &spotBarrier);
        }
    }

    // =========================================================================
    // GetDirectionalLightSettings / SetDirectionalLightSettings
    // ディレクショナルライトのパラメータを一括取得・設定する。
    // ImGui UI から呼ばれる。
    // =========================================================================

} // namespace SasamiRenderer
