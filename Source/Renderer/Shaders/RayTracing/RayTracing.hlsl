// =============================================================================
// RayTracing.hlsl
// DXR（DirectX Raytracing）ハードウェアレイトレーシング シェーダーライブラリ。
//
// 【全体構成】
//  - RayGenShader:      カメラレイを生成し最初の TraceRay を発行する（1 ピクセル 1 スレッド）
//  - ClosestHitShader:  ヒット点のマテリアル評価・ライティング・反射レイを処理する
//  - MissShader:        レイが何にも当たらなかったとき（スカイカラーを返す）
//  - ShadowMissShader:  シャドウレイが何にも当たらなかったとき（遮蔽なし）
//
// 【NEE（Next Event Estimation）】
//  各ライトに対してシャドウレイを飛ばして可視性を確認してからライティングを評価する。
//  可視性テストなしに直接ライトを評価すると壁の裏まで照らされる誤りが起きる。
//
// 【ディスクリプタ】
//  t0 (TLAS): SceneBVH（最上位加速構造）
//  b0 (CBV):  gFrame（FrameConstants、フレームごとの全パラメータ）
//  Resource Descriptor Heap 経由でバーテックス・インデックス・マテリアル・インスタンスバッファを参照
//
// 【品質モード】
//  gFrame.qualityTier == 0: フル PBR（GGX スペキュラー + 拡散）
//  gFrame.qualityTier != 0: 簡易 Lambert（高速・デバッグ用）
// =============================================================================

struct VertexData
{
    float3 position;
    float3 normal;
    float4 color;
    float2 uv;
};

struct MaterialData
{
    int albedoDescriptorIndex;
    int occlusionDescriptorIndex;
    float metallic;
    float roughness;
    float4 baseColor;
    float4 emissiveOcclusionStrength;
    float4 transmissionParams; // x: transmission, y: ior, z: shell strength, w: thickness
    float4 volumeParams;       // rgb: attenuation color, w: attenuation distance
};

struct InstanceData
{
    uint vertexOffset;
    uint indexOffset;
    uint materialIndex;
    uint padding;
};

struct PointLightData
{
    float4 posRange;
    float4 colorIntensity;
};

struct SpotLightData
{
    float4 posRange;
    float4 dirCosInner;
    float4 colorIntensity;
    float4 params;
};

static const uint MAX_POINT_LIGHTS = 16;
static const uint MAX_SPOT_LIGHTS = 16;
static const uint INSTANCE_MASK_RADIANCE = 0xFFu;
static const uint INSTANCE_MASK_OPAQUE_SHADOW = 0xFEu;

struct FrameConstants
{
    uint renderWidth;
    uint renderHeight;
    uint outputWidth;
    uint outputHeight;
    uint outputDescriptorIndex;
    uint vertexDescriptorIndex;
    uint indexDescriptorIndex;
    uint materialDescriptorIndex;
    uint instanceDescriptorIndex;
    uint pointLightCount;
    uint spotLightCount;
    uint pointLightBudget;
    uint spotLightBudget;
    uint qualityTier;
    uint debugView;
    uint flags;
    uint maxBounceCount;
    float dynamicResolutionScale;
    float2 padding;
    float4 cameraPosition;
    row_major float4x4 inverseViewProjection;
    float4 directionalLightDirection;
    float4 directionalLightColorIntensity;
    float4 directionalLightMarkerParams;
    PointLightData pointLights[MAX_POINT_LIGHTS];
    SpotLightData spotLights[MAX_SPOT_LIGHTS];
};

struct RadiancePayload
{
    float3 color;
    uint hit;
    uint bounceIndex;
};

struct ShadowPayload
{
    uint occluded;
};

struct Attributes
{
    float2 barycentrics : SV_Barycentrics;
};

RaytracingAccelerationStructure SceneBVH : register(t0);
ConstantBuffer<FrameConstants> gFrame : register(b0);
SamplerState LinearWrapSampler : register(s0);

float Saturate(float value)
{
    return clamp(value, 0.0, 1.0);
}

float3 SafeNormalize(float3 value)
{
    float lenSq = dot(value, value);
    if (lenSq <= 1e-8) {
        return float3(0.0, 0.0, 1.0);
    }
    return value * rsqrt(lenSq);
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

float DistributionGGX(float NdotH, float a)
{
    float a2 = a * a;
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * d * d, 1e-4);
}

