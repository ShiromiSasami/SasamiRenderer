//
// SWRT_Reflection_Temporal_CS.hlsl
// Simple temporal EMA (exponential moving average) accumulation for the
// SWRT Legacy reflection output.
//
// Layout:
//   t0 = current-frame raw reflection (output of SWRT_Reflection_CS)
//   t1 = previous EMA result (ping-pong history read side)
//   t2 = current-frame GBuffer normal/camera-distance
//   t3 = previous history surface metadata
//   t4 = current-frame GBuffer material
//   t5 = previous history material metadata
//   u0 = ping-pong history write side
//   u1 = ping-pong history surface metadata write side
//   u2 = ping-pong history material metadata write side
//
// After dispatch the caller CopyResource(u0 → reflOutput) so the downstream
// compositing shader reads the EMA-blended result.
//
// Alpha semantics:
//   alpha = 1.0  → use current frame only (camera moved, first frame)
//   alpha = 0.1  → 90% history + 10% current (camera stationary)
//

cbuffer BlendCB : register(b0)
{
    float g_alpha;   // blend weight for current frame
    uint  g_width;
    uint  g_height;
    uint  g_validationEnabled;
    uint  g_gbufferWidth;
    uint  g_gbufferHeight;
    uint  g_materialValidationEnabled;
};

cbuffer ReprojectionCB : register(b1)
{
    row_major float4x4 g_invVP;
    row_major float4x4 g_prevVP;
    float3 g_cameraPos;
    float  g_reprojectionPad0;
    float3 g_prevCameraPos;
    float  g_reprojectionPad1;
};

Texture2D<float4>   g_current  : register(t0);  // this frame's raw reflection
Texture2D<float4>   g_history  : register(t1);  // previous EMA result
Texture2D<float4>   g_currentSurface : register(t2); // encoded normal.xyz + camera distance
Texture2D<float4>   g_historySurface : register(t3); // previous encoded normal.xyz + camera distance
Texture2D<float4>   g_currentMaterial : register(t4); // roughness, metallic, AO, unused
Texture2D<float4>   g_historyMaterial : register(t5); // previous roughness, metallic, AO, unused

RWTexture2D<float4> g_outHist  : register(u0);  // write EMA result (next frame's history)
RWTexture2D<float4> g_outSurface : register(u1); // write current-frame surface metadata
RWTexture2D<float4> g_outMaterial : register(u2); // write current-frame material metadata

float3 ReconstructWorldPos(uint2 gbufferPixel, float cameraDistance)
{
    float ndcX = ((float(gbufferPixel.x) + 0.5f) / float(g_gbufferWidth)) * 2.0f - 1.0f;
    float ndcY = -((float(gbufferPixel.y) + 0.5f) / float(g_gbufferHeight)) * 2.0f + 1.0f;
    float4 viewRayH = mul(float4(ndcX, ndcY, 1.0f, 1.0f), g_invVP);
    float3 viewRay = normalize(viewRayH.xyz / viewRayH.w - g_cameraPos);
    return g_cameraPos + viewRay * cameraDistance;
}

bool ReprojectToPreviousHistory(float3 worldPos, out uint2 prevHistoryPixel, out float expectedPrevDistance)
{
    prevHistoryPixel = uint2(0u, 0u);
    expectedPrevDistance = 0.0f;

    float4 prevClip = mul(float4(worldPos, 1.0f), g_prevVP);
    if (prevClip.w <= 1e-5f)
        return false;

    float2 prevNdc = prevClip.xy / prevClip.w;
    float2 prevUv = prevNdc * float2(0.5f, -0.5f) + 0.5f;
    if (any(prevUv < 0.0f) || any(prevUv > 1.0f))
        return false;

    prevHistoryPixel = uint2(
        min(uint(prevUv.x * float(g_width)), g_width - 1u),
        min(uint(prevUv.y * float(g_height)), g_height - 1u));
    expectedPrevDistance = length(worldPos - g_prevCameraPos);
    return expectedPrevDistance > 1e-5f;
}

float HistoryValidationWeight(float4 curSurface, float4 histSurface, float expectedHistDepth)
{
    if (g_validationEnabled == 0u)
        return 1.0f;

    const float curDepth = curSurface.w;
    const float histDepth = histSurface.w;
    if (curDepth <= 1e-5f || histDepth <= 1e-5f || expectedHistDepth <= 1e-5f)
        return 0.0f;

    const float3 curN = normalize(curSurface.xyz * 2.0f - 1.0f);
    const float3 histN = normalize(histSurface.xyz * 2.0f - 1.0f);
    const float normalWeight = smoothstep(0.82f, 0.96f, dot(curN, histN));

    const float depthTolerance = max(0.03f, expectedHistDepth * 0.015f);
    const float depthWeight = 1.0f - smoothstep(depthTolerance, depthTolerance * 3.0f, abs(histDepth - expectedHistDepth));

    return normalWeight * depthWeight;
}

float MaterialValidationWeight(float4 curMaterial, float4 histMaterial)
{
    if (g_materialValidationEnabled == 0u)
        return 1.0f;

    const float roughnessDiff = abs(curMaterial.r - histMaterial.r);
    const float metallicDiff = abs(curMaterial.g - histMaterial.g);
    const float aoDiff = abs(curMaterial.b - histMaterial.b);

    const float roughnessWeight = 1.0f - smoothstep(0.05f, 0.18f, roughnessDiff);
    const float metallicWeight = 1.0f - smoothstep(0.04f, 0.16f, metallicDiff);
    const float aoWeight = 1.0f - smoothstep(0.15f, 0.35f, aoDiff);
    return roughnessWeight * metallicWeight * aoWeight;
}

