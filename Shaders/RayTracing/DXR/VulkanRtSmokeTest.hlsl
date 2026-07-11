// VulkanRtSmokeTest.hlsl
// Minimal ray-tracing library used by VulkanGraphicsDevice::RunRayTracingSmokeTest
// to validate the Vulkan HW ray-tracing RHI path (pipeline + SBT + dispatch).
//
// Compiled to SPIR-V with DXC (-T lib_6_6 -spirv -fspv-target-env=vulkan1.2).
// Explicit [[vk::binding]] slots are used (not ResourceDescriptorHeap) so the
// binding model matches the fixed descriptor set layout that
// CreateRhiRayTracingPipeline builds: binding 0 = acceleration structure,
// binding 1 = storage image.
//
// Each ray is shot along +Z at a single triangle placed in the z = 1 plane.
// Pixels whose ray hits the triangle are written red; misses are written blue.

[[vk::binding(0, 0)]] RaytracingAccelerationStructure Scene  : register(t0);
[[vk::binding(1, 0)]] RWTexture2D<float4>             Output : register(u0);

struct Payload
{
    float3 color;
};

[shader("raygeneration")]
void RayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dim   = DispatchRaysDimensions().xy;

    // Map pixel to [-1, 1] in the z = 0 plane and shoot straight along +Z.
    const float2 uv = (float2(pixel) + 0.5f) / float2(dim);

    RayDesc ray;
    ray.Origin    = float3(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, 0.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin      = 0.001f;
    ray.TMax      = 100.0f;

    Payload payload;
    payload.color = float3(0.0f, 0.0f, 0.0f);
    TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

    Output[pixel] = float4(payload.color, 1.0f);
}

[shader("miss")]
void Miss(inout Payload payload)
{
    payload.color = float3(0.0f, 0.0f, 1.0f);  // blue = ray missed the triangle
}

[shader("closesthit")]
void ClosestHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.color = float3(1.0f, 0.0f, 0.0f);  // red = ray hit the triangle
}