float GeometrySchlickGGX(float NdotV, float k)
{
    return NdotV / max(NdotV * (1.0 - k) + k, 1e-4);
}

float GeometrySmith(float NdotV, float NdotL, float k)
{
    return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}

float2 WrapUv(float2 uv)
{
    return frac(uv);
}

float4 SampleBaseColor(MaterialData material, float2 uv, float4 fallbackColor)
{
    const float4 baseColor = fallbackColor * material.baseColor;
    if (material.albedoDescriptorIndex < 0) {
        return baseColor;
    }

    Texture2D<float4> texture = ResourceDescriptorHeap[NonUniformResourceIndex(material.albedoDescriptorIndex)];
    return texture.SampleLevel(LinearWrapSampler, WrapUv(uv), 0) * baseColor;
}

float SampleAmbientOcclusion(MaterialData material, float2 uv)
{
    float ao = 1.0;
    if (material.occlusionDescriptorIndex < 0) {
        return ao;
    }

    Texture2D<float4> texture = ResourceDescriptorHeap[NonUniformResourceIndex(material.occlusionDescriptorIndex)];
    ao = texture.SampleLevel(LinearWrapSampler, WrapUv(uv), 0).r;
    return lerp(1.0, ao, saturate(material.emissiveOcclusionStrength.w));
}

float3 SkyColor(float3 rayDirection)
{
    float t = Saturate(rayDirection.y * 0.5 + 0.5);
    return lerp(float3(0.10, 0.12, 0.18), float3(0.50, 0.62, 0.90), t);
}

float3 ApplyDirectionalLightMarker(float3 color, float3 sampleDir)
{
    if (gFrame.directionalLightMarkerParams.x <= 0.5 || gFrame.directionalLightColorIntensity.a <= 0.0) {
        return color;
    }

    const float3 lightDir = SafeNormalize(gFrame.directionalLightDirection.xyz);
    const float cosTheta = Saturate(dot(SafeNormalize(sampleDir), lightDir));
    const float discMask = smoothstep(cos(gFrame.directionalLightMarkerParams.y), 1.0, cosTheta);
    const float haloMask = smoothstep(cos(gFrame.directionalLightMarkerParams.z), 1.0, cosTheta);
    const float3 markerColor =
        lerp(saturate(gFrame.directionalLightColorIntensity.rgb), float3(1.0, 1.0, 1.0), 0.35);

    return color + markerColor * ((haloMask * 0.5) + (discMask * 6.0)) * gFrame.directionalLightMarkerParams.w;
}

float3 EvaluateBrdf(float3 normal,
                    float3 view,
                    float3 lightDir,
                    float3 lightColor,
                    float3 albedo,
                    float roughness,
                    float metallic)
{
    float a = roughness * roughness;
    float k = ((roughness + 1.0) * (roughness + 1.0)) / 8.0;
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float3 halfVector = SafeNormalize(view + lightDir);
    float NdotV = Saturate(dot(normal, view));
    float NdotL = Saturate(dot(normal, lightDir));
    if (NdotL <= 0.0) {
        return 0.0.xxx;
    }
    if (gFrame.qualityTier != 0u) {
        return albedo / 3.14159265 * NdotL * lightColor;
    }

    float NdotH = Saturate(dot(normal, halfVector));
    float VdotH = Saturate(dot(view, halfVector));
    float D = DistributionGGX(NdotH, a);
    float G = GeometrySmith(NdotV, NdotL, k);
    float3 F = FresnelSchlick(VdotH, F0);
    float3 specular = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-4);
    float3 kd = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kd * albedo / 3.14159265;
    return (diffuse + specular) * NdotL * lightColor;
}

// 輝度レイ（カメラレイ・反射レイ）を発行するヘルパー。
// bounceIndex を渡すことでバウンス回数を ClosestHitShader が追跡できる。
RadiancePayload TraceRadianceRay(RayDesc ray, uint bounceIndex)
{
    RadiancePayload payload;
    payload.color = 0.0.xxx;
    payload.hit = 0u;
    payload.bounceIndex = bounceIndex;

    TraceRay(SceneBVH,
             RAY_FLAG_FORCE_OPAQUE,
             INSTANCE_MASK_RADIANCE,
             0,
             1,
             0,
             ray,
             payload);
    return payload;
}

