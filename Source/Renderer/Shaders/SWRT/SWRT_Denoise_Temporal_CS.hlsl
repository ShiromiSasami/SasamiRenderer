//
// SWRT_Denoise_Temporal_CS.hlsl
// Pass 5 of the ReSTIR DI pipeline – SVGF temporal accumulation.
//
// Blends the current shaded colour with the previous frame's filtered result
// and accumulates luminance moments for the subsequent A-Trous wavelet filter.
//
// Bindings:
//   b0  : ReSTIRFrameConstants (inline CBV)
//   t6  : g_shadedColor  (Pass4 output – scratch SRV[0])
//   t7  : g_prevColor    (prev frame filtered colour – scratch SRV[1])
//   t8  : g_prevMoments  (prev frame moments R16G16_FLOAT – scratch SRV[2])
//   t9  : g_gbuffer      (current GBuffer for depth/normal – scratch SRV[3])
//   u0  : g_momentsOut   (current moments R16G16_FLOAT – scratch UAV[0])
//   u1  : g_colorOut     (temporally-blended colour     – scratch UAV[1])
//

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
    float  g_temporalAlpha;   // typically 0.1
    float  g_phiColor;
    float  g_phiNormal;
    float  g_phiDepth;
    float  g_stepWidth;
    float  g_maxSurfaceRoughness;
    float  g_maxPrimaryHitDistance;
    float  g_minReflectionEnergy;
    float3 g_dirLightDir;
    float  g_dirLightIntensity;
    float3 g_dirLightColor;
    float  g_shadowBias;
    float3 g_ambientColor;
    float  g_ambientIntensity;
    uint   g_pointLightCount;
    uint   g_spotLightCount;
    uint   g_cbPad0;
    uint   g_cbPad1;
};

Texture2D<float4> g_shadedColor : register(t6);   // scratch SRV[0]
Texture2D<float4> g_prevColor   : register(t7);   // scratch SRV[1]
Texture2D<float2> g_prevMoments : register(t8);   // scratch SRV[2]
Texture2D<float4> g_gbuffer     : register(t9);   // scratch SRV[3]

RWTexture2D<float2> g_momentsOut : register(u0);  // scratch UAV[0]
RWTexture2D<float4> g_colorOut   : register(u1);  // scratch UAV[1]

float Luminance(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

[numthreads(16, 16, 1)]
void CS_Denoise_Temporal(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_renderWidth || id.y >= g_renderHeight) return;

    float4 cur  = g_shadedColor[id.xy];
    float4 gbuf = g_gbuffer[id.xy];
    float  depth = gbuf.w;

    // Sky / miss pixels: pass through with no history
    if (depth < 0.0f)
    {
        g_colorOut[id.xy]   = cur;
        g_momentsOut[id.xy] = float2(0.0f, 0.0f);
        return;
    }

    // On the first frame there is no previous data – skip temporal
    if (g_frameIndex == 0u)
    {
        float lum = Luminance(cur.rgb);
        g_colorOut[id.xy]   = cur;
        g_momentsOut[id.xy] = float2(lum, lum * lum);
        return;
    }

    // Simple same-pixel temporal (stationary scene assumption).
    // A full implementation would reproject using motion vectors.
    float4 prev   = g_prevColor[id.xy];
    float2 pMom   = g_prevMoments[id.xy];

    // Validate: reject if previous pixel doesn't exist or is sky
    // (no depth stored; we accept if prev colour alpha > 0 as proxy)
    bool historyValid = (prev.a > 0.0f);

    float3 blendColor;
    if (historyValid)
    {
        float alpha = g_temporalAlpha;
        blendColor  = lerp(prev.rgb, cur.rgb, alpha);
    }
    else
    {
        blendColor = cur.rgb;
    }

    float lum  = Luminance(blendColor);
    float lum2 = lum * lum;

    float2 newMom;
    if (historyValid)
    {
        float alpha = g_temporalAlpha;
        newMom.x = lerp(pMom.x, lum,  alpha);
        newMom.y = lerp(pMom.y, lum2, alpha);
    }
    else
    {
        newMom = float2(lum, lum2);
    }

    g_colorOut[id.xy]   = float4(blendColor, cur.a);
    g_momentsOut[id.xy] = newMom;
}
