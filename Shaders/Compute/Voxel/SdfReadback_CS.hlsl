//
// SdfReadback_CS.hlsl
// Copies an R32F 3D texture into a structured float buffer, one thread per texel, so the host can
// compare it with the CPU SDF (GpuSdfTextureReadback, `voxel.sdf check`). Load does no filtering or
// conversion on R32F, so every value must come back bit-identical.
//
// Bindings (one descriptor table per slot, see GpuSdfTextureReadback):
//   t0 : g_texture  the uploaded SDF
//   t1 : g_params   single ReadbackParams element
//   u0 : g_output   width*height*depth floats, x + y*w + z*w*h
//

struct ReadbackParams
{
    uint3 dim;
    uint  pad;
};

Texture3D<float> g_texture : register(t0);
StructuredBuffer<ReadbackParams> g_params : register(t1);
RWStructuredBuffer<float> g_output : register(u0);

[numthreads(4, 4, 4)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint3 dim = g_params[0].dim;
    if (any(dtid >= dim))
    {
        return;
    }
    g_output[dtid.x + dim.x * (dtid.y + dim.y * dtid.z)] = g_texture.Load(int4(dtid, 0));
}
