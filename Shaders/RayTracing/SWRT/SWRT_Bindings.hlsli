#ifndef SASAMI_SWRT_BINDINGS_HLSLI
#define SASAMI_SWRT_BINDINGS_HLSLI

// Shared SWRT resource bindings.

StructuredBuffer<BvhNode>       g_bvhNodes    : register(t0);
StructuredBuffer<GpuTriangle>   g_triangles   : register(t1);
StructuredBuffer<GpuMeshInfo>   g_meshInfos   : register(t2);
StructuredBuffer<GpuInstanceInfo> g_instances : register(t3);
StructuredBuffer<TlasNode>      g_tlasNodes   : register(t4);
StructuredBuffer<GpuMaterial>   g_materials   : register(t5);

#endif // SASAMI_SWRT_BINDINGS_HLSLI
