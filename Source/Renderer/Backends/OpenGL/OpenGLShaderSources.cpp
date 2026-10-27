#include "Renderer/Backends/OpenGL/OpenGLShaderSources.h"

namespace SasamiEngine::Render
{
    extern const char* const kOpenGLNativeMeshVS = R"GLSL(
#version 120
attribute vec3 a_position;
attribute vec3 a_normal;
attribute vec4 a_color;
attribute vec2 a_uv;
uniform mat4 u_modelViewProjection;
uniform mat4 u_model;
uniform vec4 u_baseColor;
varying vec3 v_worldPos;
varying vec3 v_worldNormal;
varying vec4 v_color;
varying vec2 v_uv;
void main()
{
    gl_Position = u_modelViewProjection * vec4(a_position, 1.0);
    v_worldPos = (u_model * vec4(a_position, 1.0)).xyz;
    v_worldNormal = mat3(u_model) * a_normal;
    v_color = a_color * u_baseColor;
    v_uv = a_uv;
}
)GLSL";

    extern const char* const kOpenGLNativeMeshPS = R"GLSL(
#version 120
uniform vec4 u_lightDirIntensity;
uniform vec4 u_lightColor;
uniform vec4 u_emissiveRoughness;
uniform sampler2D u_albedoTexture;
uniform int u_hasAlbedoTexture;
uniform sampler2D u_occlusionTexture;
uniform int u_hasOcclusionTexture;
uniform samplerCube u_ambientCube;
uniform int u_hasAmbientCube;
// Screen-space ambient occlusion produced by the native SSAO pass, sampled by window position
// because this forward shader has no screen UV of its own. u_hasRuntimeAoTexture is 0 whenever the
// pass could not run (feature off, no glClipControl, failed program), and the shader then falls
// back to runtimeAo = 1.0, which reproduces the pre-SSAO picture exactly.
uniform sampler2D u_runtimeAoTexture;
uniform int u_hasRuntimeAoTexture;
uniform vec2 u_renderSize;
uniform vec3 u_cameraPos;
uniform float u_metallic;
uniform float u_enableIbl;
varying vec3 v_worldPos;
varying vec3 v_worldNormal;
varying vec4 v_color;
varying vec2 v_uv;
// Narkowicz 2015 ACES fit with exposure, same as Shaders/Effects/PostProcess/ToneMap_PS.hlsl.
// RenderSettings::exposure
uniform float u_exposure;
vec3 nativeTonemap(vec3 hdr)
{
    vec3 x = hdr * u_exposure;
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}
// Chan 2018, "Material Advances in Call of Duty: WWII" (adopted by Filament as
// computeMicroShadowing) -- the same formula as Shaders/Shared/Common/MicroShadowing.hlsli,
// transcribed rather than included because this backend's shaders are hand-written GLSL and
// cannot share an HLSL header. rsqrt/saturate/lerp become inversesqrt/clamp/mix.
float computeMicroShadowing(float ndotl, float ao, float strength)
{
    float aperture = inversesqrt(1.0 - min(ao, 0.9999));
    float microShadow = clamp(ndotl * aperture, 0.0, 1.0);
    microShadow *= microShadow;
    return mix(1.0, microShadow, clamp(strength, 0.0, 1.0));
}
// Transcribed from Shaders/Native/NativePbr.hlsli and Shaders/Native/NativeLighting.hlsli (the
// same model the DX11 and Vulkan native paths evaluate): GGX + correlated Smith + Schlick.
const float kPi = 3.14159265359;
float D_GGX(float NoH, float roughness)
{
    float alpha = max(roughness * roughness, 0.002025);
    float alpha2 = alpha * alpha;
    float denom = (NoH * NoH) * (alpha2 - 1.0) + 1.0;
    denom = kPi * denom * denom;
    return alpha2 / denom;
}
float V_SmithGGXCorrelated(float NoV, float NoL, float roughness)
{
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float lambdaV = NoL * sqrt(max(NoV * NoV * (1.0 - alpha2) + alpha2, 1e-5));
    float lambdaL = NoV * sqrt(max(NoL * NoL * (1.0 - alpha2) + alpha2, 1e-5));
    return 0.5 / max(lambdaV + lambdaL, 1e-5);
}
vec3 F_Schlick(vec3 f0, float VoH)
{
    float f = pow(clamp(1.0 - VoH, 0.0, 1.0), 5.0);
    return f0 + (1.0 - f0) * f;
}
vec3 evaluatePbr(vec3 albedo, float metallic, float roughness, vec3 n, vec3 v, vec3 l,
                 vec3 lightColor, float lightIntensity)
{
    vec3 h = normalize(v + l);
    float NoV = max(dot(n, v), 1e-5);
    float NoL = clamp(dot(n, l), 0.0, 1.0);
    float NoH = clamp(dot(n, h), 0.0, 1.0);
    float VoH = clamp(dot(v, h), 0.0, 1.0);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    float D = D_GGX(NoH, roughness);
    float V = V_SmithGGXCorrelated(NoV, NoL, roughness);
    vec3 F = F_Schlick(f0, VoH);
    vec3 specular = D * V * F;
    vec3 diffuse = albedo * (1.0 - metallic) / kPi;
    return (diffuse + specular) * lightColor * lightIntensity * NoL;
}
float resolveAo(float materialAo, float runtimeAo)
{
    const float kNativeAoFloor = 0.25;
    float rawAo = clamp(materialAo * runtimeAo, 0.0, 1.0);
    return mix(kNativeAoFloor, 1.0, rawAo);
}
vec3 hemisphereAmbient(vec3 n)
{
    float skyWeight = clamp(n.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 skyColor = vec3(0.35, 0.45, 0.60);
    vec3 groundColor = vec3(0.12, 0.10, 0.08);
    return mix(groundColor, skyColor, skyWeight);
}
vec3 shadeSurface(vec3 albedo, float metallic, float roughness, vec3 n, vec3 v, vec3 l,
                  vec3 lightColor, float lightIntensity, float shadowTerm, float ao,
                  bool ambientEnabled, vec3 ambientRadiance, vec3 emissive)
{
    const float kNativeAoDirectStrength = 0.5;
    vec3 direct = evaluatePbr(albedo, metallic, roughness, n, v, l, lightColor, lightIntensity) * shadowTerm;
    direct *= computeMicroShadowing(clamp(dot(n, l), 0.0, 1.0), ao, kNativeAoDirectStrength);
    vec3 ambient = vec3(0.0);
    if (ambientEnabled) {
        ambient = albedo * ambientRadiance * ao;
    }
    vec3 lit = direct + ambient;
    lit += emissive * ao;
    return lit;
}
void main()
{
    vec3 n = normalize(v_worldNormal);
    vec3 v = normalize(u_cameraPos - v_worldPos);
    vec3 l = normalize(-u_lightDirIntensity.xyz);
    float roughness = clamp(u_emissiveRoughness.w, 0.0, 1.0);
    float metallic = clamp(u_metallic, 0.0, 1.0);
    // This backend has no shadow map, so the shadow term is always fully lit.
    float shadowTerm = 1.0;
    vec4 surface = v_color;
    if (u_hasAlbedoTexture != 0) {
        vec4 texColor = texture2D(u_albedoTexture, v_uv);
        // Cut-out geometry -- fences, foliage cards, grilles -- stores its silhouette in the
        // albedo alpha and has no material flag anywhere in this renderer to announce itself.
        // Without this clip the whole quad is drawn and paints over what is behind it, which
        // reads as geometry popping in front of walls it should be hidden by. Only the sampled
        // alpha is tested, never v_color.a, so an opaque material tinted with a low alpha is
        // left alone.
        if (texColor.a < 0.5) {
            discard;
        }
        surface *= texColor;
    }
    float materialAo = (u_hasOcclusionTexture != 0) ? texture2D(u_occlusionTexture, v_uv).r : 1.0;
    float runtimeAo = 1.0;
    if (u_hasRuntimeAoTexture != 0) {
        runtimeAo = texture2D(u_runtimeAoTexture, gl_FragCoord.xy / max(u_renderSize, vec2(1.0))).r;
    }
    float ao = resolveAo(materialAo, runtimeAo);
    bool ambientEnabled = u_enableIbl > 0.5;
    vec3 ambientRadiance = (u_hasAmbientCube != 0) ? textureCube(u_ambientCube, n).rgb : hemisphereAmbient(n);
    vec3 lit = shadeSurface(surface.rgb, metallic, roughness, n, v, l, u_lightColor.rgb,
                            u_lightDirIntensity.w, shadowTerm, ao, ambientEnabled, ambientRadiance,
                            u_emissiveRoughness.rgb);
    lit = nativeTonemap(lit);
    lit = pow(clamp(lit, 0.0, 1.0), vec3(1.0 / 2.2));
    gl_FragColor = vec4(lit, surface.a);
}
)GLSL";

        // Ports Shaders/Raster/Geometry/SkinnedMesh/SkinnedMesh_VS.hlsl's up-to-4-influence bone
        // blending. Bone matrices are uploaded row-major, un-transposed (same convention as
        // u_modelViewProjection below and the ray-march u_invVP: GLSL reads it column-major, which
        // is the transpose, so `M * v` here reproduces HLSL's `mul(v, M_rowmajor)`). Skinning
        // blends into object/model space (mirrors the HLSL "bone-space -> model-space" comment);
        // u_modelViewProjection already folds in the per-draw model matrix; u_model is applied
        // after skinning only to produce the world position/normal for lighting. Like the static
        // mesh shader above, this skips the inverse-transpose normal fixup.
    extern const char* const kOpenGLNativeSkinnedMeshVS = R"GLSL(
