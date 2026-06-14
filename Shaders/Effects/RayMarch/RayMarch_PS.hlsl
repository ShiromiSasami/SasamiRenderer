// RayMarch_PS.hlsl
// SDF raymarching demo: sphere, box, capsule, animated water surface + procedural sky.
//
// CB layout matches PushCameraCB (5-arg form):
//   u_invVP           -> camera inverse view-projection (row-major, compatibility only)
//   world[ 0.. 3]     -> camPos.xyz, time
//   world[ 4.. 7]     -> sunDir.xyz, sunIntensity
//   world[ 8..11]     -> sunColor.rgb, cloudCover
//   world[12..15]     -> renderW, renderH, reserved, cloudDensity
//   extra0[4]         -> debugMode, tanHalfFovX, tanHalfFovY, explicitCameraBasisEnabled
//   extra1[4]         -> cameraRight.xyz, reserved
//   extra2[4]         -> cameraUp.xyz, reserved

cbuffer RayMarchCB : register(b0)
{
    row_major float4x4 u_invVP;

    float3 u_camPos;      float u_time;
    float3 u_sunDir;      float u_sunI;
    float3 u_sunColor;    float u_cloudCover;
    float  u_renderW;     float u_renderH; float u_fluidMode; float u_cloudDensity;

    float4 u_extra0;
    float4 u_extra1;
    float4 u_extra2;
}

static const float kEps      = 0.0005f;
static const float kMaxDist  = 300.0f;   // increased for better coverage when camera is elevated
static const int   kMaxSteps = 512;      // increased to prevent step exhaustion at large distances
static const float PI = 3.14159265f;

// ---------------------------------------------------------------------------
// Floating Origin helpers
// ---------------------------------------------------------------------------
// Snap to a 1024 m grid so that wave/noise inputs are always small (< 1024 u).
// This prevents float32 catastrophic cancellation at large world coordinates
// (cam.x ~ 10^6 竊・ULP 竕・0.125 u 竊・visible wave quantisation).
//
// All wave / noise functions now take "stable XZ" = MakeStableXZ(worldXZ).
// sdWater and shadeWater call MakeStableXZ once and pass the result down,
// keeping the SDF comparison in world Y (unchanged).
static const float kFloatOriginCell = 1024.0f;

#include "RayMarch_WaterScene.hlsli"
#include "RayMarch_CloudSky.hlsli"
#include "RayMarch_Shading.hlsli"

