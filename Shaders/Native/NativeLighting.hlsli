#pragma once

// Surface lighting composition shared by the DirectX 11 and Vulkan native mesh pixel shaders
// (static and skinned), so the two backends evaluate the same model instead of two hand-kept
// copies. The OpenGL backend's GLSL (OpenGLShaderSources.cpp) transcribes the same functions.
// Must stay compilable by both FXC (SM5, DX11) and DXC (SPIR-V, Vulkan): no SM6-only intrinsics.
//
// Mirrors Shaders/Raster/Lighting/Deferred/DeferredLighting_PS.hlsl:
//   rawAo = saturate(materialAo * runtimeAo); ao = lerp(aoFloor, 1, rawAo)
//   direct *= directionalVisibility * dirMicroShadow (Chan 2018)
// Requires NativePbr.hlsli and Shared/Common/MicroShadowing.hlsli to be included first.

// Lower bound ambient occlusion multiplier, mirroring DX12's u_shadowParams.w -- fully occluded
// texels are dimmed to this fraction of the ambient rather than going fully black.
static const float kNativeAoFloor = 0.25f;

// Strength of the direct-light micro-shadowing term, mirroring DX12's u_debugParams.y
// (LightSystemShadow.cpp feeds it RenderSettings::aoDirectLightingStrength, whose default is
// 0.5). Like kNativeAoFloor this is a fixed stand-in rather than the live setting:
// RhiBackendMeshFrameDesc carries no field for it, so the native paths cannot see it yet.
static const float kNativeAoDirectStrength = 0.5f;

// materialAo is a property of the mesh's own material and always applies; runtimeAo is the
// screen-space effect and is 1.0 whenever no AO texture was produced this frame.
float NativeResolveAo(float materialAo, float runtimeAo)
{
    float rawAo = saturate(materialAo * runtimeAo);
    return lerp(kNativeAoFloor, 1.0f, rawAo);
}

// Analytic sky/ground split used until the skybox-built ambient cube arrives (it loads
// asynchronously) and for any scene without one.
float3 NativeHemisphereAmbient(float3 n)
{
    float skyWeight = saturate(n.y * 0.5 + 0.5);
    float3 skyColor = float3(0.35, 0.45, 0.60);
    float3 groundColor = float3(0.12, 0.10, 0.08);
    return lerp(groundColor, skyColor, skyWeight);
}

// Pre-tonemap outgoing radiance for one directional light plus ambient and emissive.
// ambientRadiance is the cube sample (or NativeHemisphereAmbient) and is only used when
// ambientEnabled (the IBL feature flag) is set.
float3 NativeShadeSurface(float3 albedo, float metallic, float roughness, float3 n, float3 v,
                          float3 l, float3 lightColor, float lightIntensity, float shadowTerm,
                          float ao, bool ambientEnabled, float3 ambientRadiance, float3 emissive)
{
    float3 direct = EvaluatePbr(albedo, metallic, roughness, n, v, l, lightColor, lightIntensity) * shadowTerm;
    // Small-scale occlusion the shadow map cannot resolve (Chan 2018, "Material Advances in
    // Call of Duty: WWII"), applied on top of shadow-map visibility. A no-op wherever ao == 1.
    direct *= ComputeMicroShadowing(saturate(dot(n, l)), ao, kNativeAoDirectStrength);

    float3 ambient = float3(0.0, 0.0, 0.0);
    if (ambientEnabled) {
        ambient = albedo * ambientRadiance * ao;
    }

    float3 lit = direct + ambient;
    lit += emissive * ao;
    return lit;
}