#version 120
attribute vec3 a_position;
attribute vec3 a_normal;
attribute vec4 a_color;
attribute vec2 a_uv;
attribute vec4 a_boneIndices;
attribute vec4 a_boneWeights;
uniform mat4 u_modelViewProjection;
uniform mat4 u_model;
uniform vec4 u_baseColor;
uniform mat4 u_boneMatrices[128];
varying vec3 v_worldPos;
varying vec3 v_worldNormal;
varying vec4 v_color;
varying vec2 v_uv;
void main()
{
    vec4 pos4 = vec4(a_position, 1.0);
    mat4 boneX = u_boneMatrices[int(a_boneIndices.x + 0.5)];
    mat4 boneY = u_boneMatrices[int(a_boneIndices.y + 0.5)];
    mat4 boneZ = u_boneMatrices[int(a_boneIndices.z + 0.5)];
    mat4 boneW = u_boneMatrices[int(a_boneIndices.w + 0.5)];
    vec4 skinnedPos =
        a_boneWeights.x * (boneX * pos4) +
        a_boneWeights.y * (boneY * pos4) +
        a_boneWeights.z * (boneZ * pos4) +
        a_boneWeights.w * (boneW * pos4);
    vec3 skinnedNormal =
        a_boneWeights.x * (mat3(boneX) * a_normal) +
        a_boneWeights.y * (mat3(boneY) * a_normal) +
        a_boneWeights.z * (mat3(boneZ) * a_normal) +
        a_boneWeights.w * (mat3(boneW) * a_normal);
    gl_Position = u_modelViewProjection * skinnedPos;
    v_worldPos = (u_model * skinnedPos).xyz;
    v_worldNormal = mat3(u_model) * skinnedNormal;
    v_color = a_color * u_baseColor;
    v_uv = a_uv;
}
)GLSL";

        // ---- Ray march shaders ----
    extern const char* const kOpenGLRayMarchVS = R"GLSL(
#version 330 core
void main()
{
    float x = (gl_VertexID == 1) ?  3.0 : -1.0;
    float y = (gl_VertexID == 2) ?  3.0 : -1.0;
    gl_Position = vec4(x, y, 0.0, 1.0);
}
)GLSL";

    extern const char* const kOpenGLRayMarchFS_Part1 = R"GLSL(
#version 330 core
out vec4 fragColor;

// Uniforms matching RayMarchCB layout (row-major matrix uploaded un-transposed;
// GLSL reads it as column-major = transposed, so M*v == HLSL mul(v,M_rowmajor))
uniform mat4  u_invVP;
uniform vec3  u_camPos;     uniform float u_time;
uniform vec3  u_sunDir;     uniform float u_sunI;
uniform vec3  u_sunColor;   uniform float u_cloudCover;
uniform float u_renderW;    uniform float u_renderH;
uniform float u_fluidMode;  uniform float u_cloudDensity;
uniform vec4  u_extra0;
uniform vec4  u_extra1;
uniform vec4  u_extra2;

const float kEps      = 0.0005;
const float kMaxDist  = 300.0;
const int   kMaxSteps = 512;
const float PI        = 3.14159265;
const float kFloatOriginCell = 1024.0;

// ---------------------------------------------------------------------------
// Floating Origin helpers
// ---------------------------------------------------------------------------
vec2 CamOrigin()  { return floor(u_camPos.xz / kFloatOriginCell) * kFloatOriginCell; }
vec2 MakeStableXZ(vec2 pWS_xz) { return pWS_xz - CamOrigin(); }
vec2 CamLocalXZ()  { return u_camPos.xz - CamOrigin(); }

// ---------------------------------------------------------------------------
// Wave / fluid helpers
// ---------------------------------------------------------------------------
void waveLod(vec2 p_stable, out float midW, out float hiW)
{
    float dist = length(p_stable - CamLocalXZ());
    midW = clamp(1.0 - (dist - 160.0) / 160.0, 0.0, 1.0);
    hiW  = clamp(1.0 - (dist -  80.0) /  80.0, 0.0, 1.0);
}

