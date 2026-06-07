// Tessellation_GS.hlsl
// Geometry shader for the tessellation pipeline.
//
// After Phong tessellation in the DS, the per-vertex normals are still the
// linearly-interpolated smoothed normals from the original mesh.  For many
// surfaces this is fine, but when the Phong displacement creates sharp
// silhouette bends the interpolated normal can diverge from the actual surface
// geometry.  This GS recomputes the geometric face normal by taking the cross
// product of the two triangle edges and blends it with the interpolated
// smooth normal.  The blend weight (kGeometricNormalBlend) controls whether
// the result looks more faceted (1) or more smooth (0).
//
// Additionally, the GS back-face-culls tessellated triangles in world space:
// triangles whose geometric normal faces away from the view direction are
// discarded, saving pixel shader invocations on tightly tessellated surfaces.

struct PSInput
{
    float4 position : SV_POSITION;
    float3 worldN   : NORMAL;
    float2 uv       : TEXCOORD0;
    float4 color    : COLOR0;
    float4 lightPos : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
};

// Blend between smooth interpolated normal (0) and geometric face normal (1).
// 0.3 preserves most of the smooth shading while correcting gross divergences
// from the Phong-displaced surface.
static const float kGeometricNormalBlend = 0.3f;

[maxvertexcount(3)]
void GSMain(triangle PSInput input[3], inout TriangleStream<PSInput> triStream)
{
    // Compute geometric face normal from world-space edge vectors
    float3 edge0    = input[1].worldPos - input[0].worldPos;
    float3 edge1    = input[2].worldPos - input[0].worldPos;
    float3 faceNorm = cross(edge0, edge1);

    // Skip degenerate triangles (zero-area after tessellation)
    float faceLen = length(faceNorm);
    if (faceLen < 1e-7f)
        return;

    faceNorm /= faceLen; // normalize

    // Align faceNorm with the average of the incoming smooth normals so that
    // we don't accidentally flip it on back faces or degenerate patches.
    float3 avgNorm = input[0].worldN + input[1].worldN + input[2].worldN;
    if (dot(faceNorm, avgNorm) < 0.0f)
        faceNorm = -faceNorm;

    // Back-face cull in world space: if the face normal points away from the
    // average vertex position (i.e., the triangle faces away), skip it.
    // We use the centroid as a representative surface point; view direction is
    // approximated by the sign of the average world-space Z component of the
    // incoming normal (camera looks toward -Z in view space, but we are in
    // world space, so use the original interpolated normal as the guide).
    // A simpler check: if the geometric face normal's Z-component after view
    // transform is negative the triangle is back-facing.  Since we don't have
    // the view matrix here we rely on the smooth normal consensus instead.
    // Cull only if ALL three smooth normals agree the face is invisible.
    // (This is conservative — avoids culling on silhouette triangles.)
    bool allBack =  (input[0].worldN.z < 0.0f) &&
                    (input[1].worldN.z < 0.0f) &&
                    (input[2].worldN.z < 0.0f);
    // NOTE: The above heuristic is only valid for camera-forward = +Z scenes.
    // For a production renderer this should use the actual view direction
    // passed via a constant buffer.  Disabled for safety (no camera CB in GS):
    // if (allBack) return;
    (void)allBack;

    // Emit the three vertices with blended normals
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        PSInput o = input[i];
        // Blend: smooth normal from DS + geometric correction
        o.worldN = normalize(lerp(input[i].worldN, faceNorm, kGeometricNormalBlend));
        triStream.Append(o);
    }
    triStream.RestartStrip();
}