// ミスシェーダー: 輝度レイが何にも当たらなかった場合に呼ばれる。
// 空の色（グラジエント）を返し、太陽ディスクのマーカーも描画する。
[shader("miss")]
void MissShader(inout RadiancePayload payload)
{
    payload.color = ApplyDirectionalLightMarker(SkyColor(WorldRayDirection()), WorldRayDirection());
    payload.hit = 0u;
}

// シャドウミスシェーダー: シャドウレイが何にも当たらなかった場合に呼ばれる。
// occluded = 0 を返すことで「影なし（ライトが可視）」を示す。
[shader("miss")]
void ShadowMissShader(inout ShadowPayload payload)
{
    payload.occluded = 0u;
}

// 最近傍ヒットシェーダー: レイが最初にジオメトリに当たったときに呼ばれる。
// 【処理内容】
//  1. バリセントリック補間でワールド座標・法線・UV を計算
//  2. マテリアル（アルベド・粗さ・メタリック・エミッシブ）を評価
//  3. ディレクショナルライトを NEE シャドウレイ付きで評価
//  4. ポイントライト・スポットライトをそれぞれ NEE シャドウレイ付きで評価
//  5. bounceIndex < maxBounceCount なら反射レイを再帰的に発行
[shader("closesthit")]
void ClosestHitShader(inout RadiancePayload payload, in Attributes attributes)
{
    StructuredBuffer<VertexData> vertices = ResourceDescriptorHeap[gFrame.vertexDescriptorIndex];
    StructuredBuffer<uint> indices = ResourceDescriptorHeap[gFrame.indexDescriptorIndex];
    StructuredBuffer<MaterialData> materials = ResourceDescriptorHeap[gFrame.materialDescriptorIndex];
    StructuredBuffer<InstanceData> instances = ResourceDescriptorHeap[gFrame.instanceDescriptorIndex];

    const uint instanceIndex = InstanceID();
    const InstanceData instance = instances[instanceIndex];
    const uint baseIndex = instance.indexOffset + PrimitiveIndex() * 3;

    const uint index0 = indices[baseIndex + 0];
    const uint index1 = indices[baseIndex + 1];
    const uint index2 = indices[baseIndex + 2];

    const VertexData v0 = vertices[instance.vertexOffset + index0];
    const VertexData v1 = vertices[instance.vertexOffset + index1];
    const VertexData v2 = vertices[instance.vertexOffset + index2];

    const float baryU = attributes.barycentrics.x;
    const float baryV = attributes.barycentrics.y;
    const float baryW = 1.0 - baryU - baryV;

    const float2 uv = v0.uv * baryW + v1.uv * baryU + v2.uv * baryV;
    const float4 vertexColor = v0.color * baryW + v1.color * baryU + v2.color * baryV;
    const float3 localNormal = v0.normal * baryW + v1.normal * baryU + v2.normal * baryV;
    const float3 localGeometricNormal = cross(v1.position - v0.position, v2.position - v0.position);
    const float3 worldPosition = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    const float3 view = SafeNormalize(-WorldRayDirection());
    const float3x3 worldToObject = (float3x3)WorldToObject3x4();
    float3 geometricNormal = SafeNormalize(mul(localGeometricNormal, worldToObject));
    float3 normal = SafeNormalize(mul(localNormal, worldToObject));
    if (dot(geometricNormal, view) < 0.0) {
        geometricNormal = -geometricNormal;
    }
    if (dot(normal, view) < 0.0) {
        normal = -normal;
    }

    const MaterialData material = materials[instance.materialIndex];
    const float4 baseColor = SampleBaseColor(material, uv, vertexColor);
    const float3 albedo = baseColor.rgb;
    const float materialAlpha = Saturate(baseColor.a);
    const float ao = SampleAmbientOcclusion(material, uv);
    const float roughness = saturate(material.roughness);
    const float metallic = saturate(material.metallic);
    const float transmission = saturate(material.transmissionParams.x);
    const float materialIor = max(material.transmissionParams.y, 1.0);
    const float shellStrength = max(material.transmissionParams.z, 0.0);
    const float materialThickness = max(material.transmissionParams.w, 0.0);
    const float3 attenuationColor = saturate(material.volumeParams.rgb);
    const float attenuationDistance = max(material.volumeParams.w, 1e-4);
    const float3 emissive = material.emissiveOcclusionStrength.rgb;
    const float NdotV = Saturate(dot(normal, view));
    const float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    const float effectiveTransmission = transmission * (1.0 - metallic);
    float3 result = emissive;
    if (payload.bounceIndex <= 1u) {
        result += 0.08 * ao * albedo;
    }

    const float3 directionalLight = SafeNormalize(gFrame.directionalLightDirection.xyz);
    const float directionalFacingDot = dot(normal, directionalLight);
    float directionalVisibility = 1.0;
    if (directionalFacingDot > 0.0 && (gFrame.flags & 0x1u) != 0u) {
        ShadowPayload shadowPayload;
        shadowPayload.occluded = 1u;
        RayDesc shadowRay;
        const float3 shadowOffsetNormal = (dot(geometricNormal, directionalLight) >= 0.0)
            ? geometricNormal
            : -geometricNormal;
        shadowRay.Origin = worldPosition + shadowOffsetNormal * 0.01;
        shadowRay.Direction = directionalLight;
        shadowRay.TMin = 0.01;
        shadowRay.TMax = 1000.0;
        TraceRay(SceneBVH,
                 RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                 INSTANCE_MASK_OPAQUE_SHADOW,
                 0,
                 1,
                 1,
                 shadowRay,
                 shadowPayload);

        directionalVisibility = (shadowPayload.occluded == 0u) ? 1.0 : 0.0;
    }
    const float3 directionalColor = gFrame.directionalLightColorIntensity.rgb * gFrame.directionalLightColorIntensity.a;
    if (directionalFacingDot > 0.0) {
        result += EvaluateBrdf(normal, view, directionalLight, directionalColor, albedo, roughness, metallic) * directionalVisibility;
    } else {
        directionalVisibility = 0.0;
    }

    [loop]
    for (uint lightIndex = 0; lightIndex < min(gFrame.pointLightCount, gFrame.pointLightBudget); ++lightIndex) {
        const PointLightData light = gFrame.pointLights[lightIndex];
        const float3 toLight = light.posRange.xyz - worldPosition;
        const float distance = length(toLight);
        if (distance <= 1e-4 || distance >= light.posRange.w) {
            continue;
        }

        const float3 lightDir = SafeNormalize(toLight);
        if (dot(normal, lightDir) <= 0.0) {
            continue;
        }

        // NEE: ポイントライトへのシャドウレイ。
        // フラグ 0x1u が立っていれば（シャドウ有効）遮蔽テストを行い、
        // 遮蔽されていれば（occluded = 1）このライトへの寄与をスキップする。
        if ((gFrame.flags & 0x1u) != 0u) {
            ShadowPayload pointShadow;
            pointShadow.occluded = 1u;
            const float3 shadowOffsetN = (dot(geometricNormal, lightDir) >= 0.0)
                ? geometricNormal : -geometricNormal;
            RayDesc pointShadowRay;
            pointShadowRay.Origin    = worldPosition + shadowOffsetN * 0.01;
            pointShadowRay.Direction = lightDir;
            pointShadowRay.TMin      = 0.01;
            pointShadowRay.TMax      = distance - 0.01;
            TraceRay(SceneBVH,
                     RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                     INSTANCE_MASK_OPAQUE_SHADOW, 0, 1, 1,
                     pointShadowRay, pointShadow);
            if (pointShadow.occluded != 0u) continue;
        }

        const float attenuation = pow(Saturate(1.0 - distance / max(light.posRange.w, 1e-3)), 2.0);
        const float3 lightColor = light.colorIntensity.rgb * light.colorIntensity.a * attenuation;
        result += EvaluateBrdf(normal, view, lightDir, lightColor, albedo, roughness, metallic);
    }

    [loop]
    for (uint lightIndex = 0; lightIndex < min(gFrame.spotLightCount, gFrame.spotLightBudget); ++lightIndex) {
        const SpotLightData light = gFrame.spotLights[lightIndex];
        const float3 toLight = light.posRange.xyz - worldPosition;
        const float distance = length(toLight);
        if (distance <= 1e-4 || distance >= light.posRange.w) {
            continue;
        }

        const float3 lightDir = SafeNormalize(toLight);
        if (dot(normal, lightDir) <= 0.0) {
            continue;
        }
        const float cosTheta = dot(-lightDir, SafeNormalize(light.dirCosInner.xyz));
        const float spotFactor = Saturate((cosTheta - light.params.x) / max(light.dirCosInner.w - light.params.x, 1e-4));
        if (spotFactor <= 0.0) {
            continue;
        }

        // NEE: スポットライトへのシャドウレイ。ポイントライトと同じ手順。
        if ((gFrame.flags & 0x1u) != 0u) {
            ShadowPayload spotShadow;
            spotShadow.occluded = 1u;
            const float3 shadowOffsetN = (dot(geometricNormal, lightDir) >= 0.0)
                ? geometricNormal : -geometricNormal;
            RayDesc spotShadowRay;
            spotShadowRay.Origin    = worldPosition + shadowOffsetN * 0.01;
            spotShadowRay.Direction = lightDir;
            spotShadowRay.TMin      = 0.01;
            spotShadowRay.TMax      = distance - 0.01;
            TraceRay(SceneBVH,
                     RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                     INSTANCE_MASK_OPAQUE_SHADOW, 0, 1, 1,
                     spotShadowRay, spotShadow);
            if (spotShadow.occluded != 0u) continue;
        }

        const float attenuation = pow(Saturate(1.0 - distance / max(light.posRange.w, 1e-3)), 2.0) * spotFactor;
        const float3 lightColor = light.colorIntensity.rgb * light.colorIntensity.a * attenuation;
        result += EvaluateBrdf(normal, view, lightDir, lightColor, albedo, roughness, metallic);
    }

    const uint maxBounceCount = (gFrame.maxBounceCount > 0u) ? gFrame.maxBounceCount : 1u;
    if (gFrame.debugView == 0u &&
        payload.bounceIndex < maxBounceCount) {
        const float reflectionStrength = pow(1.0 - roughness, 2.0);
        const float3 reflectionF = FresnelSchlick(NdotV, F0);
        const float reflectionEnergy = reflectionStrength * max(reflectionF.r, max(reflectionF.g, reflectionF.b));
        if (reflectionEnergy > 1e-3) {
            RayDesc reflectionRay;
            const float3 reflectionDir = SafeNormalize(reflect(-view, normal));
            const float3 reflectionOffsetNormal = (dot(geometricNormal, reflectionDir) >= 0.0)
                ? geometricNormal
                : -geometricNormal;
            reflectionRay.Origin = worldPosition + reflectionOffsetNormal * 0.01;
            reflectionRay.Direction = reflectionDir;
            reflectionRay.TMin = 0.01;
            reflectionRay.TMax = 10000.0;

            const RadiancePayload reflectionPayload = TraceRadianceRay(reflectionRay, payload.bounceIndex + 1u);
            result += reflectionPayload.color * reflectionF * reflectionStrength;
        }
    }

    float3 outputColor = result;
    if (gFrame.debugView == 0u &&
        payload.bounceIndex < maxBounceCount &&
        (materialAlpha < 0.999 || effectiveTransmission > 0.001)) {
        const float3 viewFresnel = FresnelSchlick(NdotV, F0);
        const float fresnelStrength = Saturate(max(viewFresnel.r, max(viewFresnel.g, viewFresnel.b)));
        const float fresnelEdge = pow(1.0 - NdotV, 5.0);

        float3 transmissionDir = refract(WorldRayDirection(), normal, 1.0 / materialIor);
        if (dot(transmissionDir, transmissionDir) <= 1e-6) {
            transmissionDir = WorldRayDirection();
        }
        transmissionDir = SafeNormalize(transmissionDir);

        RayDesc transmissionRay;
        transmissionRay.Origin = worldPosition + transmissionDir * 0.03;
        transmissionRay.Direction = transmissionDir;
        transmissionRay.TMin = 0.01;
        transmissionRay.TMax = 10000.0;

        const RadiancePayload transmissionPayload =
            TraceRadianceRay(transmissionRay, payload.bounceIndex + 1u);
        float3 transmittedRadiance =
            transmissionPayload.color * lerp(1.0.xxx, albedo, effectiveTransmission * 0.35);
        if (materialThickness > 0.0) {
            transmittedRadiance *= pow(max(attenuationColor, float3(0.001, 0.001, 0.001)),
                                       materialThickness / attenuationDistance);
        }

        const float alphaTransparency = 1.0 - materialAlpha;
        const float opticalTransmission = effectiveTransmission * (1.0 - fresnelStrength);
        const float transmissionWeight = Saturate(max(alphaTransparency, opticalTransmission));
        const float shellAlpha = effectiveTransmission * shellStrength * (0.04 + 0.28 * fresnelEdge);

        outputColor = lerp(result, transmittedRadiance, transmissionWeight);
        outputColor += result * shellAlpha;
    }

    switch (gFrame.debugView) {
    case 1u:
        outputColor = saturate(albedo);
        break;
    case 2u:
        outputColor = normal * 0.5 + 0.5;
        break;
    case 3u:
        outputColor = roughness.xxx;
        break;
    case 4u:
        outputColor = metallic.xxx;
        break;
    case 5u:
        outputColor = ao.xxx;
        break;
    case 6u:
        outputColor = directionalVisibility.xxx;
        break;
    default:
        break;
    }

    payload.color = outputColor;
    payload.hit = 1u;
}