float waveHeight(vec2 p, float t)
{
    float midW, hiW;
    waveLod(p, midW, hiW);
    float h = 0.0;
    h += sin(p.x * 1.40 + t * 1.80) * 0.180;
    h += sin(p.y * 1.90 + t * 1.40) * 0.140;
    h += sin(p.x * 0.70 - p.y * 1.10 + t * 0.90) * 0.090;
    h += sin(p.x * 2.80 + p.y * 2.10 - t * 2.30) * 0.048 * midW;
    h += sin(p.x * 4.10 - p.y * 3.30 + t * 3.10) * 0.024 * hiW;
    h += sin(p.x * 6.50 + p.y * 5.20 - t * 4.20) * 0.012 * hiW;
    return h;
}

float sdWater(vec3 p)
{
    return (p.y - waveHeight(MakeStableXZ(p.xz), u_time)) * 0.75;
}

vec3 waveNormal(vec2 p, float t)
{
    float midW, hiW;
    waveLod(p, midW, hiW);
    float dhdx = 0.0, dhdz = 0.0;
    dhdx += cos(p.x * 1.40 + t * 1.80) * 1.40 * 0.180;
    dhdz += cos(p.y * 1.90 + t * 1.40) * 1.90 * 0.140;
    dhdx += cos(p.x * 0.70 - p.y * 1.10 + t * 0.90) *  0.70 * 0.090;
    dhdz += cos(p.x * 0.70 - p.y * 1.10 + t * 0.90) * -1.10 * 0.090;
    dhdx += cos(p.x * 2.80 + p.y * 2.10 - t * 2.30) *  2.80 * 0.048 * midW;
    dhdz += cos(p.x * 2.80 + p.y * 2.10 - t * 2.30) *  2.10 * 0.048 * midW;
    dhdx += cos(p.x * 4.10 - p.y * 3.30 + t * 3.10) *  4.10 * 0.024 * hiW;
    dhdz += cos(p.x * 4.10 - p.y * 3.30 + t * 3.10) * -3.30 * 0.024 * hiW;
    dhdx += cos(p.x * 6.50 + p.y * 5.20 - t * 4.20) *  6.50 * 0.012 * hiW;
    dhdz += cos(p.x * 6.50 + p.y * 5.20 - t * 4.20) *  5.20 * 0.012 * hiW;
    return normalize(vec3(-dhdx, 1.0, -dhdz));
}

// ---------------------------------------------------------------------------
// SDF primitives
// ---------------------------------------------------------------------------
float sdSphere(vec3 p, float r) { return length(p) - r; }

float sdRoundBox(vec3 p, vec3 b, float r)
{
    vec3 q = abs(p) - b;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0) - r;
}

float sdCapsule(vec3 p, vec3 a, vec3 b, float r)
{
    vec3 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h) - r;
}

// ---------------------------------------------------------------------------
// Scene map
// ---------------------------------------------------------------------------
vec2 map(vec3 p)
{
    float bob = sin(u_time * 0.6) * 0.18;
    float dWater  = sdWater(p);
    vec3  pSphere = p - vec3(-1.8, 2.0 + bob, 0.5);
    float dSphere = sdSphere(pSphere, 1.0);
    float angle   = u_time * 0.3;
    vec3  pBox    = p - vec3(2.5, 1.2, -0.3);
    pBox.xz = vec2(pBox.x * cos(angle) + pBox.z * sin(angle),
                  -pBox.x * sin(angle) + pBox.z * cos(angle));
    float dBox    = sdRoundBox(pBox, vec3(0.75, 0.65, 0.75), 0.12);
    vec3  capA    = vec3(-4.2, -0.3,  0.8);
    vec3  capB    = vec3(-4.2,  2.8,  0.8);
    float dCap    = sdCapsule(p, capA, capB, 0.38);
    vec2  res     = vec2(dWater, 0.0);
    if (dSphere < res.x) res = vec2(dSphere, 1.0);
    if (dBox    < res.x) res = vec2(dBox,    2.0);
    if (dCap    < res.x) res = vec2(dCap,    3.0);
    return res;
}

vec3 calcNormal(vec3 p)
{
    vec2 e = vec2(kEps, 0.0);
    return normalize(vec3(
        map(p + e.xyy).x - map(p - e.xyy).x,
        map(p + e.yxy).x - map(p - e.yxy).x,
        map(p + e.yyx).x - map(p - e.yyx).x));
}

float softShadow(vec3 ro, vec3 rd, float mint, float maxt, float k)
{
    float res = 1.0, t = mint;
    for (int i = 0; i < 32; ++i) {
        float h = map(ro + rd * t).x;
        if (h < 0.001) return 0.0;
        res = min(res, k * h / t);
        t  += clamp(h, 0.01, 0.4);
        if (t > maxt) break;
    }
    return clamp(res, 0.0, 1.0);
}

float calcAO(vec3 pos, vec3 nor)
{
    float occ = 0.0, sca = 1.0;
    for (int i = 0; i < 5; i++) {
        float h = 0.01 + 0.12 * float(i) / 4.0;
        float d = map(pos + h * nor).x;
        occ    += (h - d) * sca;
        sca    *= 0.95;
    }
    return clamp(1.0 - 3.0 * occ, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// Cloud / sky
// ---------------------------------------------------------------------------
vec3 GradHash3(vec3 p)
{
    p = vec3(dot(p, vec3(127.1, 311.7,  74.7)),
             dot(p, vec3(269.5, 183.3, 246.1)),
             dot(p, vec3(113.5, 271.9, 124.6)));
    vec3 g = fract(sin(p) * 43758.5453) * 2.0 - 1.0;
    return g / (length(g) + 1e-5);
}

float GradNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return mix(
        mix(mix(dot(GradHash3(i),                 f),
                dot(GradHash3(i + vec3(1,0,0)), f - vec3(1,0,0)), u.x),
            mix(dot(GradHash3(i + vec3(0,1,0)), f - vec3(0,1,0)),
                dot(GradHash3(i + vec3(1,1,0)), f - vec3(1,1,0)), u.x), u.y),
        mix(mix(dot(GradHash3(i + vec3(0,0,1)), f - vec3(0,0,1)),
                dot(GradHash3(i + vec3(1,0,1)), f - vec3(1,0,1)), u.x),
            mix(dot(GradHash3(i + vec3(0,1,1)), f - vec3(0,1,1)),
                dot(GradHash3(i + vec3(1,1,1)), f - vec3(1,1,1)), u.x), u.y), u.z);
}

float FBM5(vec3 p)
{
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++) {
        v += a * (GradNoise(p) * 0.5 + 0.5);
        p  = p * 2.02 + vec3(73.1, 61.4, 53.7);
        a *= 0.5;
    }
    return v;
}

