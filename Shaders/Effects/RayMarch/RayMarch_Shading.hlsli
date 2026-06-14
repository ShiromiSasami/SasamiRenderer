#ifndef SASAMI_RAYMARCH_SHADING_HLSLI
#define SASAMI_RAYMARCH_SHADING_HLSLI

// Water and solid material shading helpers.

float3 shadeWater(float3 pos, float3 rd, float tHit)
{
    // Precision-safe stable XZ: CamLocalXZ() + rd.xz*tHit keeps both operands
    // small.  MakeStableXZ(pos.xz) would cancel (pos.xz = large + large).
    float2 stableXZ = CamLocalXZ() + rd.xz * tHit;
    float3 N   = waveNormal(stableXZ, u_time);
    float3 V   = -rd;
    float3 sunDir = normalize(u_sunDir);

    float NdotV = saturate(dot(N, V));

    // Schlick Fresnel (F0=0.02 for water at normal incidence)
    float fresnel = 0.02f + 0.98f * pow(1.0f - NdotV, 5.0f);

    // Reflected sky
    float3 reflDir  = reflect(rd, N);
    float3 reflColor = skyColor(reflDir);

    // Water depth absorption color
    float3 deepColor    = float3(0.00f, 0.08f, 0.18f);
    float3 shallowColor = float3(0.04f, 0.28f, 0.42f);
    float3 waterColor   = lerp(deepColor, shallowColor, pow(NdotV, 1.5f));

    // Whitecap foam at wave crests
    float wh   = waveHeight(stableXZ, u_time);
    float foam = smoothstep(0.22f, 0.40f, wh);

    // Subsurface ambient (tint from sky overhead)
    float3 skyAmb = skyColor(float3(0.0f, 1.0f, 0.0f)) * 0.20f;
    waterColor   += skyAmb * (1.0f - fresnel);

    // Caustic shimmer (cheap: use wave height + view angle)
    float caustic = pow(saturate(dot(N, float3(0.0f, 1.0f, 0.0f))), 32.0f)
                  * smoothstep(0.0f, 0.06f, wh + 0.06f) * 0.4f;
    waterColor   += float3(0.3f, 0.6f, 0.8f) * caustic;

    // Sun specular highlight
    float3 H    = normalize(sunDir + V);
    float  spec = pow(max(0.0f, dot(N, H)), 512.0f) * u_sunI;

    // Soft shadow from objects above water
    float shadow = softShadow(pos + float3(0.0f, 0.01f, 0.0f), sunDir, 0.05f, 15.0f, 8.0f);

    // Blend water color and reflection by Fresnel
    float3 color = lerp(waterColor, reflColor, fresnel);

    // Add foam layer
    color = lerp(color, float3(0.92f, 0.96f, 1.0f), foam * 0.7f);

    // Specular on top (not shadowed for now 窶・specular can be direct)
    color += u_sunColor * spec * 3.5f;

    // Multiply non-foam by shadow (diffuse sun contribution)
    float sunDiff = max(0.0f, dot(N, sunDir)) * shadow * u_sunI * (1.0f - foam);
    color += u_sunColor * sunDiff * waterColor * 0.12f;

    return color;
}

// ---------------------------------------------------------------------------
// Solid object albedo
// ---------------------------------------------------------------------------
float3 solidAlbedo(float matId, float3 pos)
{
    if (matId < 1.5f) return float3(0.90f, 0.35f, 0.10f);  // sphere: warm orange
    if (matId < 2.5f) return float3(0.20f, 0.40f, 0.75f);  // box: steel blue
    return float3(0.22f, 0.52f, 0.18f);                      // capsule: mossy green
}


#endif // SASAMI_RAYMARCH_SHADING_HLSLI