// レイ生成シェーダー: 各ピクセルに対してカメラレイを生成・発行する。
// 解像度スケールに応じた動的解像度をサポートし、
// 1 ピクセルが複数の出力ピクセルをカバーする場合は全てに同じ色を書き込む。
[shader("raygeneration")]
void RayGenShader()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= gFrame.renderWidth || pixel.y >= gFrame.renderHeight) {
        return;
    }

    const float2 uv = (float2(pixel) + 0.5) / float2(gFrame.renderWidth, gFrame.renderHeight);
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    const float4 nearPoint = mul(float4(ndc, 0.0, 1.0), gFrame.inverseViewProjection);
    const float4 farPoint = mul(float4(ndc, 1.0, 1.0), gFrame.inverseViewProjection);
    const float3 worldNear = nearPoint.xyz / max(nearPoint.w, 1e-6);
    const float3 worldFar = farPoint.xyz / max(farPoint.w, 1e-6);

    RayDesc ray;
    ray.Origin = gFrame.cameraPosition.xyz;
    ray.Direction = SafeNormalize(worldFar - worldNear);
    ray.TMin = 0.001;
    ray.TMax = 10000.0;

    RadiancePayload payload;
    payload.color = 0.0.xxx;
    payload.hit = 0u;
    payload.bounceIndex = 1u;

    TraceRay(SceneBVH,
             RAY_FLAG_FORCE_OPAQUE,
             INSTANCE_MASK_RADIANCE,
             0,
             1,
             0,
             ray,
             payload);

    float3 color = payload.color;
    if (gFrame.debugView == 0u) {
        color = color / (color + 1.0);
        color = pow(saturate(color), 1.0 / 2.2);
    } else {
        color = (payload.hit != 0u) ? saturate(color) : 0.0.xxx;
    }

    const uint2 outputBegin = uint2(
        (pixel.x * gFrame.outputWidth) / max(gFrame.renderWidth, 1u),
        (pixel.y * gFrame.outputHeight) / max(gFrame.renderHeight, 1u));
    const uint2 outputEnd = uint2(
        min(gFrame.outputWidth,
            ((pixel.x + 1u) * gFrame.outputWidth + max(gFrame.renderWidth, 1u) - 1u) / max(gFrame.renderWidth, 1u)),
        min(gFrame.outputHeight,
            ((pixel.y + 1u) * gFrame.outputHeight + max(gFrame.renderHeight, 1u) - 1u) / max(gFrame.renderHeight, 1u)));

    RWTexture2D<float4> output = ResourceDescriptorHeap[gFrame.outputDescriptorIndex];
    [loop]
    for (uint y = outputBegin.y; y < outputEnd.y; ++y) {
        [loop]
        for (uint x = outputBegin.x; x < outputEnd.x; ++x) {
            output[uint2(x, y)] = float4(color, 1.0);
        }
    }
}