float SampleCloudDensity(vec3 worldPos, float time, float cloudCover, float cloudDensity)
{
    const float heightScale = 4.0;
    vec3 p = vec3(worldPos.x, worldPos.y * heightScale, worldPos.z) * 0.0001;
    p.x += time * 0.008;
    p.z += time * 0.003;
    float base   = FBM5(p * 0.8);
    float detail = FBM5(p * 2.5 + vec3(1.4, 2.1, 0.9) + time * 0.002) * 0.35;
    float raw    = base + detail - (1.0 - cloudCover);
    return max(raw * cloudDensity, 0.0);
}

vec4 MarchClouds(vec3 rd, vec3 sunDir, vec3 sunColor, float sunIntensity,
                 float time, float cloudCover, float cloudDensity, float jitter)
{
    const float kCloudBaseY = 1500.0;
    const float kCloudTopY  = 5000.0;
    if (rd.y < 0.005) return vec4(0.0);
    float tBase = (kCloudBaseY - u_camPos.y) / rd.y;
    float tTop  = (kCloudTopY  - u_camPos.y) / rd.y;
    if (tTop <= 0.0) return vec4(0.0);
    tBase = max(tBase, 0.0);
    if (tBase > 300000.0) return vec4(0.0);

    const int kSteps = 48;
    float dt = (tTop - tBase) / float(kSteps);
    float g = 0.6, g2 = g * g;
    float cosSun = dot(rd, sunDir);
    float denom  = max(1.0 + g2 - 2.0 * g * cosSun, 1e-6);
    float phaseHG = (1.0 - g2) / (4.0 * PI * pow(denom, 1.5));

    float transmittance = 1.0;
    vec3  scattered     = vec3(0.0);

    for (int i = 0; i < kSteps; i++) {
        if (transmittance < 0.02) break;
        float  t       = tBase + (float(i) + jitter) * dt;
        vec3   pos     = u_camPos + rd * t;
        float  density = SampleCloudDensity(pos, time, cloudCover, cloudDensity);
        if (density < 1e-4) continue;
        float sigmaE   = density * 0.0008;
        float sampleT  = exp(-sigmaE * dt);
        float sunAtten = exp(-density * 1.2);
        vec3  sunLit   = sunColor * sunIntensity * sunAtten * phaseHG;
        vec3  ambient  = vec3(0.35, 0.50, 0.72) * 0.28;
        vec3  inScatter = (sunLit + ambient) * density;
        scattered += inScatter * transmittance * (1.0 - sampleT) / max(sigmaE, 1e-6);
        transmittance *= sampleT;
    }
    return vec4(scattered, 1.0 - transmittance);
}

)GLSL";

        // The analytic sky, shared by the ray-march background above and the native sky pass below.
        // The canonical version is HLSL (Shaders/Effects/Sky/ProceduralSky/ProceduralSky.hlsli);
        // this is its GLSL translation, kept as one copy rather than two because the OpenGL backend
        // cannot #include the HLSL and a second transcription would drift.
    extern const char* const kOpenGLSkyFunctionGLSL = R"GLSL(
vec3 ComputeSkyColor(vec3 rd, vec3 sunDir, vec3 sunColor, float sunIntensity)
{
    float cosTheta  = dot(rd, sunDir);
    float elevation = rd.y;
    vec3 zenithColor  = vec3(0.05, 0.15, 0.65) * 2.0;
    vec3 horizonColor = vec3(0.50, 0.68, 0.88) * 1.5;
    vec3 groundColor  = vec3(0.08, 0.06, 0.04);
    vec3 skyBase;
    if (elevation >= 0.0)
        skyBase = mix(horizonColor, zenithColor, pow(clamp(elevation, 0.0, 1.0), 0.4));
    else
        skyBase = mix(horizonColor, groundColor, clamp(-elevation * 5.0, 0.0, 1.0));
    float sunInfluence = clamp(cosTheta * 0.5 + 0.5, 0.0, 1.0);
    vec3  warmTint = mix(vec3(1.0), sunColor * vec3(1.1, 0.9, 0.7), sunInfluence * 0.4);
    skyBase *= warmTint;
    float mie      = pow(max(cosTheta, 0.0), 6.0) * 0.3;
    vec3  mieColor = sunColor * mie * sunIntensity;
    float horizonFactor = pow(clamp(1.0 - abs(elevation), 0.0, 1.0), 3.0);
    vec3  horizGlow = sunColor
        * mix(vec3(1.0, 0.7, 0.4), vec3(1.0, 0.9, 0.7), sunIntensity)
        * horizonFactor * max(cosTheta, 0.0) * 0.4 * sunIntensity;
    float sunDisc = smoothstep(0.9996, 1.0, cosTheta);
    vec3  disc    = sunColor * sunDisc * 12.0 * sunIntensity;
    return max(skyBase + mieColor + horizGlow + disc, vec3(0.0));
}

)GLSL";

    extern const char* const kOpenGLRayMarchFS_Part2 = R"GLSL(
vec3 skyColor(vec3 rd)
{
    return ComputeSkyColor(rd, normalize(u_sunDir), u_sunColor, u_sunI);
}

// ---------------------------------------------------------------------------
// Shading
// ---------------------------------------------------------------------------
vec3 shadeWater(vec3 pos, vec3 rd, float tHit)
{
    vec2  stableXZ = CamLocalXZ() + rd.xz * tHit;
    vec3  N        = waveNormal(stableXZ, u_time);
    vec3  V        = -rd;
    vec3  sunDir   = normalize(u_sunDir);
    float NdotV    = clamp(dot(N, V), 0.0, 1.0);
    float fresnel  = 0.02 + 0.98 * pow(1.0 - NdotV, 5.0);
    vec3  reflDir  = reflect(rd, N);
    vec3  reflColor = skyColor(reflDir);
    vec3  deepColor    = vec3(0.00, 0.08, 0.18);
    vec3  shallowColor = vec3(0.04, 0.28, 0.42);
    vec3  waterColor   = mix(deepColor, shallowColor, pow(NdotV, 1.5));
    float wh   = waveHeight(stableXZ, u_time);
    float foam = smoothstep(0.22, 0.40, wh);
    vec3  skyAmb = skyColor(vec3(0.0, 1.0, 0.0)) * 0.20;
    waterColor  += skyAmb * (1.0 - fresnel);
    float caustic = pow(clamp(dot(N, vec3(0.0, 1.0, 0.0)), 0.0, 1.0), 32.0)
                  * smoothstep(0.0, 0.06, wh + 0.06) * 0.4;
    waterColor  += vec3(0.3, 0.6, 0.8) * caustic;
    vec3  H     = normalize(sunDir + V);
    float spec  = pow(max(0.0, dot(N, H)), 512.0) * u_sunI;
    float shadow = softShadow(pos + vec3(0.0, 0.01, 0.0), sunDir, 0.05, 15.0, 8.0);
    vec3  color  = mix(waterColor, reflColor, fresnel);
    color  = mix(color, vec3(0.92, 0.96, 1.0), foam * 0.7);
    color += u_sunColor * spec * 3.5;
    float sunDiff = max(0.0, dot(N, sunDir)) * shadow * u_sunI * (1.0 - foam);
    color += u_sunColor * sunDiff * waterColor * 0.12;
    return color;
}