[numthreads(16, 16, 1)]
void CS_ReflectionTemporal(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_width || id.y >= g_height)
        return;

    float4 cur  = g_current.Load(int3(id.xy, 0));
    if (any(isnan(cur)) || any(isinf(cur)))
    {
        // A NaN/Inf value written into g_outHist below would poison this pixel's history
        // permanently: NaN <= 0.0f is always false, so the hist.a <= 0.0f early-out never
        // triggers to self-heal it, and lerp(NaN, cur, alpha) stays NaN regardless of alpha.
        cur = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const uint2 surfacePx = (g_validationEnabled != 0u)
        ? uint2(
            min(uint(float(id.x) * float(g_gbufferWidth) / float(g_width)), g_gbufferWidth - 1u),
            min(uint(float(id.y) * float(g_gbufferHeight) / float(g_height)), g_gbufferHeight - 1u))
        : id.xy;
    float4 curSurface = g_currentSurface.Load(int3(surfacePx, 0));
    float4 curMaterial = g_currentMaterial.Load(int3(surfacePx, 0));

    uint2 prevHistoryPx = id.xy;
    float expectedPrevDepth = curSurface.w;
    bool reprojected = true;
    if (g_validationEnabled != 0u)
    {
        const float3 worldPos = ReconstructWorldPos(surfacePx, curSurface.w);
        reprojected = ReprojectToPreviousHistory(worldPos, prevHistoryPx, expectedPrevDepth);
    }

    float4 hist = g_history.Load(int3(prevHistoryPx, 0));
    float4 histSurface = g_historySurface.Load(int3(prevHistoryPx, 0));
    float4 histMaterial = g_historyMaterial.Load(int3(prevHistoryPx, 0));

    if (any(isnan(hist)) || any(isinf(hist)) ||
        any(isnan(histSurface)) || any(isinf(histSurface)) ||
        any(isnan(histMaterial)) || any(isinf(histMaterial)))
    {
        // Freshly-allocated (first frame / resize) ping-pong history textures are
        // committed with no clear value and never explicitly cleared, so their
        // initial content is undefined per the D3D12 spec. A NaN/Inf here would
        // otherwise poison this pixel's history permanently: lerp(hist, cur, alpha)
        // stays NaN regardless of alpha, and NaN survives the additive, provably
        // non-negative composite pipeline downstream (0 * NaN = NaN, not 0).
        // Force hist.a <= 0.0f so the existing self-heal branch below discards it
        // and writes cur directly instead.
        hist.a = 0.0f;
    }

    if (cur.a <= 0.0f || hist.a <= 0.0f || !reprojected)
    {
        // ReprojectToPreviousHistory writes its out-param unconditionally at entry
        // (before its own early-return checks), so on reprojection failure
        // prevHistoryPx silently collapses to (0,0) instead of a location related
        // to id.xy: hist/histSurface/histMaterial above were just loaded from an
        // unrelated pixel. The weight computation below would zero historyWeight
        // and drive effectiveAlpha to 1.0f expecting lerp(hist,cur,1.0f) to reduce
        // to cur, but HLSL's lerp is not guaranteed bit-exact at t=1 (it compiles
        // to hist + t*(cur-hist)): when the unrelated hist sample is far larger in
        // magnitude than cur, cur-hist rounds to -hist and the result collapses to
        // exactly 0 (solid black) via catastrophic cancellation. Bypass the blend
        // entirely and write cur directly, same as the existing self-heal branch.
        g_outHist[id.xy] = cur;
        g_outSurface[id.xy] = curSurface;
        g_outMaterial[id.xy] = curMaterial;
        return;
    }

    const float surfaceWeight = HistoryValidationWeight(curSurface, histSurface, expectedPrevDepth);
    const float materialWeight = MaterialValidationWeight(curMaterial, histMaterial);
    const float historyWeight = surfaceWeight * materialWeight;

    // Rougher surfaces tend to be blurrier and tolerate more history, while
    // smoother surfaces benefit from a slightly more reactive blend.
    const float roughness = saturate(curMaterial.r);
    const float roughnessHistoryScale = lerp(0.85f, 1.25f, roughness);
    const float weightedHistory = saturate(historyWeight * roughnessHistoryScale);
    const float effectiveAlpha = lerp(1.0f, g_alpha, weightedHistory);
    // Same non-exact-lerp-at-t=1 hazard as the !reprojected branch above, reached
    // via a different path: a successfully-reprojected pixel can still have
    // weightedHistory == 0 (validation rejects hist -- e.g. depth/normal mismatch
    // from animated geometry such as the fluid surface, not just camera motion),
    // which makes effectiveAlpha exactly 1.0f. lerp(hist,cur,1.0f) is not
    // guaranteed to equal cur bit-exactly, and can collapse to 0 (black) via
    // catastrophic cancellation when hist's magnitude dwarfs cur's. Bypass lerp
    // and write cur directly whenever history carries zero weight.
    float4 blended = (weightedHistory <= 0.0f) ? cur : lerp(hist, cur, effectiveAlpha);

    g_outHist[id.xy] = blended;
    g_outSurface[id.xy] = curSurface;
    g_outMaterial[id.xy] = curMaterial;
}
