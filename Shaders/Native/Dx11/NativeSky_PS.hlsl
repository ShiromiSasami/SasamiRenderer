// NativeSky_PS.hlsl -- the renderer's own analytic sky, drawn as the background of the native path.
//
// The native backends used to clear to a flat grey and draw nothing else behind the geometry, so
// every scene sat against a uniform slab of colour. This draws the same sky the D3D12 path gets.
//
// The sky function is included rather than reimplemented: ProceduralSky_PS, the SWRT miss path and
// the water's environment reflection all evaluate this one function, so a scene cannot end up with
// two different skies depending on which surface you look at.
// Relative to this file, not to the shader root: D3D_COMPILE_STANDARD_FILE_INCLUDE resolves an
// include against the directory of the file doing the including, so a root-relative path fails
// to open (it did, silently, until the log was read).
#include "../../Effects/Sky/ProceduralSky/ProceduralSky.hlsli"
#include "../NativeTonemap.hlsli"

cbuffer NativeSkyCB : register(b0)
{
    row_major float4x4 u_invViewProjection;
    float4             u_cameraPos;      // xyz = camera world position
    float4             u_sunDirection;   // xyz = light-to-surface direction, w = intensity
    float4             u_sunColor;       // rgb = sun colour, w = 1 when an HDR environment cube is bound
    float4             u_exposureParams; // x = exposure (RenderSettings::exposure)
};

// The HDR environment cube built by Skybox::BuildNativeCubemap from the loaded panorama. It only
// exists after that asynchronous load finishes, so u_sunColor.w gates it and ComputeSkyColor()
// stays the fallback -- otherwise the first frames of every run would sample an unbound texture.
TextureCube  EnvCube    : register(t0);
SamplerState EnvSampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

float4 PSMain(PSInput input) : SV_TARGET
{
    // Reconstruct the view ray. Reverse-Z here as everywhere in this renderer: near = 1.0 and
    // far = 0.0, so the near point takes z = 1 and the far point z = 0.
    const float2 ndc = float2(input.uv.x * 2.0f - 1.0f, 1.0f - input.uv.y * 2.0f);
    const float4 nearH = mul(float4(ndc, 1.0f, 1.0f), u_invViewProjection);
    const float4 farH  = mul(float4(ndc, 0.0f, 1.0f), u_invViewProjection);
    if (abs(nearH.w) < 1e-7f || abs(farH.w) < 1e-7f) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    const float3 rayDir = normalize((farH.xyz / farH.w) - (nearH.xyz / nearH.w));
    const float3 toSun = normalize(-u_sunDirection.xyz);

    if (u_sunColor.w > 0.5f) {
        // The cube holds linear HDR radiance, so it needs the same ACES + gamma the native mesh
        // shader applies to its own lighting. ComputeSkyColor below is deliberately NOT tonemapped:
        // it already returns display-ready values, and running it through the curve a second time
        // would wash the procedural sky out.
        float3 skyRadiance = EnvCube.SampleLevel(EnvSampler, rayDir, 0.0f).rgb;
        skyRadiance = NativeTonemap(skyRadiance, u_exposureParams.x);
        skyRadiance = pow(saturate(skyRadiance), 1.0f / 2.2f);
        return float4(skyRadiance, 1.0f);
    }

    const float3 sky = ComputeSkyColor(rayDir, toSun, u_sunColor.rgb, u_sunDirection.w);
    return float4(sky, 1.0f);
}