vec3 solidAlbedo(float matId, vec3 pos)
{
    if (matId < 1.5) return vec3(0.90, 0.35, 0.10);
    if (matId < 2.5) return vec3(0.20, 0.40, 0.75);
    return vec3(0.22, 0.52, 0.18);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
void main()
{
    vec2 uv  = gl_FragCoord.xy / vec2(u_renderW, u_renderH);
    vec2 ndc = uv * 2.0 - 1.0;
    ndc.y    = -ndc.y;

    vec3 ro = u_camPos;
    vec3 rd;
    vec3 right   = u_extra1.xyz;
    vec3 up      = u_extra2.xyz;
    vec3 forward = normalize(cross(right, up));
    if (u_extra0.w > 0.5) {
        rd = normalize(forward + right * (ndc.x * u_extra0.y) + up * (ndc.y * u_extra0.z));
    } else {
        vec4 near4  = u_invVP * vec4(ndc, 0.0, 1.0);
        vec4 far4   = u_invVP * vec4(ndc, 1.0, 1.0);
        vec3 nearWS = near4.xyz / near4.w;
        vec3 farWS  = far4.xyz  / far4.w;
        rd = normalize(farWS - nearWS);
    }

    // Cone marching pixel footprint
    vec2 adjNdc = vec2(ndc.x, ndc.y + 2.0 / u_renderH);
    vec3 adjRd;
    if (u_extra0.w > 0.5) {
        adjRd = normalize(forward + right * (adjNdc.x * u_extra0.y) + up * (adjNdc.y * u_extra0.z));
    } else {
        vec4 near4   = u_invVP * vec4(ndc,    0.0, 1.0);
        vec4 adjFar4 = u_invVP * vec4(adjNdc, 1.0, 1.0);
        vec3 nearWS  = near4.xyz / near4.w;
        adjRd = normalize(adjFar4.xyz / adjFar4.w - nearWS);
    }
    float pixelConeAngle = max(length(rd - adjRd), 1e-5);

    // Ray march
    vec2  camLocal = CamLocalXZ();
    float t        = 0.02;
    float matId    = -1.0;
    for (int i = 0; i < kMaxSteps; ++i) {
        vec3  p        = ro + rd * t;
        vec2  h        = map(p);
        vec2  stableXZ = camLocal + rd.xz * t;
        float dWaterP  = (p.y - waveHeight(stableXZ, u_time)) * 0.75;
        if (h.y < 0.5 || dWaterP < h.x) h = vec2(dWaterP, 0.0);
        bool  isWater  = (h.y < 0.5);
        float epsilon  = isWater ? kEps : max(pixelConeAngle * t * 0.2, kEps);
        if (h.x < epsilon) { matId = h.y; break; }
        t += h.x;
        if (t > kMaxDist) break;
    }

    // Miss: sky + clouds
    if (matId < 0.0) {
        if (u_extra0.x > 0.5) { fragColor = vec4(1.0); return; }
        vec3 sunDir = normalize(u_sunDir);
        vec3 col    = ComputeSkyColor(rd, sunDir, u_sunColor, u_sunI);
        if (rd.y > 0.01 && u_cloudCover > 0.01) {
            float cloudDensity = max(u_cloudDensity, 0.5);
            float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(127.1, 311.7))) * 43758.5453);
            vec4  clouds = MarchClouds(rd, sunDir, u_sunColor, u_sunI,
                                       u_time, u_cloudCover, cloudDensity, jitter);
            col = mix(col, clouds.rgb / max(clouds.a, 1e-4), clamp(clouds.a, 0.0, 1.0));
        }
        fragColor = vec4(col, 1.0);
        return;
    }

    vec3 pos   = ro + rd * t;
    vec3 color;
    if (matId < 0.5) {
        color = shadeWater(pos, rd, t);
    } else {
        vec3  N      = calcNormal(pos);
        vec3  V      = -rd;
        vec3  sunDir = normalize(u_sunDir);
        float ao     = calcAO(pos, N);
        float shadow = softShadow(pos + N * 0.002, sunDir, 0.01, 20.0, 8.0);
        vec3  albedo = solidAlbedo(matId, pos);
        float NdotL  = max(0.0, dot(N, sunDir));
        vec3  diffuse = albedo * u_sunColor * u_sunI * NdotL * shadow;
        vec3  ambient = albedo * skyColor(N) * 0.15 * ao;
        vec3  H      = normalize(sunDir + V);
        vec3  spec   = u_sunColor * u_sunI * pow(max(0.0, dot(N, H)), 48.0) * shadow * 0.3;
        color = diffuse + ambient + spec;
    }

    float fogFactor = 1.0 - exp(-t * 0.007);
    color = mix(color, skyColor(rd) * 0.55, fogFactor);

    if (u_extra0.x > 0.5) {
        float tn = clamp(t / kMaxDist, 0.0, 1.0);
        vec3 dbgCol;
        if      (tn < 0.25) { float s = tn / 0.25;           dbgCol = mix(vec3(0,0,1), vec3(0,1,1), s); }
        else if (tn < 0.50) { float s = (tn - 0.25) / 0.25;  dbgCol = mix(vec3(0,1,1), vec3(0,1,0), s); }
        else if (tn < 0.75) { float s = (tn - 0.50) / 0.25;  dbgCol = mix(vec3(0,1,0), vec3(1,1,0), s); }
        else                { float s = (tn - 0.75) / 0.25;  dbgCol = mix(vec3(1,1,0), vec3(1,0,0), s); }
        fragColor = vec4(dbgCol, 1.0);
        return;
    }

    fragColor = vec4(color, 1.0);
}
)GLSL";

        // Native-path sky. Same fullscreen-triangle trick and the same #version as the ray-march
        // shaders: gl_VertexID needs GLSL 1.30+, and this context gives 3.30. Only the *mesh*
        // shaders above are 1.20, which is why they cannot use it.
    extern const char* const kOpenGLNativeSkyVS = R"GLSL(
#version 330 core
void main()
{
    float x = (gl_VertexID == 1) ?  3.0 : -1.0;
    float y = (gl_VertexID == 2) ?  3.0 : -1.0;
    gl_Position = vec4(x, y, 0.0, 1.0);
}
)GLSL";

    extern const char* const kOpenGLNativeSkyFS_Part1 = R"GLSL(
