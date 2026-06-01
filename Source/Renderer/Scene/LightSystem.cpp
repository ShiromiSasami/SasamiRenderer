// =============================================================================
// LightSystem.cpp
// ライトデータの管理と GPU バッファへのアップロード実装。
//
// 【データフロー】
//   CPU (m_pointLights etc.)
//     → EnsureLightBuffers() でアップロードバッファを確保・拡張
//     → UpdateFrameLighting() でライトデータを書き込み
//     → ExecuteShadowPass() でシャドウマップを描画
//     → LightingRenderNode が SRV を参照してライティングを実行
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
    // =========================================================================
    // Initialize
    // アプリ起動時に 1 回だけ呼ぶ。
    // シャドウマップ SRV のディスクリプタスロットを確保する。
    // まだテクスチャは作らない（EnsureShadowResources() で遅延生成）。
    // =========================================================================
    bool LightSystem::Initialize(IRHIDevice& device, const AllocateSrvRangeCallback& allocateSrvRange)
    {
        ScopedPerfTimer perfTimer("LightSystem::Initialize");
        m_device = &device;

        if (!m_shadowMapManager.Initialize(device, allocateSrvRange)) {
            return false;
        }

        return true;
    }

    // =========================================================================
    // InitializeFrameResources
    // フレームバッファリング数分（通常 2）だけ呼ぶ。
    // 各フレームの定数バッファと StructuredBuffer 用アップロードバッファを確保する。
    // =========================================================================
    bool LightSystem::InitializeFrameResources(FrameResources& frame, const AllocateSrvRangeCallback& allocateSrvRange)
    {
        if (!m_device || !allocateSrvRange) {
            return false;
        }

        // --- 定数バッファ（LightCBData）確保 ---
        // D3D12 の定数バッファは 256 バイトアラインが必須。
        const UINT lightCbSize = (sizeof(LightCBLayout::LightCBData) + 255u) & ~255u;
        if (!ResourceUploadUtility::CreateUploadBuffer(*m_device, lightCbSize, frame.lightCB, &frame.lightCBPtr)) {
            DebugLogDialog("LightSystem::InitializeFrameResources: Light CB creation failed.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        // --- ポイントライト・スポットライト SRV スロットを 2 つ確保 ---
        // t6 = ポイントライト StructuredBuffer, t7 = スポットライト StructuredBuffer
        const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        CpuDescriptorHandle lightSrvCpu{};
        GpuDescriptorHandle lightSrvGpu{};
        if (!allocateSrvRange(2, lightSrvCpu, lightSrvGpu)) {
            DebugLogDialog("LightSystem::InitializeFrameResources: SRV allocation failed for light buffers.\n", L"SasamiRenderer Initialize Error", MB_OK | MB_ICONERROR);
            return false;
        }

        frame.pointSrvCpu  = lightSrvCpu;
        frame.spotSrvCpu   = { lightSrvCpu.ptr + static_cast<SIZE_T>(inc) }; // 隣のスロット
        frame.lightSrvTable = lightSrvGpu; // ディスクリプタテーブルの先頭
        return true;
    }

    // =========================================================================
    // ShutdownFrameResources
    // フレームリソースを解放する。アプリ終了時またはリサイズ時に呼ぶ。
    // アップロードバッファは永続 Map しているので Unmap してから解放する。
    // =========================================================================
    void LightSystem::ShutdownFrameResources(FrameResources& frame)
    {
        if (frame.lightCB.IsValid() && frame.lightCBPtr) {
            frame.lightCB->Unmap(0, nullptr);
            frame.lightCBPtr = nullptr;
        }

        if (frame.pointLightBuffer.IsValid() && frame.pointLightBufferPtr) {
            frame.pointLightBuffer->Unmap(0, nullptr);
            frame.pointLightBufferPtr = nullptr;
        }

        if (frame.spotLightBuffer.IsValid() && frame.spotLightBufferPtr) {
            frame.spotLightBuffer->Unmap(0, nullptr);
            frame.spotLightBufferPtr = nullptr;
        }

        frame.lightCB.Reset();
        frame.pointLightBuffer.Reset();
        frame.spotLightBuffer.Reset();
        frame.pointLightCapacity = 0;
        frame.spotLightCapacity  = 0;
        frame.pointSrvCpu  = {};
        frame.spotSrvCpu   = {};
        frame.lightSrvTable = {};
    }

    // =========================================================================
    // EnsureLightBuffers（プライベート）
    // ポイントライト・スポットライトの StructuredBuffer を必要なサイズに拡張する。
    // 現在の容量が足りない場合のみ再確保する（2 倍成長で再確保回数を抑える）。
    // =========================================================================
    void LightSystem::EnsureLightBuffers(FrameResources& frame, size_t pointCount, size_t spotCount)
    {
        if (!m_device) {
            return;
        }

        // 現在の容量から必要な容量まで 2 倍ずつ成長させる
        auto growCapacity = [](UINT current, UINT required) -> UINT {
            UINT cap = (current > 0) ? current : 1u;
            while (cap < required) {
                cap *= 2u;
            }
            return cap;
        };

        // 空でも最低 1 要素は確保する（SRV 作成に 0 要素は不可）
        const UINT requiredPoint = (pointCount > 0) ? static_cast<UINT>(pointCount) : 1u;
        const UINT requiredSpot  = (spotCount  > 0) ? static_cast<UINT>(spotCount)  : 1u;

        // --- ポイントライトバッファ拡張 ---
        if (requiredPoint > frame.pointLightCapacity) {
            frame.pointLightCapacity = growCapacity(frame.pointLightCapacity, requiredPoint);
            if (frame.pointLightBuffer.IsValid() && frame.pointLightBufferPtr) {
                frame.pointLightBuffer->Unmap(0, nullptr);
            }
            frame.pointLightBuffer.Reset();
            frame.pointLightBufferPtr = nullptr;

            const UINT64 byteSize = static_cast<UINT64>(frame.pointLightCapacity) * sizeof(PointLightGPU);
            if (!ResourceUploadUtility::CreateUploadBuffer(*m_device, byteSize, frame.pointLightBuffer, &frame.pointLightBufferPtr)) {
                frame.pointLightCapacity = 0;
            }
        }

        // --- スポットライトバッファ拡張 ---
        if (requiredSpot > frame.spotLightCapacity) {
            frame.spotLightCapacity = growCapacity(frame.spotLightCapacity, requiredSpot);
            if (frame.spotLightBuffer.IsValid() && frame.spotLightBufferPtr) {
                frame.spotLightBuffer->Unmap(0, nullptr);
            }
            frame.spotLightBuffer.Reset();
            frame.spotLightBufferPtr = nullptr;

            const UINT64 byteSize = static_cast<UINT64>(frame.spotLightCapacity) * sizeof(SpotLightGPU);
            if (!ResourceUploadUtility::CreateUploadBuffer(*m_device, byteSize, frame.spotLightBuffer, &frame.spotLightBufferPtr)) {
                frame.spotLightCapacity = 0;
            }
        }

        // --- ポイントライト SRV を作成（バッファが有効な場合）---
        if (frame.pointLightBuffer.IsValid()) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_UNKNOWN; // 構造化バッファは UNKNOWN
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = frame.pointLightCapacity;
            srvDesc.Buffer.StructureByteStride = sizeof(PointLightGPU);
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            m_device->CreateShaderResourceView(frame.pointLightBuffer, &srvDesc, frame.pointSrvCpu);
        }

        // --- スポットライト SRV を作成 ---
        if (frame.spotLightBuffer.IsValid()) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = frame.spotLightCapacity;
            srvDesc.Buffer.StructureByteStride = sizeof(SpotLightGPU);
            srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            m_device->CreateShaderResourceView(frame.spotLightBuffer, &srvDesc, frame.spotSrvCpu);
        }
    }

    // =========================================================================
    // BuildShadowPassContext
    // ライトの yaw/pitch/distance などからシャドウカメラの VP 行列を計算する。
    // ExecuteShadowPass() の内部でも呼ばれるが、外部から VP 行列だけ取得したい場合にも使える。
    // =========================================================================
    // Shadow pass execution → LightSystem_Shadow.cpp

    LightSystem::DirectionalLightSettings LightSystem::GetDirectionalLightSettings() const
    {
        DirectionalLightSettings settings{};
        settings.yaw       = m_lightYaw;
        settings.pitch     = m_lightPitch;
        settings.distance  = m_lightDistance;
        settings.orthoHalf = m_lightOrthoHalf;
        settings.nearZ     = m_lightNear;
        settings.farZ      = m_lightFar;
        settings.color[0]  = m_dirColor[0];
        settings.color[1]  = m_dirColor[1];
        settings.color[2]  = m_dirColor[2];
        settings.intensity = m_dirIntensity;
        settings.shadowMode = m_shadowMode;
        settings.shadowDistance = m_shadowDistance;
        settings.cascadeDistributionExponent = m_cascadeDistributionExponent;
        settings.cascadeBlendFraction = m_cascadeBlendFraction;
        settings.depthRangeMode = m_depthRangeMode;
        settings.depthBias = m_shadowDepthBias;
        settings.slopeScaleBias = m_shadowSlopeScaleBias;
        settings.normalBias = m_shadowNormalBias;
        settings.farBiasScale = m_shadowFarBiasScale;
        return settings;
    }

    void LightSystem::SetDirectionalLightSettings(const DirectionalLightSettings& settings)
    {
        m_lightYaw       = settings.yaw;
        m_lightPitch     = settings.pitch;
        m_lightDistance  = settings.distance;
        m_lightOrthoHalf = settings.orthoHalf;
        m_lightNear      = settings.nearZ;
        m_lightFar       = settings.farZ;
        m_dirColor[0]    = settings.color[0];
        m_dirColor[1]    = settings.color[1];
        m_dirColor[2]    = settings.color[2];
        m_dirIntensity   = settings.intensity;
        m_shadowMode = settings.shadowMode;
        m_shadowDistance = (std::max)(1.0f, settings.shadowDistance);
        m_cascadeDistributionExponent = (std::max)(1.0f, settings.cascadeDistributionExponent);
        m_cascadeBlendFraction = Clamp01(settings.cascadeBlendFraction);
        m_depthRangeMode = settings.depthRangeMode;
        m_shadowDepthBias = settings.depthBias;
        m_shadowSlopeScaleBias = settings.slopeScaleBias;
        m_shadowNormalBias = (std::max)(0.0f, settings.normalBias);
        m_shadowFarBiasScale = (std::max)(1.0f, settings.farBiasScale);
    }
}