float4 PSMain(float4 svPos : SV_POSITION) : SV_TARGET
{
    // Explicit basis mode avoids far-world subtraction. Otherwise the shader
    // falls back to the original invVP path.
    float2 uv  = svPos.xy / float2(u_renderW, u_renderH);
    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y      = -ndc.y;

    float3 ro = u_camPos;
    float3 rd;
    float3 right = u_extra1.xyz;
    float3 up = u_extra2.xyz;
    float3 forward = normalize(cross(right, up));
    if (u_extra0.w > 0.5f) {
        rd = normalize(forward + right * (ndc.x * u_extra0.y) + up * (ndc.y * u_extra0.z));
    } else {
        float4 near4  = mul(float4(ndc, 0.0f, 1.0f), u_invVP);
        float4 far4   = mul(float4(ndc, 1.0f, 1.0f), u_invVP);
        float3 nearWS = near4.xyz / near4.w;
        float3 farWS  = far4.xyz  / far4.w;
        rd = normalize(farWS - nearWS);
    }

    // Cone marching: compute pixel footprint half-angle so the termination
    // epsilon grows proportionally with distance (Quilez / Keinert et al.).
    // At distance t the cone radius is pixelConeAngle*t 窶・we stop when the
    // SDF is smaller than that radius, matching screen-space quality at any t.
    float2 adjNdc         = float2(ndc.x, ndc.y + 2.0f / u_renderH);
    float3 adjRd;
    if (u_extra0.w > 0.5f) {
        adjRd = normalize(forward + right * (adjNdc.x * u_extra0.y) + up * (adjNdc.y * u_extra0.z));
    } else {
        float4 near4   = mul(float4(ndc, 0.0f, 1.0f), u_invVP);
        float4 adjFar4 = mul(float4(adjNdc, 1.0f, 1.0f), u_invVP);
        float3 nearWS  = near4.xyz / near4.w;
        adjRd = normalize(adjFar4.xyz / adjFar4.w - nearWS);
    }
    float  pixelConeAngle = max(length(rd - adjRd), 1e-5f);

    // ---- Ray march ----
    // camLocal is computed once: it is always small (< kFloatOriginCell) so
    // camLocal + rd.xz * t never suffers catastrophic cancellation even when
    // the camera world-XZ is ~10^6.  This is used to produce a precision-safe
    // water SDF that overrides the one inside map() (which goes through the
    // large sum ro.xz + rd.xz * t 竊・MakeStableXZ 竊・still cancels).
    float2 camLocal = CamLocalXZ();
    float t     = 0.02f;
    float matId = -1.0f;
    for (int i = 0; i < kMaxSteps; ++i) {
        float3 p = ro + rd * t;
        float2 h = map(p);
        // Precision-safe water SDF.
        // camLocal + rd.xz*t: both operands are small 竊・no cancellation.
        // Override whenever map() chose water OR when precise water is closer
        // than whatever solid map() returned (guards against imprecise union).
        float2 stableXZ = camLocal + rd.xz * t;
        float  dWaterP  = (p.y - waveHeight(stableXZ, u_time)) * 0.75f;
        if (h.y < 0.5f || dWaterP < h.x) h = float2(dWaterP, 0.0f);
        bool  isWater = (h.y < 0.5f);
        float epsilon = isWater ? kEps : max(pixelConeAngle * t * 0.2f, kEps);
        if (h.x < epsilon) { matId = h.y; break; }
        t += h.x;
        if (t > kMaxDist) break;
    }

    // ---- Miss: procedural sky + volumetric clouds ----
    if (matId < 0.0f) {
        // Debug heatmap: sky = white (no hit distance)
        if (u_extra0.x > 0.5f) return float4(1.0f, 1.0f, 1.0f, 1.0f);
        float3 sunDir = normalize(u_sunDir);
        float3 col = ComputeSkyColor(rd, sunDir, u_sunColor, u_sunI);

        if (rd.y > 0.01f && u_cloudCover > 0.01f) {
            float cloudDensity = max(u_cloudDensity, 0.5f);
            // Per-pixel jitter in [0,1): breaks fixed-step banding (horizontal lines)
            // at cloud edges where transmittance changes rapidly.
            float jitter = frac(sin(dot(svPos.xy, float2(127.1f, 311.7f))) * 43758.5453f);
            float4 clouds = MarchClouds(rd, sunDir, u_sunColor, u_sunI,
                                        u_time, u_cloudCover, cloudDensity, jitter);
            col = lerp(col, clouds.rgb / max(clouds.a, 1e-4f), saturate(clouds.a));
        }

        return float4(col, 1.0f);
    }

    float3 pos = ro + rd * t;

    // ---- Water surface ----
    float3 color;
    if (matId < 0.5f) {
        color = shadeWater(pos, rd, t);
    }
    else {
        // ---- Solid objects ----
        float3 N  = calcNormal(pos);
        float3 V  = -rd;
        float3 sunDir = normalize(u_sunDir);

        float ao     = calcAO(pos, N);
        float shadow = softShadow(pos + N * 0.002f, sunDir, 0.01f, 20.0f, 8.0f);

        float3 albedo = solidAlbedo(matId, pos);

        float NdotL   = max(0.0f, dot(N, sunDir));
        float3 diffuse = albedo * u_sunColor * u_sunI * NdotL * shadow;
        float3 ambient = albedo * skyColor(N) * 0.15f * ao;

        float3 H      = normalize(sunDir + V);
        float3 spec   = u_sunColor * u_sunI * pow(max(0.0f, dot(N, H)), 48.0f) * shadow * 0.3f;

        color = diffuse + ambient + spec;
    }

    // Atmospheric fog
    float fogFactor = 1.0f - exp(-t * 0.007f);
    color = lerp(color, skyColor(rd) * 0.55f, fogFactor);

    // 笏笏 DEBUG: Distance Heatmap 笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏笏
    // u_extra0.x > 0.5 竊・override output with hit-distance color.
    // blue(near) 竊・cyan 竊・green 竊・yellow 竊・red(kMaxDist)
    // Sky pixels are rendered white so they don't confuse the distance reading.
    if (u_extra0.x > 0.5f)
    {
        float tn = saturate(t / kMaxDist);
        float3 dbgCol;
        if      (tn < 0.25f) { float s = tn / 0.25f;          dbgCol = lerp(float3(0,0,1), float3(0,1,1), s); }
        else if (tn < 0.50f) { float s = (tn-0.25f) / 0.25f;  dbgCol = lerp(float3(0,1,1), float3(0,1,0), s); }
        else if (tn < 0.75f) { float s = (tn-0.50f) / 0.25f;  dbgCol = lerp(float3(0,1,0), float3(1,1,0), s); }
        else                 { float s = (tn-0.75f) / 0.25f;  dbgCol = lerp(float3(1,1,0), float3(1,0,0), s); }
        return float4(dbgCol, 1.0f);
    }

    return float4(color, 1.0f);
}