#version 330 core
out vec4 fragColor;
uniform mat4 u_invViewProjection;
uniform vec3 u_sunDir;
uniform vec3 u_sunColor;
uniform float u_sunI;
uniform float u_exposure; // RenderSettings::exposure
uniform vec2 u_renderSize;
// HDR environment cube built by Skybox::BuildNativeCubemap from the loaded panorama. It only
// exists once that asynchronous load finishes, so u_hasEnvCube gates it and ComputeSkyColor stays
// the fallback -- otherwise the first frames of every run would sample an unbound texture.
uniform samplerCube u_envCube;
uniform int u_hasEnvCube;
)GLSL";

    extern const char* const kOpenGLNativeSkyFS_Part2 = R"GLSL(
void main()
{
    vec2 uv  = gl_FragCoord.xy / u_renderSize;
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0);

    // Reverse-Z: near = 1.0, far = 0.0. The matrix is uploaded row-major and read column-major,
    // so M * v here equals mul(v, M) in the HLSL shaders.
    vec4 nearH = u_invViewProjection * vec4(ndc, 1.0, 1.0);
    vec4 farH  = u_invViewProjection * vec4(ndc, 0.0, 1.0);
    if (abs(nearH.w) < 1e-7 || abs(farH.w) < 1e-7) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 rd = normalize((farH.xyz / farH.w) - (nearH.xyz / nearH.w));
    if (u_hasEnvCube != 0) {
        // The cube holds linear HDR radiance, so it needs the same ACES + gamma the native mesh
        // shader applies to its own lighting. ComputeSkyColor below is deliberately NOT tonemapped:
        // it already returns display-ready values, and running it through the curve a second time
        // would wash the procedural sky out. Same split as NativeSky.hlsl and NativeSky_PS.hlsl.
        vec3 skyRadiance = texture(u_envCube, rd).rgb;
        vec3 x = skyRadiance * u_exposure;
        skyRadiance = clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
        skyRadiance = pow(clamp(skyRadiance, 0.0, 1.0), vec3(1.0 / 2.2));
        fragColor = vec4(skyRadiance, 1.0);
        return;
    }
    fragColor = vec4(ComputeSkyColor(rd, normalize(-u_sunDir), u_sunColor, u_sunI), 1.0);
}
)GLSL";
    // ---- Native ambient occlusion (SSAO) ----
    //
    // The canonical implementation is the D3D12 one
    // (Shaders/Effects/AmbientOcclusion/SSAO/SSAO_VS.hlsl + SSAO_PS.hlsl + SSAO_Sampling.hlsli);
    // these are its GLSL translation for the native path. D3D12 runs SSAO as a full-screen raster
    // pass, not a compute dispatch, so this backend's lack of a compute path costs nothing here.
    //
    // D3D12 reads depth and world normals out of the deferred GBuffer. The native path has no
    // GBuffer, so the prepass programs below produce the same two inputs on their own.

    // Depth + world-normal prepass for the static native mesh path, GLSL 1.20 to match the mesh
    // programs it shares a vertex layout with. Attribute 2 (a_color) is intentionally absent: the
    // prepass needs no shading, so the draw loop never enables that array.
    extern const char* const kOpenGLAoPrepassVS = R"GLSL(
#version 120
attribute vec3 a_position;
attribute vec3 a_normal;
attribute vec2 a_uv;
uniform mat4 u_modelViewProjection;
uniform mat4 u_model;
varying vec3 v_worldNormal;
varying vec2 v_uv;
void main()
{
    gl_Position = u_modelViewProjection * vec4(a_position, 1.0);
    // Row-major data read as column-major is the transpose, which makes GLSL's mat3(M) * n equal
    // the HLSL shaders' mul(n, (float3x3)M) -- the same convention u_modelViewProjection uses.
    // This is the plain world matrix, not its inverse transpose, so non-uniformly scaled instances
    // skew their normals; that matches the raster GBuffer path rather than introducing a
    // backend-specific special case.
    v_worldNormal = mat3(u_model) * a_normal;
    v_uv = a_uv;
}
)GLSL";

    // Skinned counterpart. Same up-to-4-influence blending as kOpenGLNativeSkinnedMeshVS: skinning
    // lands in model space and u_model then carries it to world space, which is where SSAO works.
    extern const char* const kOpenGLAoPrepassSkinnedVS = R"GLSL(
#version 120
attribute vec3 a_position;
attribute vec3 a_normal;
attribute vec2 a_uv;
attribute vec4 a_boneIndices;
attribute vec4 a_boneWeights;
uniform mat4 u_modelViewProjection;
uniform mat4 u_model;
uniform mat4 u_boneMatrices[128];
varying vec3 v_worldNormal;
varying vec2 v_uv;
void main()
{
    vec4 pos4 = vec4(a_position, 1.0);
    mat4 boneX = u_boneMatrices[int(a_boneIndices.x + 0.5)];
    mat4 boneY = u_boneMatrices[int(a_boneIndices.y + 0.5)];
    mat4 boneZ = u_boneMatrices[int(a_boneIndices.z + 0.5)];
    mat4 boneW = u_boneMatrices[int(a_boneIndices.w + 0.5)];
    vec4 skinnedPos =
        a_boneWeights.x * (boneX * pos4) +
        a_boneWeights.y * (boneY * pos4) +
        a_boneWeights.z * (boneZ * pos4) +
        a_boneWeights.w * (boneW * pos4);
    vec3 skinnedNormal =
        a_boneWeights.x * (mat3(boneX) * a_normal) +
        a_boneWeights.y * (mat3(boneY) * a_normal) +
        a_boneWeights.z * (mat3(boneZ) * a_normal) +
        a_boneWeights.w * (mat3(boneW) * a_normal);
    gl_Position = u_modelViewProjection * skinnedPos;
    v_worldNormal = mat3(u_model) * skinnedNormal;
    v_uv = a_uv;
}
)GLSL";

    // Shared prepass fragment stage. Writes the same encoded world normal the raster GBuffer does
    // (n * 0.5 + 0.5, decoded by SSAO_NormalDecode.hlsli's DecodeNormalFromEncoded and by the GLSL
    // DecodeNormal below), and repeats the mesh shader's cut-out alpha clip so foliage and grille
    // cards occlude by their silhouette instead of by their whole quad.
    extern const char* const kOpenGLAoPrepassFS = R"GLSL(
