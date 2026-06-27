// VulkanGraphicsDevice_RayTracing.cpp
// Vulkan ray tracing backend — stubs only.
// Full implementation requires VK_KHR_ray_tracing_pipeline and
// VK_KHR_acceleration_structure extensions (see VulkanGraphicsDevice.h).

#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"

namespace SasamiRenderer
{
#if RHI_VULKAN

    bool VulkanGraphicsDevice::BuildRhiBlases(
        const RhiBlasDesc*, uint32_t, RhiAccelerationStructureHandle*)
    {
        return false;
    }

    RhiAccelerationStructureHandle VulkanGraphicsDevice::BuildRhiTlas(const RhiTlasDesc&)
    {
        return {};
    }

    bool VulkanGraphicsDevice::DestroyRhiAccelerationStructure(RhiAccelerationStructureHandle)
    {
        return false;
    }

    RhiGpuAddress VulkanGraphicsDevice::GetRhiAccelerationStructureGpuAddress(
        RhiAccelerationStructureHandle)
    {
        return 0;
    }

    bool VulkanGraphicsDevice::CreateRhiAccelerationStructureSrv(
        RhiAccelerationStructureHandle, RhiCpuDescriptorHandle)
    {
        return false;
    }

    RhiRayTracingPipelineHandle VulkanGraphicsDevice::CreateRhiRayTracingPipeline(
        const RhiRayTracingPipelineDesc&)
    {
        return {};
    }

    RhiShaderBindingTableHandle VulkanGraphicsDevice::CreateRhiShaderBindingTable(
        const RhiShaderBindingTableDesc&)
    {
        return {};
    }

#endif // RHI_VULKAN
} // namespace SasamiRenderer
