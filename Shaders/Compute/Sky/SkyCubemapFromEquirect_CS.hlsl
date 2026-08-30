// Generates cubemap mip 0 from an equirectangular HDR source.
//
// This is the GPU port of RendererMathUtility::GenerateSkyCubemapFromEquirect.
// The CPU version cost 9.93 s for a 2048^2 x 6 face cube (measured in a Debug
// build via ScopedPerfTimer in Skybox::UploadHdrSkyboxTexture): ~25 M texels,
// each doing an atan2/acos plus a hand-written bilinear fetch. The mip chain was
// only ~25% of that, so the base-face conversion is what had to move to the GPU.
//
// The texel-center direction math and the direction->UV mapping below are kept
// bit-for-bit identical to the CPU version so the generated sky is unchanged.
// The CPU code wrapped X and clamped Y around its manual bilinear taps; that is
// reproduced by the sampler state (AddressU = WRAP, AddressV = CLAMP, linear),
// which also applies the same half-texel offset in hardware.

cbuffer SkyCubemapGenCB : register(b0)
{
    uint g_faceSize;
    uint g_pad0;
    uint g_pad1;
    uint g_pad2;
};

Texture2D<float4>        g_equirect        : register(t0);
SamplerState             g_equirectSampler : register(s0);
RWTexture2DArray<float4> g_cubeMip0        : register(u0);

static const float kPi = 3.1415926535f;

// Cubemap texel center -> normalized direction. s,t are face-local in [-1,1].
// Face order is the D3D12 array-slice order: +X, -X, +Y, -Y, +Z, -Z.
float3 CubemapTexelDirection(uint face, uint x, uint y, uint faceSize)
{
    const float s = ((float(x) + 0.5f) / float(faceSize)) * 2.0f - 1.0f;
    const float t = ((float(y) + 0.5f) / float(faceSize)) * 2.0f - 1.0f;

    float3 dir;
    switch (face) {
    case 0:  dir = float3( 1.0f,   -t,   -s); break; // +X
    case 1:  dir = float3(-1.0f,   -t,    s); break; // -X
    case 2:  dir = float3(    s, 1.0f,    t); break; // +Y
    case 3:  dir = float3(    s,-1.0f,   -t); break; // -Y
    case 4:  dir = float3(    s,   -t, 1.0f); break; // +Z
    default: dir = float3(   -s,   -t,-1.0f); break; // -Z
    }
    return normalize(dir);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= g_faceSize || tid.y >= g_faceSize) {
        return;
    }

    const uint face = tid.z;
    const float3 dir = CubemapTexelDirection(face, tid.x, tid.y, g_faceSize);

    // u = atan2(z, x) / (2*pi) + 0.5, v = acos(y) / pi
    const float u = atan2(dir.z, dir.x) / (2.0f * kPi) + 0.5f;
    const float v = acos(clamp(dir.y, -1.0f, 1.0f)) / kPi;

    const float3 color = g_equirect.SampleLevel(g_equirectSampler, float2(u, v), 0.0f).rgb;
    g_cubeMip0[uint3(tid.x, tid.y, face)] = float4(color, 1.0f);
}
