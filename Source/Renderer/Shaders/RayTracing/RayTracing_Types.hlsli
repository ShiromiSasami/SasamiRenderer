#ifndef SASAMI_RAYTRACING_TYPES_HLSLI
#define SASAMI_RAYTRACING_TYPES_HLSLI

struct VertexData
{
    float3 position;
    float3 normal;
    float4 color;
    float2 uv;
};

struct MaterialData
{
    int albedoDescriptorIndex;
    int occlusionDescriptorIndex;
    float metallic;
    float roughness;
    float4 baseColor;
    float4 emissiveOcclusionStrength;
    float4 transmissionParams; // x: transmission, y: ior, z: shell strength, w: thickness
    float4 volumeParams;       // rgb: attenuation color, w: attenuation distance
};

struct InstanceData
{
    uint vertexOffset;
    uint indexOffset;
    uint materialIndex;
    uint padding;
};

struct PointLightData
{
    float4 posRange;
    float4 colorIntensity;
};

struct SpotLightData
{
    float4 posRange;
    float4 dirCosInner;
    float4 colorIntensity;
    float4 params;
};

static const uint MAX_POINT_LIGHTS = 16;
static const uint MAX_SPOT_LIGHTS = 16;
static const uint INSTANCE_MASK_RADIANCE = 0xFFu;
static const uint INSTANCE_MASK_OPAQUE_SHADOW = 0xFEu;

struct FrameConstants
{
    uint renderWidth;
    uint renderHeight;
    uint outputWidth;
    uint outputHeight;
    uint outputDescriptorIndex;
    uint vertexDescriptorIndex;
    uint indexDescriptorIndex;
    uint materialDescriptorIndex;
    uint instanceDescriptorIndex;
    uint pointLightCount;
    uint spotLightCount;
    uint pointLightBudget;
    uint spotLightBudget;
    uint qualityTier;
    uint debugView;
    uint flags;
    uint maxBounceCount;
    float dynamicResolutionScale;
    float2 padding;
    float4 cameraPosition;
    row_major float4x4 inverseViewProjection;
    float4 directionalLightDirection;
    float4 directionalLightColorIntensity;
    float4 directionalLightMarkerParams;
    PointLightData pointLights[MAX_POINT_LIGHTS];
    SpotLightData spotLights[MAX_SPOT_LIGHTS];
};

struct RadiancePayload
{
    float3 color;
    uint hit;
    uint bounceIndex;
};

struct ShadowPayload
{
    uint occluded;
};

struct Attributes
{
    float2 barycentrics : SV_Barycentrics;
};

#endif // SASAMI_RAYTRACING_TYPES_HLSLI