#version 120
uniform sampler2D u_albedoTexture;
uniform int u_hasAlbedoTexture;
varying vec3 v_worldNormal;
varying vec2 v_uv;
void main()
{
    if (u_hasAlbedoTexture != 0 && texture2D(u_albedoTexture, v_uv).a < 0.5) {
        discard;
    }
    // Guarded rather than a bare normalize(): a degenerate interpolated normal would otherwise
    // write NaN into the target and poison every sample that reads it.
    float lenSq = dot(v_worldNormal, v_worldNormal);
    vec3 n = (lenSq > 1e-12) ? v_worldNormal * inversesqrt(lenSq) : vec3(0.0, 1.0, 0.0);
    gl_FragColor = vec4(n * 0.5 + 0.5, 1.0);
}
)GLSL";

    // Full-screen triangle for the SSAO pass. Needs gl_VertexID, hence #version 330, exactly like
    // the native sky and ray-march vertex stages.
    extern const char* const kOpenGLSsaoVS = R"GLSL(
#version 330 core
out vec2 v_uv;
void main()
{
    float x = (gl_VertexID == 1) ?  3.0 : -1.0;
    float y = (gl_VertexID == 2) ?  3.0 : -1.0;
    gl_Position = vec4(x, y, 0.0, 1.0);
    // UV follows GL's bottom-left window origin, so (0,0) is the bottom-left pixel. The HLSL
    // original emits a top-left-origin UV instead; every UV<->NDC conversion below is adjusted for
    // that one difference and nothing else.
    v_uv = vec2(x, y) * 0.5 + 0.5;
}
)GLSL";

    // SSAO fragment stage: a line-for-line translation of SSAO_PS.hlsl. The cbuffer fields become
    // loose uniforms (the native path has no constant buffers) and the two texture/sampler pairs
    // become sampler2D units, but the kernel, the weights and the final visibility formula are the
    // D3D12 ones unchanged.
    extern const char* const kOpenGLSsaoFS = R"GLSL(
#version 330 core
in vec2 v_uv;
out vec4 fragColor;
uniform mat4 u_cameraPV;
uniform mat4 u_cameraInvPV;
uniform vec4 u_ssaoParams;   // x: radius, y: bias, z: intensity, w: thickness
uniform vec4 u_cameraPos;    // xyz: camera world position, w: quality index
uniform sampler2D u_depthTexture;
uniform sampler2D u_normalTexture;

const float kPi = 3.14159265;
// 32 entries, the same table as SSAO_Sampling.hlsli's g_ssaoKernel. The length is spelled out
// rather than taken from a named constant because an array constructor needs a literal size on
// the oldest GLSL profile that accepts one at all.
const vec3 kSsaoKernel[32] = vec3[32](
    vec3( 0.1340,  0.0000,  0.9910),
    vec3( 0.4640, -0.2130,  0.8600),
    vec3(-0.1820,  0.5010,  0.8470),
    vec3(-0.5570, -0.2040,  0.8060),
    vec3( 0.5410,  0.4370,  0.7190),
    vec3( 0.0420, -0.7440,  0.6670),
    vec3(-0.6810,  0.3180,  0.6600),
    vec3( 0.5170, -0.5500,  0.6560),
    vec3(-0.6510, -0.4590,  0.6050),
    vec3( 0.7600,  0.2010,  0.6180),
    vec3(-0.0680,  0.7960,  0.6010),
    vec3(-0.2740, -0.7510,  0.6000),
    vec3( 0.7280, -0.4640,  0.5050),
    vec3(-0.7820,  0.3530,  0.5150),
    vec3( 0.2680,  0.8200,  0.5060),
    vec3(-0.5590, -0.6440,  0.5220),
    vec3( 0.8890,  0.0110,  0.4580),
    vec3( 0.6610,  0.6170,  0.4270),
    vec3(-0.0720,  0.9200,  0.3850),
    vec3(-0.7070,  0.6200,  0.3400),
    vec3(-0.9540, -0.0410,  0.2980),
    vec3(-0.6120, -0.7610,  0.2140),
    vec3( 0.1130, -0.9820,  0.1510),
    vec3( 0.7740, -0.6180,  0.1360),
    vec3( 0.9840,  0.0850,  0.1560),
    vec3( 0.5430,  0.8340,  0.0970),
    vec3(-0.1860,  0.9800,  0.0740),
    vec3(-0.8220,  0.5660,  0.0610),
    vec3(-0.9980, -0.0110,  0.0620),
    vec3(-0.5610, -0.8280,  0.0210),
    vec3( 0.2150, -0.9750,  0.0520),
    vec3( 0.8320, -0.5510,  0.0560)
);

// Mirrors Shaders/Shared/Common/WorldPosReconstruction.hlsli. The only change is uv.y: that file
// flips it for D3D's top-left texture origin, while a GL target's v = 0 already sits at ndc.y = -1.
vec3 ReconstructWorldPos(vec2 uv, float depth)
{
    vec4 ndc = vec4(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, depth, 1.0);
    vec4 worldH = u_cameraInvPV * ndc;
    return worldH.xyz / max(worldH.w, 1e-6);
}

// Mirrors DecodeNormalFromEncoded in SSAO_NormalDecode.hlsli.
vec3 DecodeNormal(vec2 uv)
{
    vec3 normal = textureLod(u_normalTexture, uv, 0.0).xyz * 2.0 - 1.0;
    float lenSq = dot(normal, normal);
    if (lenSq <= 1e-5) {
        return vec3(0.0, 1.0, 0.0);
    }
    return normal * inversesqrt(lenSq);
}

