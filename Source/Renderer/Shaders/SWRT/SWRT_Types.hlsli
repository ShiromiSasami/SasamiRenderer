#ifndef SASAMI_SWRT_TYPES_HLSLI
#define SASAMI_SWRT_TYPES_HLSLI

// Shared BVH, mesh, instance, TLAS, and material structures.

struct BvhNode
{
    float3 bMin;
    int    leftChild;    // <0 竊・leaf: firstTri = ~leftChild
    float3 bMax;
    int    rightOrCount; // leaf: count; interior: right child index
};

// Triangle data (Mﾃｶller窶典rumbore precomputed form + normals/uvs for shading)
struct GpuTriangle
{
    float3 p0;
    float  pad0;
    float3 edge1;
    float  pad1;
    float3 edge2;
    float  pad2;
    float3 n0;
    float  pad3;
    float3 n1;
    float  pad4;
    float3 n2;
    float  pad5;
    float2 uv0;
    float2 uv1;
    float2 uv2;
    float2 pad6;
};

// Per-mesh offset info
struct GpuMeshInfo
{
    uint nodeOffset;     // first BvhNode index for this mesh
    uint triOffset;      // first GpuTriangle index for this mesh
    uint pad0;
    uint pad1;
};

// Per-instance data
struct GpuInstanceInfo
{
    uint meshIndex;
    uint materialIndex;
    uint pad0;
    uint pad1;
    float4x4 world;
    float4x4 invWorld;
    float3 worldBoundsMin;
    float  pad2;
    float3 worldBoundsMax;
    float  pad3;
};

// TLAS node (same 32-byte layout as BvhNode, references instances)
struct TlasNode
{
    float3 bMin;
    int    leftChild;    // <0 竊・leaf: firstInstance = ~leftChild
    float3 bMax;
    int    rightOrCount; // leaf: instanceCount; interior: right child
};

// Per-material data (simplified 窶・no texture descriptor for v1)
struct GpuMaterial
{
    float4 baseColor;   // rgba
    float  roughness;
    float  metallic;
    float  transmission;
    float  ior;
    float3 specularColor;
    float  workflow;    // 0=metallic-roughness, 1=specular-glossiness
    float3 emissive;
    float  occlusionStrength;
};

#endif // SASAMI_SWRT_TYPES_HLSLI
