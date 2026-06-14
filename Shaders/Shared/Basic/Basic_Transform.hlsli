#ifndef SASAMI_BASIC_TRANSFORM_HLSLI
#define SASAMI_BASIC_TRANSFORM_HLSLI

// Basic forward shader transform helpers.


float3x3 ComputeWorldToObject3x3()
{
    const float a = u_world[0][0];
    const float b = u_world[0][1];
    const float c = u_world[0][2];
    const float d = u_world[1][0];
    const float e = u_world[1][1];
    const float f = u_world[1][2];
    const float g = u_world[2][0];
    const float h = u_world[2][1];
    const float i = u_world[2][2];

    const float cofactor00 = e * i - f * h;
    const float cofactor01 = c * h - b * i;
    const float cofactor02 = b * f - c * e;
    const float cofactor10 = f * g - d * i;
    const float cofactor11 = a * i - c * g;
    const float cofactor12 = c * d - a * f;
    const float cofactor20 = d * h - e * g;
    const float cofactor21 = b * g - a * h;
    const float cofactor22 = a * e - b * d;

    const float det = a * cofactor00 + b * cofactor10 + c * cofactor20;
    if (abs(det) <= 1e-8) {
        return float3x3(
            1.0, 0.0, 0.0,
            0.0, 1.0, 0.0,
            0.0, 0.0, 1.0);
    }

    const float invDet = 1.0 / det;
    return float3x3(
        cofactor00 * invDet, cofactor01 * invDet, cofactor02 * invDet,
        cofactor10 * invDet, cofactor11 * invDet, cofactor12 * invDet,
        cofactor20 * invDet, cofactor21 * invDet, cofactor22 * invDet);
}

#endif // SASAMI_BASIC_TRANSFORM_HLSLI
