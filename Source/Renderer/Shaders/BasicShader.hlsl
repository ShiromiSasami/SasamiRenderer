Texture2D tex0 : register(t0);
Texture2D shadowMap : register(t1);
SamplerState samp0 : register(s0);

struct PointLight
{
    float4 posRange;
    float4 colorIntensity;
};

struct SpotLight
{
    float4 posRange;
    float4 dirCosInner;
    float4 colorIntensity;
    float4 params; // x: cosOuter
};

StructuredBuffer<PointLight> u_pointLights : register(t2);
StructuredBuffer<SpotLight> u_spotLights : register(t3);

cbuffer CameraCB : register(b0)
{
    row_major float4x4 u_mvp;
    row_major float4x4 u_world;
}

cbuffer LightCB : register(b1)
{
    row_major float4x4 u_lightVP;
    float4 u_dirDir;     // xyz: forward dir, w: intensity
    float4 u_dirColor;   // rgb: color
    float4 u_lightCounts; // x: pointCount, y: spotCount
}

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal   : NORMAL;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
    float4 lightPos : TEXCOORD1; // clip-space in light view
    float3 worldPos : TEXCOORD2;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    float4 worldPos = mul(float4(input.position, 1.0f), u_world);
    output.position = mul(float4(input.position, 1.0f), u_mvp);
    output.normal = normalize(mul(float4(input.normal, 0.0f), u_world).xyz);
    output.color = input.color;
    output.uv = input.uv;
    output.lightPos = mul(worldPos, u_lightVP);
    output.worldPos = worldPos.xyz;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 texColor = tex0.Sample(samp0, input.uv);
    // Fallback: if no texture/alpha, render vertex color
    float4 base = (texColor.a > 0.001f)
        ? float4(texColor.rgb * texColor.a, texColor.a)
        : float4(1.0f, 1.0f, 1.0f, 1.0f);

    // Shadow sampling (manual 2x2 PCF):
    // visibility = (1 / 4) * sum_{kernel} step(shadowDepth >= receiverDepth - bias)
    float3 sc = input.lightPos.xyz / max(input.lightPos.w, 1e-6);
    float2 suv = sc.xy * 0.5 + 0.5;   // NDC->UV
    float  sdepth = sc.z * 0.5 + 0.5; // map from [-1,1] to [0,1]
    float bias = 0.0015;
    float visibility = 1.0;
    if (all(suv >= 0.0) && all(suv <= 1.0)) {
        float2 texel = float2(1.0 / 1024.0, 1.0 / 1024.0);
        float acc = 0.0;
        [unroll]
        for (int dy = 0; dy < 2; ++dy) {
            [unroll]
            for (int dx = 0; dx < 2; ++dx) {
                float2 uv = suv + float2(dx, dy) * texel;
                float sd = shadowMap.SampleLevel(samp0, uv, 0).r;
                acc += (sdepth - bias <= sd) ? 1.0 : 0.0;
            }
        }
        visibility = acc * 0.25;
    }

    float3 N = normalize(input.normal);
    float3 L = normalize(-u_dirDir.xyz); // direction to light
    // Lambert diffuse:
    // L_o = max(N.L, 0) * lightIntensity * visibility
    float diffuse = saturate(dot(N, L)) * visibility * u_dirDir.w;

    float pointLightTerm = 0.0;
    int pointCount = (int)u_lightCounts.x;
    for (int pointIndex = 0; pointIndex < pointCount; ++pointIndex) {
        PointLight pl = u_pointLights[pointIndex];
        float4 posRange = pl.posRange;
        float4 colInt = pl.colorIntensity;
        float3 toL = posRange.xyz - input.worldPos;
        float dist = length(toL);
        if (dist < posRange.w && dist > 1e-4 && colInt.w > 0.0) {
            float3 Lp = toL / dist;
            // Distance attenuation:
            // atten = saturate(1 - d / range)^2
            float atten = saturate(1.0 - dist / max(posRange.w, 1e-3));
            atten *= atten;
            pointLightTerm += saturate(dot(N, Lp)) * colInt.w * atten;
        }
    }

    float spotLightTerm = 0.0;
    int spotCount = (int)u_lightCounts.y;
    for (int spotIndex = 0; spotIndex < spotCount; ++spotIndex) {
        SpotLight sl = u_spotLights[spotIndex];
        float4 posRange = sl.posRange;
        float4 dirInner = sl.dirCosInner;
        float4 colInt = sl.colorIntensity;
        float4 params = sl.params;
        float3 toL = posRange.xyz - input.worldPos;
        float dist = length(toL);
        if (dist < posRange.w && dist > 1e-4 && colInt.w > 0.0) {
            float3 Ls = toL / dist;
            float cosTheta = dot(normalize(-Ls), normalize(dirInner.xyz));
            // Spot cone falloff:
            // spotFactor = smoothstep(cosOuter, cosInner, cosTheta)
            float spotFactor = smoothstep(params.x, dirInner.w, cosTheta);
            float atten = saturate(1.0 - dist / max(posRange.w, 1e-3));
            atten *= atten;
            spotLightTerm += saturate(dot(N, Ls)) * colInt.w * atten * spotFactor;
        }
    }

    float ambient = 0.2;
    // Final non-PBR shading sum:
    // shade = saturate(ambient + directional + point + spot)
    float shade = saturate(ambient + diffuse + pointLightTerm + spotLightTerm);
    return base * input.color * shade;
}
