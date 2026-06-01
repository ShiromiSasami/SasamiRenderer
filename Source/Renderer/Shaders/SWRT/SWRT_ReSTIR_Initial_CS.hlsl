//
// SWRT_ReSTIR_Initial_CS.hlsl
// Pass 1 of the ReSTIR reflection DI pipeline.
//
// Each thread processes one reflection pixel:
//   1. Read the rasterized primary surface from the GBuffer.
//   2. Trace the first reflection ray and store the secondary hit surface.
//   3. Sample M=8 candidate lights for that secondary hit.
//

#include "SWRT/SWRT_Common.hlsli"
#include "SWRT/SWRT_Reservoir.hlsli"

cbuffer ReSTIRFrameConstants : register(b0)
{
    row_major float4x4 g_invVP;
    row_major float4x4 g_prevVP;
    float3 g_cameraPos;
    float  g_tMin;
    uint   g_renderWidth;
    uint   g_renderHeight;
    uint   g_frameIndex;
    uint   g_reservoirWidth;
    float  g_temporalAlpha;
    float  g_phiColor;
    float  g_phiNormal;
    float  g_phiDepth;
    float  g_stepWidth;
    float  g_maxSurfaceRoughness;
    float  g_maxPrimaryHitDistance; // ReSTIR reflection path uses this as max reflection distance.
    float  g_minReflectionEnergy;
    float3 g_dirLightDir;
    float  g_dirLightIntensity;
    float3 g_dirLightColor;
    float  g_shadowBias;
    float3 g_ambientColor;
    float  g_ambientIntensity;
    uint   g_pointLightCount;
    uint   g_spotLightCount;
    uint   g_gbufferWidth;
    uint   g_gbufferHeight;
};

Texture2D<float4> g_rasterNormal   : register(t6); // xyz=N*0.5+0.5, w=camera distance
Texture2D<float4> g_rasterMaterial : register(t7); // r=roughness, g=metallic, a=reflection strength
Texture2D<float4> g_rasterAlbedo   : register(t8);

struct GpuPointLightRT { float3 pos; float range; float3 colorIntensity; float pad; };
struct GpuSpotLightRT  { float3 pos; float range; float3 dir; float cosInner;
                         float3 colorIntensity; float cosOuter; };

StructuredBuffer<GpuPointLightRT> g_pointLights : register(t12);
StructuredBuffer<GpuSpotLightRT>  g_spotLights  : register(t13);

RWTexture2D<float4>           g_gbufferOut     : register(u0); // secondary normal.xyz + reflection hit distance
RWTexture2D<float4>           g_hitPositionOut : register(u1); // secondary worldPos.xyz + (1 + metallic), 0 = invalid
RWTexture2D<float4>           g_hitMaterialOut : register(u2); // secondary baseColor.rgb + roughness
RWStructuredBuffer<Reservoir> g_reservoirOut   : register(u3);

float3 ComputeCameraRayDir(uint2 pixel, uint2 sourceSize)
{
    float ndcX =  ((float(pixel.x) + 0.5f) / float(sourceSize.x)) * 2.0f - 1.0f;
    float ndcY = -((float(pixel.y) + 0.5f) / float(sourceSize.y)) * 2.0f + 1.0f;
    float4 dir = mul(float4(ndcX, ndcY, 1.0f, 1.0f), g_invVP);
    return normalize(dir.xyz / dir.w - g_cameraPos);
}

float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10f;
}

