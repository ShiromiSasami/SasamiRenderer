// DirectX 11 native mesh pixel shader. Replaces the embedded kDx11MeshPixelShader with a real
// Cook-Torrance PBR evaluation (see NativePbr.hlsli for the BRDF and its citation) plus PCF
// shadowing (see NativeShadow.hlsli).
#include "NativeMeshCommon.hlsli"
#include "../NativePbr.hlsli"
#include "../NativeShadow.hlsli"
#include "../NativeTonemap.hlsli"
// Chan 2018 micro-shadowing, shared verbatim with the D3D12 lighting passes. Reached by the
// shader-root branch of Dx11ShaderIncludeHandler, the same way the other shared headers are.
#include "Shared/Common/MicroShadowing.hlsli"
// AO floor, hemisphere fallback and the lighting composition, shared with Vulkan's NativeMesh.hlsl.
#include "../NativeLighting.hlsli"

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 texColor = (u_featureFlags.x > 0.5) ? AlbedoTex.Sample(LinearWrap, input.uv) : float4(1.0, 1.0, 1.0, 1.0);
    // Cut-out geometry -- fences, foliage cards, grilles -- stores its silhouette in the albedo
    // alpha and has no material flag anywhere in this renderer to announce itself. Without this
    // clip the whole quad is drawn and paints over what is behind it, which reads as geometry
    // popping in front of walls it should be hidden by. Only the sampled alpha is tested, never
    // u_baseColor.a, so an opaque material tinted with a low alpha is left alone.
    if (u_featureFlags.x > 0.5 && texColor.a < 0.5) {
        discard;
    }

    float3 n = normalize(input.worldN);
    float3 v = normalize(u_cameraPosMetallic.xyz - input.worldPos);
    float3 l = normalize(-u_lightDirIntensity.xyz);

    float roughness = saturate(u_emissiveRoughness.w);
    float metallic = saturate(u_cameraPosMetallic.w);
    float3 albedo = input.color.rgb * texColor.rgb;

    float shadowTerm = (u_featureFlags.w > 0.5)
        ? SampleShadow(ShadowMap, ShadowCmp, input.worldPos, n, u_lightViewProjection, u_shadowParams)
        : 1.0;

    // materialAo does not check u_featureFlags.z (SSAO): it is a property of the mesh's own
    // material, not a toggleable screen-space effect. runtimeAo comes from Dx11SsaoGenerator and
    // stays 1.0 when SSAO is off or no AO texture was produced this frame.
    float materialAo = OcclusionTex.Sample(LinearWrap, input.uv).r;
    float runtimeAo = 1.0f;
    if (u_featureFlags.z > 0.5) {
        // SV_POSITION in a pixel shader is in pixels, so scaling by the inverse render size gives
        // the screen UV the full-resolution AO texture is addressed by.
        float2 screenUv = input.position.xy * u_featureFlags2.zw;
        runtimeAo = RuntimeAOTex.SampleLevel(LinearWrap, screenUv, 0).r;
    }
    float ao = NativeResolveAo(materialAo, runtimeAo);

    // Prefers the real environment cube built by Skybox::BuildNativeCubemap once one is bound
    // (it arrives asynchronously), otherwise the analytic hemisphere.
    bool ambientEnabled = u_featureFlags.y > 0.5;
    float3 hemi = float3(0.0, 0.0, 0.0);
    if (ambientEnabled) {
        hemi = (u_featureFlags2.y > 0.5) ? AmbientCube.Sample(LinearWrap, n).rgb : NativeHemisphereAmbient(n);
    }

    float3 lit = NativeShadeSurface(albedo, metallic, roughness, n, v, l, u_lightColor.rgb,
                                    u_lightDirIntensity.w, shadowTerm, ao, ambientEnabled, hemi,
                                    u_emissiveRoughness.rgb);

    if (u_featureFlags2.x > 0.5) {
        lit = NativeTonemap(lit, u_exposureParams.x);
        lit = pow(saturate(lit), 1.0 / 2.2);
    }

    return float4(saturate(lit), input.color.a * texColor.a);
}