float Hash12(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

vec3 BuildTangent(vec3 normal, float angle)
{
    vec3 randDir = vec3(cos(angle), sin(angle), 0.0);
    vec3 tangent = randDir - normal * dot(randDir, normal);
    float lenSq = dot(tangent, tangent);
    if (lenSq <= 1e-5) {
        tangent = (abs(normal.z) < 0.999) ? cross(normal, vec3(0.0, 0.0, 1.0))
                                          : cross(normal, vec3(0.0, 1.0, 0.0));
        lenSq = dot(tangent, tangent);
    }
    return tangent * inversesqrt(max(lenSq, 1e-6));
}

int ResolveSampleCount(int quality)
{
    return (quality == 0) ? 12 : ((quality == 1) ? 20 : 32);
}

float ResolveSampleRadius(int sampleIndex, int sampleCount)
{
    float normalized = (float(sampleIndex) + 0.5) / float(sampleCount);
    return mix(0.12, 1.0, normalized * normalized);
}

void main()
{
    float depth = textureLod(u_depthTexture, v_uv, 0.0).r;
    // Reverse-Z: far plane is depth == 0.0, so background/empty pixels satisfy depth <= 0.0.
    if (depth <= 0.0) {
        fragColor = vec4(1.0, 1.0, 1.0, 1.0);
        return;
    }

    vec3 worldPos = ReconstructWorldPos(v_uv, depth);
    vec3 normal = DecodeNormal(v_uv);
    vec3 toCamera = u_cameraPos.xyz - worldPos;
    if (dot(normal, toCamera) < 0.0) {
        normal = -normal;
    }

    float radius = max(u_ssaoParams.x, 0.01);
    float bias = max(u_ssaoParams.y, 1e-4);
    float intensity = max(u_ssaoParams.z, 0.0);
    float thickness = max(u_ssaoParams.w, 1e-4);
    int qualityIndex = clamp(int(u_cameraPos.w + 0.5), 0, 2);
    int sampleCount = ResolveSampleCount(qualityIndex);

    // gl_FragCoord.xy is the window-space pixel centre (x.5), so floor() reproduces the HLSL
    // original's floor(SV_POSITION.xy): one fixed hash per pixel, stable from frame to frame.
    float noise = Hash12(floor(gl_FragCoord.xy));
    float angle = noise * kPi * 2.0;
    vec3 tangent = BuildTangent(normal, angle);
    vec3 bitangent = normalize(cross(normal, tangent));

    float occlusion = 0.0;
    float weightSum = 0.0;

    for (int i = 0; i < sampleCount; ++i) {
        vec3 hemisphereDir = kSsaoKernel[i];
        vec3 sampleDir = tangent * hemisphereDir.x +
                         bitangent * hemisphereDir.y +
                         normal * hemisphereDir.z;
        sampleDir = normalize(sampleDir);

        float sampleRadius = radius * ResolveSampleRadius(i, sampleCount);
        vec3 samplePos = worldPos + sampleDir * sampleRadius;
        vec4 sampleClip = u_cameraPV * vec4(samplePos, 1.0);
        if (sampleClip.w <= 1e-5) {
            continue;
        }

        vec3 sampleNdc = sampleClip.xyz / sampleClip.w;
        // No Y flip, unlike the HLSL original's float2(0.5, -0.5): see ReconstructWorldPos above.
        vec2 sampleUv = sampleNdc.xy * 0.5 + 0.5;
        if (any(lessThan(sampleUv, vec2(0.0))) || any(greaterThan(sampleUv, vec2(1.0)))) {
            continue;
        }

        float sceneDepth = textureLod(u_depthTexture, sampleUv, 0.0).r;
        if (sceneDepth <= 0.0) {
            continue;
        }

        vec3 scenePos = ReconstructWorldPos(sampleUv, sceneDepth);
        vec3 sceneDelta = scenePos - worldPos;
        float sceneDistance = length(sceneDelta);
        if (sceneDistance <= 1e-5) {
            continue;
        }

        // Occluded when the visible surface along the sample's pixel ray is nearer the camera
        // than the sample point; mirrors SSAO_PS.hlsl.
        float depthDelta = length(samplePos - u_cameraPos.xyz) - length(scenePos - u_cameraPos.xyz);
        float rangeWeight = clamp(1.0 - sceneDistance / (radius * 1.5), 0.0, 1.0);
        float horizonWeight = clamp(dot(normal, sampleDir), 0.0, 1.0);

        vec3 sceneNormal = DecodeNormal(sampleUv);
        float normalWeight = clamp(dot(sceneNormal, -sampleDir), 0.0, 1.0);
        normalWeight = mix(0.35, 1.0, normalWeight);

        float occlusionWeight = smoothstep(bias, bias + thickness, depthDelta);
        occlusion += occlusionWeight * rangeWeight * horizonWeight * normalWeight;
        weightSum += max(horizonWeight, 1e-4);
    }

    float visibility = 1.0;
    if (weightSum > 0.0) {
        visibility = clamp(1.0 - (occlusion / weightSum) * intensity, 0.0, 1.0);
    }
    fragColor = vec4(visibility, visibility, visibility, 1.0);
}
)GLSL";
    // Bilateral blur over the raw SSAO, a translation of SSAO_Blur_PS.hlsl. Shares kOpenGLSsaoVS.
    // Without it the per-pixel kernel rotation in the pass above shows up as fixed-pattern noise,
    // which is why DX12, DX11 and Vulkan all run this stage too.
    extern const char* const kOpenGLSsaoBlurFS = R"GLSL(
#version 330 core
in vec2 v_uv;
out vec4 fragColor;
uniform vec4 u_screenSize;   // x: width, y: height, z: 1/width, w: 1/height
uniform sampler2D u_ssaoTexture;
uniform sampler2D u_depthTexture;
uniform sampler2D u_normalTexture;

// Mirrors DecodeNormalFromEncoded in SSAO_NormalDecode.hlsli.
vec3 DecodeNormal(vec2 uv)
{
    vec3 normal = textureLod(u_normalTexture, uv, 0.0).xyz * 2.0 - 1.0;
    float lenSq = dot(normal, normal);
    if (lenSq <= 1e-5) {
        return vec3(0.0, 1.0, 0.0);
    }
    return normal * inversesqrt(lenSq);
}

void main()
{
    vec2  texelSize   = u_screenSize.zw;
    float centerDepth = textureLod(u_depthTexture, v_uv, 0.0).r;

    // Reverse-Z: far plane is depth == 0.0, so background/empty pixels satisfy depth <= 0.0.
    if (centerDepth <= 0.0) {
        fragColor = vec4(1.0, 1.0, 1.0, 1.0);
        return;
    }

    vec3 centerNormal = DecodeNormal(v_uv);
    float kernel[5] = float[5](0.0625, 0.25, 0.375, 0.25, 0.0625);

    float result    = 0.0;
    float weightSum = 0.0;

    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec2  sampleUv     = v_uv + vec2(float(x), float(y)) * texelSize;
            float sampleDepth  = textureLod(u_depthTexture, sampleUv, 0.0).r;
            float sampleAo     = textureLod(u_ssaoTexture, sampleUv, 0.0).r;
            vec3  sampleNormal = DecodeNormal(sampleUv);

            float depthDiff   = abs(centerDepth - sampleDepth);
            float depthWeight = exp(-depthDiff * 32.0);

            float normalDiff   = 1.0 - clamp(dot(centerNormal, sampleNormal), 0.0, 1.0);
            float normalWeight = exp(-normalDiff * 24.0);

            float w = kernel[x + 2] * kernel[y + 2] * depthWeight * normalWeight;
            result += sampleAo * w;
            weightSum += w;
        }
    }

    float blurred = result / max(weightSum, 1e-4);
    fragColor = vec4(blurred, blurred, blurred, 1.0);
}
)GLSL";

    // Standalone ambient-occlusion view for SASAMI_GL_AO_DEBUG=1. Writes the AO value as grey so
    // the term can be read on its own instead of being inferred from a lit frame -- the DX11 and
    // Vulkan paths carry the same switch. Shares kOpenGLSsaoVS.
    extern const char* const kOpenGLAoDebugFS = R"GLSL(
#version 330 core
in vec2 v_uv;
out vec4 fragColor;
uniform sampler2D u_aoTexture;
void main()
{
    float ao = textureLod(u_aoTexture, v_uv, 0.0).r;
    fragColor = vec4(ao, ao, ao, 1.0);
}
)GLSL";
}
