#ifndef SASAMI_PBR_IBL_HLSLI
#define SASAMI_PBR_IBL_HLSLI

float3 EvaluateDiffuseIrradianceFromSh(float3 n)
{
    const float x = n.x;
    const float y = n.y;
    const float z = n.z;

    // Real SH basis (L2), multiplied by cosine-kernel convolution factor per band.
    const float b0 = 0.282095 * 3.14159265;
    const float b1 = (0.488603 * y) * 2.09439510;
    const float b2 = (0.488603 * z) * 2.09439510;
    const float b3 = (0.488603 * x) * 2.09439510;
    const float b4 = (1.092548 * x * y) * 0.78539816;
    const float b5 = (1.092548 * y * z) * 0.78539816;
    const float b6 = (0.315392 * (3.0 * z * z - 1.0)) * 0.78539816;
    const float b7 = (1.092548 * x * z) * 0.78539816;
    const float b8 = (0.546274 * (x * x - y * y)) * 0.78539816;

    float3 irradiance =
        u_diffuseSh[0].rgb * b0 +
        u_diffuseSh[1].rgb * b1 +
        u_diffuseSh[2].rgb * b2 +
        u_diffuseSh[3].rgb * b3 +
        u_diffuseSh[4].rgb * b4 +
        u_diffuseSh[5].rgb * b5 +
        u_diffuseSh[6].rgb * b6 +
        u_diffuseSh[7].rgb * b7 +
        u_diffuseSh[8].rgb * b8;

    return max(irradiance, 0.0);
}

#endif // SASAMI_PBR_IBL_HLSLI