float3 TangentToWorld(float3 v, float3 N)
{
    float3 up = (abs(N.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 T = normalize(cross(up, N));
    float3 B = cross(N, T);
    return normalize(T * v.x + B * v.y + N * v.z);
}

float3 SampleGGX_H(float2 xi, float roughness)
{
    float a = max(roughness * roughness, 0.0001f);
    float phi = 6.28318530718f * xi.x;
    float cosTheta = sqrt((1.0f - xi.y) / max(1.0f + (a * a - 1.0f) * xi.y, 1e-5f));
    float sinTheta = sqrt(max(1.0f - cosTheta * cosTheta, 0.0f));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

uint TotalLightCount() { return g_pointLightCount + g_spotLightCount; }

float EvalPhat(uint i, float3 pos, float3 N)
{
    if (i < g_pointLightCount)
    {
        GpuPointLightRT pl = g_pointLights[i];
        return PhatPoint(pos, N, pl.pos, pl.colorIntensity, pl.range);
    }
    uint si = i - g_pointLightCount;
    GpuSpotLightRT sl = g_spotLights[si];
    float3 toLight = sl.pos - pos;
    float  dist    = length(toLight);
    if (dist >= sl.range) return 0.0f;
    float3 L = toLight / dist;
    float  cosA = dot(-L, normalize(sl.dir));
    if (cosA < sl.cosOuter) return 0.0f;
    float spotAtten = smoothstep(sl.cosOuter, sl.cosInner, cosA);
    return PhatPoint(pos, N, sl.pos, sl.colorIntensity * spotAtten, sl.range);
}

void StoreInvalid(uint2 pixel, uint pixIdx)
{
    g_gbufferOut[pixel]     = float4(0.0f, 0.0f, 0.0f, -1.0f);
    g_hitPositionOut[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    g_hitMaterialOut[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
    g_reservoirOut[pixIdx]  = InitReservoir();
}

[numthreads(16, 16, 1)]
void CS_ReSTIR_Initial(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_renderWidth || id.y >= g_renderHeight) return;

    uint2 pixel = id.xy;
    uint2 gbufSize = uint2(max(g_gbufferWidth, 1u), max(g_gbufferHeight, 1u));
    uint2 gbufPixel = uint2(
        min(uint(float(pixel.x) * float(gbufSize.x) / float(g_renderWidth)),  gbufSize.x - 1u),
        min(uint(float(pixel.y) * float(gbufSize.y) / float(g_renderHeight)), gbufSize.y - 1u));
    uint pixIdx = id.y * g_reservoirWidth + id.x;

    float4 rasterNormal = g_rasterNormal.Load(int3(gbufPixel, 0));
    float primaryDepth = rasterNormal.w;
    if (primaryDepth <= 0.0f)
    {
        StoreInvalid(pixel, pixIdx);
        return;
    }

    if (g_tlasNodes[0].leftChild == 0 && g_tlasNodes[0].rightOrCount == 0)
    {
        StoreInvalid(pixel, pixIdx);
        return;
    }

    float3 primaryN = normalize(rasterNormal.xyz * 2.0f - 1.0f);
    float3 primaryRayDir = ComputeCameraRayDir(gbufPixel, gbufSize);
    float3 primaryPos = g_cameraPos + primaryRayDir * primaryDepth;
    float3 V = normalize(-primaryRayDir);
    if (dot(primaryN, V) < 0.0f) primaryN = -primaryN;

    float4 primaryMat = g_rasterMaterial.Load(int3(gbufPixel, 0));
    float roughness = primaryMat.r;
    float reflectionStrength = saturate(primaryMat.a);
    float roughnessFade = 1.0f - smoothstep(g_maxSurfaceRoughness * 0.7f,
                                            g_maxSurfaceRoughness,
                                            roughness);
    float reflectionEnergy = roughnessFade * reflectionStrength;
    if (reflectionEnergy < g_minReflectionEnergy)
    {
        StoreInvalid(pixel, pixIdx);
        return;
    }

    float3 reflDir = reflect(primaryRayDir, primaryN);
    if (roughness >= 0.01f)
    {
        uint rngState = ReSTIRSeed(pixel, g_frameIndex, 11u);
        float2 xi = float2(Rand01(rngState), RadicalInverseVdC(rngState));
        float3 H = TangentToWorld(SampleGGX_H(xi, roughness), primaryN);
        reflDir = reflect(primaryRayDir, H);
        if (dot(reflDir, primaryN) <= 0.0f)
        {
            StoreInvalid(pixel, pixIdx);
            return;
        }
    }

    float3 secondaryOrigin = OffsetRay(primaryPos, primaryN);
    float3 secondaryDir = reflDir;
    float remainingDistance = g_maxPrimaryHitDistance;
    HitResult secondary;
    secondary.hit = false;
    secondary.t = 0.0f;
    secondary.u = 0.0f;
    secondary.v = 0.0f;
    secondary.instanceIndex = 0u;
    secondary.triLocalIndex = 0u;
    float transparentThroughput = 1.0f;
    float transparentTravel = 0.0f;
    float3 transparentTint = float3(1.0f, 1.0f, 1.0f);
    float3 transparentSurfaceColor = float3(0.0f, 0.0f, 0.0f);

    [loop]
    for (uint transparentLayer = 0u; transparentLayer < 4u; ++transparentLayer)
    {
        HitResult candidate = TraceClosestHit(
            secondaryOrigin, secondaryDir, g_tMin, remainingDistance);
        if (!candidate.hit)
            break;

        GpuInstanceInfo candidateInst = g_instances[candidate.instanceIndex];
        GpuMaterial candidateMat = g_materials[candidateInst.materialIndex];
        if (!SWRT_IsTransparentMaterial(candidateMat))
        {
            secondary = candidate;
            break;
        }

        float surfaceOpacity = SWRT_MaterialSurfaceOpacity(candidateMat);
        transparentSurfaceColor += transparentTint * saturate(candidateMat.baseColor.rgb) * surfaceOpacity;
        transparentTint *= SWRT_MaterialTransmittanceTint(candidateMat) * (1.0f - surfaceOpacity);
        transparentThroughput *= (1.0f - surfaceOpacity);
        if (transparentThroughput < 0.02f)
        {
            secondary = candidate;
            break;
        }

        float3 candidatePos = secondaryOrigin + secondaryDir * candidate.t;
        float3 candidateN = GetWorldNormal(candidate);
        if (dot(candidateN, -secondaryDir) < 0.0f)
            candidateN = -candidateN;

        float3 transmittedDir = refract(secondaryDir, candidateN, 1.0f / max(candidateMat.ior, 1.0f));
        if (dot(transmittedDir, transmittedDir) <= 1e-6f)
            transmittedDir = secondaryDir;
        transmittedDir = normalize(transmittedDir);

        transparentTravel += candidate.t;
        remainingDistance = max(remainingDistance - candidate.t, 0.0f);
        secondaryOrigin = candidatePos + transmittedDir * max(g_tMin, 0.01f);
        secondaryDir = transmittedDir;
    }

    if (!secondary.hit)
    {
        // Store reflDir in xyz so the shade pass can sample sky color on miss.
        // w = -2.0f distinguishes "miss with valid reflDir" from w = -1.0f (fully invalid).
        g_gbufferOut[pixel]     = float4(secondaryDir, -2.0f);
        g_hitPositionOut[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        g_hitMaterialOut[pixel] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        g_reservoirOut[pixIdx]  = InitReservoir();
        return;
    }

    float3 hitPos = secondaryOrigin + secondaryDir * secondary.t;
    float hitDistance = transparentTravel + secondary.t;
    float3 hitN = GetWorldNormal(secondary);
    if (dot(hitN, -secondaryDir) < 0.0f) hitN = -hitN;

    GpuInstanceInfo inst = g_instances[secondary.instanceIndex];
    GpuMaterial mat = g_materials[inst.materialIndex];
    float3 baseColor = saturate(transparentSurfaceColor + transparentTint * saturate(mat.baseColor.rgb));
    float hitRoughness = saturate(mat.roughness);
    float hitMetallic = saturate(mat.metallic);

    g_gbufferOut[pixel]     = float4(hitN, hitDistance);
    g_hitPositionOut[pixel] = float4(hitPos, 1.0f + hitMetallic);
    g_hitMaterialOut[pixel] = float4(baseColor, hitRoughness);

    uint totalLights = TotalLightCount();
    Reservoir r = InitReservoir();
    if (totalLights == 0u)
    {
        g_reservoirOut[pixIdx] = r;
        return;
    }

    static const uint kM = 8u;
    uint rngState = ReSTIRSeed(pixel, g_frameIndex, 0u);
    for (uint s = 0; s < kM; ++s)
    {
        uint lightIdx = (uint)(Rand01(rngState) * float(totalLights));
        lightIdx = min(lightIdx, totalLights - 1u);
        float w = EvalPhat(lightIdx, hitPos, hitN) * float(totalLights);
        UpdateReservoir(r, lightIdx, w, Rand01(rngState));
    }

    float p_hat_y = (r.lightIndex != 0xFFFFFFFFu)
                  ? EvalPhat(r.lightIndex, hitPos, hitN)
                  : 0.0f;
    FinalizeReservoir(r, p_hat_y);
    g_reservoirOut[pixIdx] = r;
}
