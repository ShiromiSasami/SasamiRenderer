// Anime "SDF face shadow" (Genshin-style; akasaki1211/sdf_shadow_threshold_map, ChiliMilk/URP_Toon):
// the face ignores its normals and takes the directional-light term from a threshold map instead.
// The light is projected onto the head's forward/right plane; lightParam01 is 1 with the light in
// front and 0 behind, and the map is mirrored when the light comes from the other side.
// Lighting reads the lit factor from SV_TARGET4.r (selector 0.375 = toon face, see IsToonFaceShading).
// Shared by OpaqueGBuffer_PS and both mesh-shader GBuffer PSs so the paths stay identical.
#ifndef SASAMI_TOON_FACE_SHADOW_INCLUDED
#define SASAMI_TOON_FACE_SHADOW_INCLUDED

// toLight: unit vector towards the light. Returns the lit factor in [0, 1].
float ToonFaceShadowLit(float3 toLight, float3 faceForward, float3 faceRight, float softness, float2 uv,
                        Texture2D faceMap, SamplerState faceSampler)
{
    const float3 F = normalize(faceForward);
    const float3 R = normalize(faceRight);
    const float2 planar = float2(dot(toLight, F), dot(toLight, R));
    const float planarLength = length(planar);
    const float cosAngle = planarLength > 1e-5 ? planar.x / planarLength : 1.0;
    const float lightParam01 = 1.0 - acos(clamp(cosAngle, -1.0, 1.0)) / 3.14159265;
    const float2 faceUv = float2(planar.y < 0.0 ? 1.0 - uv.x : uv.x, uv.y);
    const float threshold = faceMap.Sample(faceSampler, faceUv).r;
    const float edge = max(softness, 1e-4);
    return threshold >= 0.999 ? 0.0 : smoothstep(threshold - edge, threshold + edge, lightParam01);
}

#endif
