// VulkanGraphicsDevice_RayTracing.cpp
// Vulkan implementation of RHI ray tracing device methods.
// Requires VK_KHR_acceleration_structure + VK_KHR_buffer_device_address +
// VK_EXT_descriptor_indexing + VK_KHR_deferred_host_operations +
// VK_KHR_ray_tracing_pipeline (all detected and enabled at device creation;
// see CreateDevice() in VulkanGraphicsDevice.cpp).
//
// The RHI ray-tracing pipeline is fed SPIR-V bytecode via
// RhiRayTracingPipelineDesc::spirvBytecode (the DX12 backend uses the parallel
// DXIL field). CreateRhiRayTracingPipeline builds a VkPipeline with an internal
// fixed descriptor set layout (binding 0 = acceleration structure, binding 1 =
// storage image), CreateRhiShaderBindingTable packs the SBT, and DispatchRays
// records vkCmdTraceRaysKHR. RunRayTracingSmokeTest exercises the full path.
//
// NOTE: DxrRayTracer (the DX12 render-graph ray tracer) does not run on Vulkan
// (supportsFeatureRenderPasses=false); wiring these primitives into a Vulkan
// render pass — including a bindless resource model matching the DXR shader — is
// a separate, larger effort.

#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "Foundation/Tools/DebugOutput.h"

#if RHI_VULKAN
#include <filesystem>

#if defined(_WIN32)
#include <Windows.h>
#include <wrl/client.h>
#include <dxcapi.h>
#endif

#include "Renderer/Resources/ShaderCompilationService.h"
#endif

namespace SasamiRenderer
{
#if RHI_VULKAN

    namespace
    {
        // Helper: allocate a VkBuffer + VkDeviceMemory without going through the
        // normal RHI resource path (used for internal scratch / AS storage).
        struct VkRtBuffer {
            VkBuffer       buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
        };

        static VkRtBuffer AllocRtBuffer(VkDevice device,
                                        VkPhysicalDevice physDev,
                                        VkDeviceSize size,
                                        VkBufferUsageFlags usage,
                                        bool shaderDeviceAddress)
        {
            VkRtBuffer out{};
            VkBufferCreateInfo bi{};
            bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size        = size;
            bi.usage       = usage;
            if (shaderDeviceAddress) bi.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device, &bi, nullptr, &out.buffer) != VK_SUCCESS) return {};

            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device, out.buffer, &req);

            // Find device-local memory type
            VkPhysicalDeviceMemoryProperties memProps{};
            vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);
            uint32_t memIdx = UINT32_MAX;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
                if ((req.memoryTypeBits & (1u << i)) &&
                    (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    memIdx = i;
                    break;
                }
            }
            if (memIdx == UINT32_MAX) { vkDestroyBuffer(device, out.buffer, nullptr); return {}; }

            VkMemoryAllocateFlagsInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
            fi.flags = shaderDeviceAddress ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0u;

            VkMemoryAllocateInfo ai{};
            ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.pNext           = shaderDeviceAddress ? &fi : nullptr;
            ai.allocationSize  = req.size;
            ai.memoryTypeIndex = memIdx;
            if (vkAllocateMemory(device, &ai, nullptr, &out.memory) != VK_SUCCESS) {
                vkDestroyBuffer(device, out.buffer, nullptr);
                return {};
            }
            vkBindBufferMemory(device, out.buffer, out.memory, 0);
            return out;
        }

        static void FreeRtBuffer(VkDevice device, VkRtBuffer& buf)
        {
            if (buf.memory != VK_NULL_HANDLE) { vkFreeMemory(device, buf.memory, nullptr); buf.memory = VK_NULL_HANDLE; }
            if (buf.buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, buf.buffer, nullptr); buf.buffer = VK_NULL_HANDLE; }
        }

        // Helper: record a single-use command buffer, submit, and wait idle.
        struct ScopedCmd {
            VkDevice        device     = VK_NULL_HANDLE;
            VkCommandPool   pool       = VK_NULL_HANDLE;
            VkCommandBuffer cmd        = VK_NULL_HANDLE;
            VkQueue         queue      = VK_NULL_HANDLE;
            bool            ok         = false;

            ScopedCmd(VkDevice dev, VkCommandPool pool_, VkQueue q) : device(dev), pool(pool_), queue(q)
            {
                VkCommandBufferAllocateInfo ai{};
                ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                ai.commandPool        = pool;
                ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                ai.commandBufferCount = 1;
                if (vkAllocateCommandBuffers(device, &ai, &cmd) != VK_SUCCESS) return;
                VkCommandBufferBeginInfo bi{};
                bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                ok = vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS;
            }

            bool Submit()
            {
                if (!ok || vkEndCommandBuffer(cmd) != VK_SUCCESS) return false;
                VkSubmitInfo si{};
                si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                si.commandBufferCount = 1;
                si.pCommandBuffers    = &cmd;
                return vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS &&
                       vkQueueWaitIdle(queue) == VK_SUCCESS;
            }

            ~ScopedCmd()
            {
                if (cmd != VK_NULL_HANDLE) vkFreeCommandBuffers(device, pool, 1, &cmd);
            }
        };

        static VkDeviceAddress GetBufAddr(VkDevice device,
                                          PFN_vkGetBufferDeviceAddressKHR pfn,
                                          VkBuffer buffer)
        {
            VkBufferDeviceAddressInfoKHR info{};
            info.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR;
            info.buffer = buffer;
            return pfn(device, &info);
        }

        static inline VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
        {
            if (alignment == 0) return value;
            return (value + alignment - 1) & ~(alignment - 1);
        }

        // Allocate a host-visible + coherent buffer (optionally with a device
        // address). Used for the SBT and the smoke test's vertex/staging buffers,
        // where AllocRtBuffer's device-local memory would not be mappable.
        static VkRtBuffer AllocRtBufferHostVisible(VkDevice device,
                                                   VkPhysicalDevice physDev,
                                                   VkDeviceSize size,
                                                   VkBufferUsageFlags usage,
                                                   bool shaderDeviceAddress)
        {
            VkRtBuffer out{};
            VkBufferCreateInfo bi{};
            bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size        = size;
            bi.usage       = usage;
            if (shaderDeviceAddress) bi.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device, &bi, nullptr, &out.buffer) != VK_SUCCESS) return {};

            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device, out.buffer, &req);

            VkPhysicalDeviceMemoryProperties memProps{};
            vkGetPhysicalDeviceMemoryProperties(physDev, &memProps);
            uint32_t memIdx = UINT32_MAX;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
                if ((req.memoryTypeBits & (1u << i)) &&
                    (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                    (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                    memIdx = i;
                    break;
                }
            }
            if (memIdx == UINT32_MAX) { vkDestroyBuffer(device, out.buffer, nullptr); return {}; }

            VkMemoryAllocateFlagsInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
            fi.flags = shaderDeviceAddress ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0u;

            VkMemoryAllocateInfo ai{};
            ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.pNext           = shaderDeviceAddress ? &fi : nullptr;
            ai.allocationSize  = req.size;
            ai.memoryTypeIndex = memIdx;
            if (vkAllocateMemory(device, &ai, nullptr, &out.memory) != VK_SUCCESS) {
                vkDestroyBuffer(device, out.buffer, nullptr);
                return {};
            }
            vkBindBufferMemory(device, out.buffer, out.memory, 0);
            return out;
        }

        // Scan a SPIR-V module for OpEntryPoint instructions, mapping each entry
        // point name to its ray-tracing shader stage. This lets the pipeline derive
        // the correct VkShaderStageFlagBits for each export named in the RHI shader
        // group descriptors without any external stage metadata.
        static void CollectSpirvEntryStages(
            const uint32_t* code, size_t wordCount,
            std::unordered_map<std::string, VkShaderStageFlagBits>& out)
        {
            if (!code || wordCount < 5) return;
            if (code[0] != 0x07230203u) return;  // SPIR-V magic

            size_t i = 5;  // skip header (magic, version, generator, bound, schema)
            while (i < wordCount) {
                const uint32_t inst      = code[i];
                const uint32_t opcode    = inst & 0xFFFFu;
                const uint32_t instWords = inst >> 16;
                if (instWords == 0 || i + instWords > wordCount) break;

                if (opcode == 15 /* OpEntryPoint */ && instWords >= 4) {
                    const uint32_t model = code[i + 1];
                    VkShaderStageFlagBits stage;
                    bool known = true;
                    switch (model) {
                        case 5313: stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;       break;
                        case 5314: stage = VK_SHADER_STAGE_INTERSECTION_BIT_KHR; break;
                        case 5315: stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;      break;
                        case 5316: stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;  break;
                        case 5317: stage = VK_SHADER_STAGE_MISS_BIT_KHR;         break;
                        case 5318: stage = VK_SHADER_STAGE_CALLABLE_BIT_KHR;     break;
                        default:   stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR; known = false; break;
                    }
                    if (known) {
                        // Name starts at word 3 (index i+3), null-terminated, packed.
                        const char* namePtr = reinterpret_cast<const char*>(&code[i + 3]);
                        const size_t maxBytes = (instWords - 3) * sizeof(uint32_t);
                        size_t nameLen = 0;
                        while (nameLen < maxBytes && namePtr[nameLen] != '\0') ++nameLen;
                        out[std::string(namePtr, nameLen)] = stage;
                    }
                }
                i += instWords;
            }
        }
    }

    // -------------------------------------------------------------------------
    // BuildRhiBlases
    // -------------------------------------------------------------------------
    bool VulkanGraphicsDevice::BuildRhiBlases(
        const RhiBlasDesc* descs, uint32_t count,
        RhiAccelerationStructureHandle* outHandles)
    {
        if (!descs || count == 0 || !outHandles) return false;
        if (!m_hasVkKhrAccelerationStructure ||
            !m_pfnGetAsBuildSizes || !m_pfnCreateAs || !m_pfnCmdBuildAs) {
            DebugLog("VulkanGraphicsDevice::BuildRhiBlases: acceleration structure extension not available.\n");
            return false;
        }

        for (uint32_t i = 0; i < count; ++i) outHandles[i] = {};

        for (uint32_t i = 0; i < count; ++i) {
            const RhiBlasDesc& blasDesc = descs[i];
            if (!blasDesc.geometries || blasDesc.geometryCount == 0) continue;

            // Fill geometry descriptors
            std::vector<VkAccelerationStructureGeometryKHR> geoms(blasDesc.geometryCount);
            std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(blasDesc.geometryCount);
            std::vector<uint32_t> primCounts(blasDesc.geometryCount);

            for (uint32_t g = 0; g < blasDesc.geometryCount; ++g) {
                const RhiRayTracingGeometryDesc& gd = blasDesc.geometries[g];

                VkAccelerationStructureGeometryTrianglesDataKHR tris{};
                tris.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                tris.vertexFormat  = VK_FORMAT_R32G32B32_SFLOAT;
                tris.vertexData.deviceAddress  = static_cast<VkDeviceAddress>(gd.vertexBufferAddress);
                tris.vertexStride  = gd.vertexStrideInBytes;
                tris.maxVertex     = gd.vertexCount > 0 ? gd.vertexCount - 1 : 0;
                tris.indexType     = gd.indexBufferAddress != 0
                                       ? (gd.index32Bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16)
                                       : VK_INDEX_TYPE_NONE_KHR;
                tris.indexData.deviceAddress   = static_cast<VkDeviceAddress>(gd.indexBufferAddress);
                tris.transformData.deviceAddress = 0;

                geoms[g].sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
                geoms[g].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
                geoms[g].geometry.triangles = tris;
                geoms[g].flags        = gd.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0u;

                primCounts[g] = gd.indexBufferAddress != 0
                                  ? gd.indexCount / 3
                                  : gd.vertexCount / 3;

                ranges[g].primitiveCount  = primCounts[g];
                ranges[g].primitiveOffset = 0;
                ranges[g].firstVertex     = 0;
                ranges[g].transformOffset = 0;
            }

            // Query sizes
            VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
            buildInfo.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            buildInfo.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            buildInfo.flags         = blasDesc.preferFastTrace
                                        ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                                        : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
            buildInfo.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            buildInfo.geometryCount = blasDesc.geometryCount;
            buildInfo.pGeometries   = geoms.data();

            VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
            sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
            m_pfnGetAsBuildSizes(m_device,
                                 VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                 &buildInfo, primCounts.data(), &sizeInfo);

            // Allocate scratch + AS buffers
            VkRtBuffer scratch = AllocRtBuffer(m_device, m_physicalDevice,
                                               sizeInfo.buildScratchSize,
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                               true);
            VkRtBuffer asBuf   = AllocRtBuffer(m_device, m_physicalDevice,
                                               sizeInfo.accelerationStructureSize,
                                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                                               true);
            if (!scratch.buffer || !asBuf.buffer) {
                FreeRtBuffer(m_device, scratch);
                FreeRtBuffer(m_device, asBuf);
                DebugLog("VulkanGraphicsDevice::BuildRhiBlases: buffer allocation failed.\n");
                return false;
            }

            // Create acceleration structure
            VkAccelerationStructureCreateInfoKHR asCreateInfo{};
            asCreateInfo.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
            asCreateInfo.buffer = asBuf.buffer;
            asCreateInfo.size   = sizeInfo.accelerationStructureSize;
            asCreateInfo.type   = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

            VulkanAccelStruct entry{};
            if (m_pfnCreateAs(m_device, &asCreateInfo, nullptr, &entry.as) != VK_SUCCESS) {
                FreeRtBuffer(m_device, scratch);
                FreeRtBuffer(m_device, asBuf);
                DebugLog("VulkanGraphicsDevice::BuildRhiBlases: vkCreateAccelerationStructureKHR failed.\n");
                return false;
            }
            entry.buffer = asBuf.buffer;
            entry.memory = asBuf.memory;

            // Get device address
            {
                VkAccelerationStructureDeviceAddressInfoKHR addrInfo{};
                addrInfo.sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
                addrInfo.accelerationStructure = entry.as;
                entry.deviceAddress = m_pfnGetAsAddress(m_device, &addrInfo);
            }

            // Build on GPU
            buildInfo.dstAccelerationStructure  = entry.as;
            buildInfo.scratchData.deviceAddress = GetBufAddr(m_device, m_pfnGetBufAddr, scratch.buffer);

            ScopedCmd sc(m_device, m_commandPool, m_graphicsQueue);
            if (!sc.ok) {
                m_pfnDestroyAs(m_device, entry.as, nullptr);
                FreeRtBuffer(m_device, scratch);
                FreeRtBuffer(m_device, asBuf);
                return false;
            }

            const VkAccelerationStructureBuildRangeInfoKHR* pRanges = ranges.data();
            m_pfnCmdBuildAs(sc.cmd, 1, &buildInfo, &pRanges);

            if (!sc.Submit()) {
                m_pfnDestroyAs(m_device, entry.as, nullptr);
                FreeRtBuffer(m_device, scratch);
                FreeRtBuffer(m_device, asBuf);
                DebugLog("VulkanGraphicsDevice::BuildRhiBlases: build command submission failed.\n");
                return false;
            }
            FreeRtBuffer(m_device, scratch);

            const uint64_t id = m_nextRhiResourceHandle++;
            m_accelStructures.emplace(id, entry);
            outHandles[i] = RhiAccelerationStructureHandle{ id };
        }
        return true;
    }

    // -------------------------------------------------------------------------
    // BuildRhiTlas
    // -------------------------------------------------------------------------
    RhiAccelerationStructureHandle VulkanGraphicsDevice::BuildRhiTlas(const RhiTlasDesc& desc)
    {
        if (!m_hasVkKhrAccelerationStructure ||
            !m_pfnGetAsBuildSizes || !m_pfnCreateAs || !m_pfnCmdBuildAs) {
            DebugLog("VulkanGraphicsDevice::BuildRhiTlas: acceleration structure extension not available.\n");
            return {};
        }
        if (!desc.instances || desc.instanceCount == 0) return {};

        // Build instance array
        std::vector<VkAccelerationStructureInstanceKHR> vkInstances(desc.instanceCount);
        for (uint32_t i = 0; i < desc.instanceCount; ++i) {
            const RhiTlasInstanceDesc& src = desc.instances[i];
            VkAccelerationStructureInstanceKHR& dst = vkInstances[i];
            std::memset(&dst, 0, sizeof(dst));

            // 3×4 row-major → VkTransformMatrixKHR (also 3×4 row-major)
            static_assert(sizeof(dst.transform.matrix) == 12 * sizeof(float));
            std::memcpy(dst.transform.matrix, src.transform, sizeof(dst.transform.matrix));

            dst.instanceCustomIndex                    = src.instanceID & 0xFFFFFF;
            dst.mask                                   = static_cast<uint8_t>(src.instanceMask);
            dst.instanceShaderBindingTableRecordOffset = src.hitGroupIndex;
            dst.flags = src.forceOpaque ? VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR : 0u;

            const auto it = m_accelStructures.find(src.blasHandle.id);
            if (it != m_accelStructures.end()) {
                dst.accelerationStructureReference = it->second.deviceAddress;
            }
        }

        // Upload instances to a device-visible buffer
        const VkDeviceSize instanceBufSize =
            static_cast<VkDeviceSize>(desc.instanceCount * sizeof(VkAccelerationStructureInstanceKHR));

        VkRtBuffer instanceBuf = AllocRtBuffer(m_device, m_physicalDevice,
                                               instanceBufSize,
                                               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                               true);
        if (!instanceBuf.buffer) {
            DebugLog("VulkanGraphicsDevice::BuildRhiTlas: instance buffer allocation failed.\n");
            return {};
        }

        // Upload via staging
        {
            VkRtBuffer staging = AllocRtBuffer(m_device, m_physicalDevice,
                                               instanceBufSize,
                                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false);
            if (!staging.buffer) { FreeRtBuffer(m_device, instanceBuf); return {}; }

            // Map staging
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(m_device, staging.buffer, &req);
            void* mapped = nullptr;
            // The staging buffer must be host-visible; for simplicity we re-allocate
            // using vkMapMemory after checking property.
            // Since AllocRtBuffer always picks DEVICE_LOCAL, we need a separate host-visible path.
            FreeRtBuffer(m_device, staging);

            // Allocate host-visible staging buffer directly
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size  = instanceBufSize;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkBuffer stageBuf = VK_NULL_HANDLE;
            VkDeviceMemory stageMem = VK_NULL_HANDLE;
            if (vkCreateBuffer(m_device, &bi, nullptr, &stageBuf) == VK_SUCCESS) {
                VkMemoryRequirements mr{};
                vkGetBufferMemoryRequirements(m_device, stageBuf, &mr);
                VkPhysicalDeviceMemoryProperties mp{};
                vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &mp);
                uint32_t memIdx = UINT32_MAX;
                for (uint32_t j = 0; j < mp.memoryTypeCount; ++j) {
                    if ((mr.memoryTypeBits & (1u << j)) &&
                        (mp.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                        (mp.memoryTypes[j].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                        memIdx = j;
                        break;
                    }
                }
                if (memIdx != UINT32_MAX) {
                    VkMemoryAllocateInfo mai{};
                    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                    mai.allocationSize = mr.size;
                    mai.memoryTypeIndex = memIdx;
                    if (vkAllocateMemory(m_device, &mai, nullptr, &stageMem) == VK_SUCCESS) {
                        vkBindBufferMemory(m_device, stageBuf, stageMem, 0);
                        if (vkMapMemory(m_device, stageMem, 0, instanceBufSize, 0, &mapped) == VK_SUCCESS) {
                            std::memcpy(mapped, vkInstances.data(), static_cast<size_t>(instanceBufSize));
                            vkUnmapMemory(m_device, stageMem);
                        }
                    }
                }
            }
            if (mapped == nullptr) {
                if (stageMem != VK_NULL_HANDLE) vkFreeMemory(m_device, stageMem, nullptr);
                if (stageBuf != VK_NULL_HANDLE) vkDestroyBuffer(m_device, stageBuf, nullptr);
                FreeRtBuffer(m_device, instanceBuf);
                return {};
            }

            ScopedCmd copyCmd(m_device, m_commandPool, m_graphicsQueue);
            if (copyCmd.ok) {
                VkBufferCopy copy{ 0, 0, instanceBufSize };
                vkCmdCopyBuffer(copyCmd.cmd, stageBuf, instanceBuf.buffer, 1, &copy);
                copyCmd.Submit();
            }
            if (stageMem != VK_NULL_HANDLE) vkFreeMemory(m_device, stageMem, nullptr);
            if (stageBuf != VK_NULL_HANDLE) vkDestroyBuffer(m_device, stageBuf, nullptr);
        }

        // TLAS geometry
        VkAccelerationStructureGeometryInstancesDataKHR instData{};
        instData.sType              = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        instData.arrayOfPointers    = VK_FALSE;
        instData.data.deviceAddress = GetBufAddr(m_device, m_pfnGetBufAddr, instanceBuf.buffer);

        VkAccelerationStructureGeometryKHR geom{};
        geom.sType              = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geom.geometryType       = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geom.geometry.instances = instData;

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
        buildInfo.sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        buildInfo.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        buildInfo.flags         = desc.preferFastTrace
                                    ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
                                    : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        buildInfo.mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries   = &geom;

        const uint32_t instanceCount = desc.instanceCount;
        VkAccelerationStructureBuildSizesInfoKHR sizeInfo{};
        sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        m_pfnGetAsBuildSizes(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                             &buildInfo, &instanceCount, &sizeInfo);

        VkRtBuffer scratch = AllocRtBuffer(m_device, m_physicalDevice,
                                           sizeInfo.buildScratchSize,
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        VkRtBuffer asBuf   = AllocRtBuffer(m_device, m_physicalDevice,
                                           sizeInfo.accelerationStructureSize,
                                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, true);
        if (!scratch.buffer || !asBuf.buffer) {
            FreeRtBuffer(m_device, scratch);
            FreeRtBuffer(m_device, asBuf);
            FreeRtBuffer(m_device, instanceBuf);
            return {};
        }

        VkAccelerationStructureCreateInfoKHR asCreateInfo{};
        asCreateInfo.sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        asCreateInfo.buffer = asBuf.buffer;
        asCreateInfo.size   = sizeInfo.accelerationStructureSize;
        asCreateInfo.type   = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

        VulkanAccelStruct entry{};
        if (m_pfnCreateAs(m_device, &asCreateInfo, nullptr, &entry.as) != VK_SUCCESS) {
            FreeRtBuffer(m_device, scratch);
            FreeRtBuffer(m_device, asBuf);
            FreeRtBuffer(m_device, instanceBuf);
            return {};
        }
        entry.buffer = asBuf.buffer;
        entry.memory = asBuf.memory;

        {
            VkAccelerationStructureDeviceAddressInfoKHR addrInfo{};
            addrInfo.sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
            addrInfo.accelerationStructure = entry.as;
            entry.deviceAddress = m_pfnGetAsAddress(m_device, &addrInfo);
        }

        buildInfo.dstAccelerationStructure  = entry.as;
        buildInfo.scratchData.deviceAddress = GetBufAddr(m_device, m_pfnGetBufAddr, scratch.buffer);

        ScopedCmd sc(m_device, m_commandPool, m_graphicsQueue);
        if (!sc.ok) {
            m_pfnDestroyAs(m_device, entry.as, nullptr);
            FreeRtBuffer(m_device, scratch);
            FreeRtBuffer(m_device, asBuf);
            FreeRtBuffer(m_device, instanceBuf);
            return {};
        }

        VkAccelerationStructureBuildRangeInfoKHR range{ instanceCount, 0, 0, 0 };
        const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
        m_pfnCmdBuildAs(sc.cmd, 1, &buildInfo, &pRange);

        if (!sc.Submit()) {
            m_pfnDestroyAs(m_device, entry.as, nullptr);
            FreeRtBuffer(m_device, scratch);
            FreeRtBuffer(m_device, asBuf);
            FreeRtBuffer(m_device, instanceBuf);
            return {};
        }

        FreeRtBuffer(m_device, scratch);
        FreeRtBuffer(m_device, instanceBuf);

        const uint64_t id = m_nextRhiResourceHandle++;
        m_accelStructures.emplace(id, entry);
        return RhiAccelerationStructureHandle{ id };
    }

    // -------------------------------------------------------------------------
    // DestroyRhiAccelerationStructure
    // -------------------------------------------------------------------------
    bool VulkanGraphicsDevice::DestroyRhiAccelerationStructure(RhiAccelerationStructureHandle handle)
    {
        const auto it = m_accelStructures.find(handle.id);
        if (it == m_accelStructures.end()) return false;
        VulkanAccelStruct& entry = it->second;
        if (m_pfnDestroyAs && entry.as != VK_NULL_HANDLE)
            m_pfnDestroyAs(m_device, entry.as, nullptr);
        if (entry.memory != VK_NULL_HANDLE) vkFreeMemory(m_device, entry.memory, nullptr);
        if (entry.buffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, entry.buffer, nullptr);
        m_accelStructures.erase(it);
        return true;
    }

    // -------------------------------------------------------------------------
    // GetRhiAccelerationStructureGpuAddress
    // -------------------------------------------------------------------------
    RhiGpuAddress VulkanGraphicsDevice::GetRhiAccelerationStructureGpuAddress(
        RhiAccelerationStructureHandle handle)
    {
        const auto it = m_accelStructures.find(handle.id);
        if (it == m_accelStructures.end()) return 0;
        return static_cast<RhiGpuAddress>(it->second.deviceAddress);
    }

    // -------------------------------------------------------------------------
    // CreateRhiAccelerationStructureSrv
    // Stores the TLAS handle in the descriptor slot. The actual VkDescriptorSet
    // write (with VkWriteDescriptorSetAccelerationStructureKHR pNext) is deferred
    // to SetGraphicsDescriptorTable in VulkanRhiCommandEncoder.
    // -------------------------------------------------------------------------
    bool VulkanGraphicsDevice::CreateRhiAccelerationStructureSrv(
        RhiAccelerationStructureHandle handle, RhiCpuDescriptorHandle dest)
    {
        const auto it = m_accelStructures.find(handle.id);
        if (it == m_accelStructures.end() || !dest.IsValid()) return false;

        VulkanRhiDescriptor descriptor{};
        descriptor.type                = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        descriptor.accelerationStructure = it->second.as;
        descriptor.resourceId          = handle.id;
        m_rhiDescriptors[dest.ptr]     = descriptor;
        return true;
    }

    // -------------------------------------------------------------------------
    // CreateRhiRayTracingPipeline
    // Builds a VkPipeline from the SPIR-V library (RhiRayTracingPipelineDesc::
    // spirvBytecode). Shader stages are derived per export from the SPIR-V entry
    // points; an internal fixed descriptor set layout (binding 0 = acceleration
    // structure, binding 1 = storage image) provides the pipeline layout.
    // -------------------------------------------------------------------------
    RhiRayTracingPipelineHandle VulkanGraphicsDevice::CreateRhiRayTracingPipeline(
        const RhiRayTracingPipelineDesc& desc)
    {
        if (!m_hasVkKhrRayTracingPipeline || !m_pfnCreateRtPipeline) {
            DebugLog("VulkanGraphicsDevice::CreateRhiRayTracingPipeline: "
                     "VK_KHR_ray_tracing_pipeline not available.\n");
            return {};
        }
        if (!desc.spirvBytecode || desc.spirvBytecodeSizeInBytes == 0 ||
            (desc.spirvBytecodeSizeInBytes % 4) != 0) {
            DebugLog("VulkanGraphicsDevice::CreateRhiRayTracingPipeline: "
                     "valid SPIR-V bytecode required (set spirvBytecode/size).\n");
            return {};
        }
        if (!desc.shaderGroups || desc.shaderGroupCount == 0) {
            DebugLog("VulkanGraphicsDevice::CreateRhiRayTracingPipeline: no shader groups.\n");
            return {};
        }

        VkShaderModuleCreateInfo smi{};
        smi.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = desc.spirvBytecodeSizeInBytes;
        smi.pCode    = reinterpret_cast<const uint32_t*>(desc.spirvBytecode);
        VkShaderModule shaderModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &smi, nullptr, &shaderModule) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateRhiRayTracingPipeline: vkCreateShaderModule failed.\n");
            return {};
        }

        std::unordered_map<std::string, VkShaderStageFlagBits> entryStages;
        CollectSpirvEntryStages(smi.pCode, desc.spirvBytecodeSizeInBytes / 4, entryStages);

        std::vector<VkPipelineShaderStageCreateInfo>       stages;
        std::vector<VkRayTracingShaderGroupCreateInfoKHR>  groups;
        std::unordered_map<std::string, uint32_t>          stageIndexByName;
        std::vector<std::string>                           stageNames;  // stable storage for pName
        stageNames.reserve(desc.shaderGroupCount + 1);
        stages.reserve(desc.shaderGroupCount + 1);
        groups.reserve(desc.shaderGroupCount);

        bool resolveOk = true;
        auto addStage = [&](const char* exportName) -> uint32_t {
            if (!exportName) return VK_SHADER_UNUSED_KHR;
            const std::string name(exportName);
            const auto existing = stageIndexByName.find(name);
            if (existing != stageIndexByName.end()) return existing->second;
            const auto se = entryStages.find(name);
            if (se == entryStages.end()) {
                DebugLog("VulkanGraphicsDevice::CreateRhiRayTracingPipeline: "
                         "shader export not found among SPIR-V entry points: ");
                DebugLog(name.c_str());
                DebugLog("\n");
                resolveOk = false;
                return VK_SHADER_UNUSED_KHR;
            }
            const uint32_t idx = static_cast<uint32_t>(stages.size());
            stageNames.push_back(name);
            VkPipelineShaderStageCreateInfo st{};
            st.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st.stage  = se->second;
            st.module = shaderModule;
            st.pName  = stageNames.back().c_str();
            stages.push_back(st);
            stageIndexByName[name] = idx;
            return idx;
        };

        for (uint32_t g = 0; g < desc.shaderGroupCount; ++g) {
            const RhiRayTracingShaderGroupDesc& gd = desc.shaderGroups[g];
            VkRayTracingShaderGroupCreateInfoKHR grp{};
            grp.sType              = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            grp.generalShader      = VK_SHADER_UNUSED_KHR;
            grp.closestHitShader   = VK_SHADER_UNUSED_KHR;
            grp.anyHitShader       = VK_SHADER_UNUSED_KHR;
            grp.intersectionShader = VK_SHADER_UNUSED_KHR;

            if (gd.type == RhiRayTracingShaderGroupDesc::Type::General) {
                grp.type          = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
                grp.generalShader = addStage(gd.exportName);
            } else {
                grp.type            = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
                grp.closestHitShader = addStage(gd.closestHitExport);
            }
            groups.push_back(grp);
        }

        if (!resolveOk || stages.empty()) {
            vkDestroyShaderModule(m_device, shaderModule, nullptr);
            DebugLog("VulkanGraphicsDevice::CreateRhiRayTracingPipeline: failed to resolve stages.\n");
            return {};
        }

        // Internal fixed descriptor set layout: AS (0) + storage image (1).
        VkDescriptorSetLayoutBinding binds[2]{};
        binds[0].binding         = 0;
        binds[0].descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        binds[0].descriptorCount = 1;
        binds[0].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                                   VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                                   VK_SHADER_STAGE_MISS_BIT_KHR;
        binds[1].binding         = 1;
        binds[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binds[1].descriptorCount = 1;
        binds[1].stageFlags      = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

        VkDescriptorSetLayoutCreateInfo dsli{};
        dsli.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsli.bindingCount = 2;
        dsli.pBindings    = binds;
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        if (vkCreateDescriptorSetLayout(m_device, &dsli, nullptr, &dsl) != VK_SUCCESS) {
            vkDestroyShaderModule(m_device, shaderModule, nullptr);
            return {};
        }

        VkPipelineLayoutCreateInfo pli{};
        pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts    = &dsl;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        if (vkCreatePipelineLayout(m_device, &pli, nullptr, &pipelineLayout) != VK_SUCCESS) {
            vkDestroyDescriptorSetLayout(m_device, dsl, nullptr);
            vkDestroyShaderModule(m_device, shaderModule, nullptr);
            return {};
        }

        uint32_t recursion = desc.maxRecursionDepth;
        if (m_rtMaxRayRecursionDepth != 0 && recursion > m_rtMaxRayRecursionDepth)
            recursion = m_rtMaxRayRecursionDepth;

        VkRayTracingPipelineCreateInfoKHR rtci{};
        rtci.sType                        = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
        rtci.stageCount                   = static_cast<uint32_t>(stages.size());
        rtci.pStages                      = stages.data();
        rtci.groupCount                   = static_cast<uint32_t>(groups.size());
        rtci.pGroups                      = groups.data();
        rtci.maxPipelineRayRecursionDepth = recursion;
        rtci.layout                       = pipelineLayout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult pr = m_pfnCreateRtPipeline(m_device, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                                  1, &rtci, nullptr, &pipeline);
        if (pr != VK_SUCCESS || pipeline == VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(m_device, pipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(m_device, dsl, nullptr);
            vkDestroyShaderModule(m_device, shaderModule, nullptr);
            DebugLog("VulkanGraphicsDevice::CreateRhiRayTracingPipeline: vkCreateRayTracingPipelinesKHR failed.\n");
            return {};
        }

        VulkanRtPipeline rec{};
        rec.pipeline       = pipeline;
        rec.pipelineLayout = pipelineLayout;
        rec.descSetLayout  = dsl;
        rec.module         = shaderModule;
        rec.groupCount     = static_cast<uint32_t>(groups.size());

        const uint64_t id = m_nextRhiPipelineHandle++;
        m_rtPipelines.emplace(id, rec);
        return RhiRayTracingPipelineHandle{ id };
    }

    // -------------------------------------------------------------------------
    // CreateRhiShaderBindingTable
    // Fetches shader group handles and packs raygen / miss / hit regions into a
    // host-visible SBT buffer, honoring the device's handle size and alignments.
    // -------------------------------------------------------------------------
    RhiShaderBindingTableHandle VulkanGraphicsDevice::CreateRhiShaderBindingTable(
        const RhiShaderBindingTableDesc& desc)
    {
        if (!m_pfnGetRtHandles || m_rtShaderGroupHandleSize == 0) {
            DebugLog("VulkanGraphicsDevice::CreateRhiShaderBindingTable: RT pipeline properties unavailable.\n");
            return {};
        }
        const auto pit = m_rtPipelines.find(desc.pipeline.id);
        if (pit == m_rtPipelines.end()) {
            DebugLog("VulkanGraphicsDevice::CreateRhiShaderBindingTable: pipeline not found.\n");
            return {};
        }
        const VulkanRtPipeline& pipe = pit->second;
        const uint32_t     groupCount   = pipe.groupCount;
        const uint32_t     handleSize   = m_rtShaderGroupHandleSize;
        const VkDeviceSize handleAligned = AlignUp(handleSize, m_rtShaderGroupHandleAlignment);
        const VkDeviceSize baseAlign     = m_rtShaderGroupBaseAlignment;

        std::vector<uint8_t> handles(static_cast<size_t>(groupCount) * handleSize);
        if (m_pfnGetRtHandles(m_device, pipe.pipeline, 0, groupCount,
                              handles.size(), handles.data()) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateRhiShaderBindingTable: vkGetRayTracingShaderGroupHandlesKHR failed.\n");
            return {};
        }
        auto handlePtr = [&](uint32_t groupIndex) -> const uint8_t* {
            if (groupIndex >= groupCount) return nullptr;
            return handles.data() + static_cast<size_t>(groupIndex) * handleSize;
        };

        const uint32_t missCount = desc.missGroupCount;
        const uint32_t hitCount  = desc.hitGroupCount;

        const VkDeviceSize raygenSize = AlignUp(handleAligned, baseAlign);
        const VkDeviceSize missSize   = missCount ? AlignUp(missCount * handleAligned, baseAlign) : 0;
        const VkDeviceSize hitSize    = hitCount  ? AlignUp(hitCount  * handleAligned, baseAlign) : 0;

        const VkDeviceSize raygenOffset = 0;
        const VkDeviceSize missOffset   = raygenSize;
        const VkDeviceSize hitOffset    = missOffset + missSize;
        const VkDeviceSize sbtSize      = hitOffset + hitSize;

        VkRtBuffer sbtBuf = AllocRtBufferHostVisible(m_device, m_physicalDevice, sbtSize,
                                                     VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        if (!sbtBuf.buffer) {
            DebugLog("VulkanGraphicsDevice::CreateRhiShaderBindingTable: SBT buffer allocation failed.\n");
            return {};
        }

        void* mapped = nullptr;
        if (vkMapMemory(m_device, sbtBuf.memory, 0, sbtSize, 0, &mapped) != VK_SUCCESS) {
            FreeRtBuffer(m_device, sbtBuf);
            return {};
        }
        uint8_t* dst = static_cast<uint8_t*>(mapped);
        std::memset(dst, 0, static_cast<size_t>(sbtSize));
        if (const uint8_t* h = handlePtr(desc.rayGenGroupIndex))
            std::memcpy(dst + raygenOffset, h, handleSize);
        for (uint32_t i = 0; i < missCount; ++i) {
            const uint32_t gi = desc.missGroupIndices ? desc.missGroupIndices[i] : 0;
            if (const uint8_t* h = handlePtr(gi))
                std::memcpy(dst + missOffset + i * handleAligned, h, handleSize);
        }
        for (uint32_t i = 0; i < hitCount; ++i) {
            const uint32_t gi = desc.hitGroupIndices ? desc.hitGroupIndices[i] : 0;
            if (const uint8_t* h = handlePtr(gi))
                std::memcpy(dst + hitOffset + i * handleAligned, h, handleSize);
        }
        vkUnmapMemory(m_device, sbtBuf.memory);

        const VkDeviceAddress sbtAddr = GetBufAddr(m_device, m_pfnGetBufAddr, sbtBuf.buffer);

        VulkanShaderBindingTable rec{};
        rec.buffer     = sbtBuf.buffer;
        rec.memory     = sbtBuf.memory;
        rec.pipelineId = desc.pipeline.id;
        rec.raygen.deviceAddress = sbtAddr + raygenOffset;
        rec.raygen.stride        = raygenSize;   // raygen: stride must equal size
        rec.raygen.size          = raygenSize;
        rec.miss.deviceAddress   = missCount ? sbtAddr + missOffset : 0;
        rec.miss.stride          = missCount ? handleAligned : 0;
        rec.miss.size            = missSize;
        rec.hit.deviceAddress    = hitCount ? sbtAddr + hitOffset : 0;
        rec.hit.stride           = hitCount ? handleAligned : 0;
        rec.hit.size             = hitSize;
        // callable region left zeroed.

        const uint64_t id = m_nextRhiResourceHandle++;
        m_shaderBindingTables.emplace(id, rec);
        return RhiShaderBindingTableHandle{ id };
    }

    // -------------------------------------------------------------------------
    // RecordTraceRays — shared trace-rays recording helper.
    // -------------------------------------------------------------------------
    void VulkanGraphicsDevice::RecordTraceRays(
        VkCommandBuffer cmd, VkPipeline pipeline, VkPipelineLayout layout,
        VkDescriptorSet descriptorSet,
        const VkStridedDeviceAddressRegionKHR& raygen,
        const VkStridedDeviceAddressRegionKHR& miss,
        const VkStridedDeviceAddressRegionKHR& hit,
        const VkStridedDeviceAddressRegionKHR& callable,
        uint32_t width, uint32_t height, uint32_t depth)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
        if (descriptorSet != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                    layout, 0, 1, &descriptorSet, 0, nullptr);
        }
        m_pfnCmdTraceRays(cmd, &raygen, &miss, &hit, &callable, width, height, depth);
    }

    // -------------------------------------------------------------------------
    // RunRayTracingSmokeTest
    // End-to-end validation of the Vulkan HW ray-tracing RHI primitives: builds a
    // one-triangle BLAS/TLAS, compiles the smoke-test shader to SPIR-V, creates
    // an RT pipeline + SBT, traces one ray per pixel into a 64×64 storage image,
    // and verifies the centre pixel is red (hit) and a corner is blue (miss).
    // -------------------------------------------------------------------------
    bool VulkanGraphicsDevice::RunRayTracingSmokeTest(std::string* outMessage)
    {
        auto fail = [&](const char* msg) -> bool {
            if (outMessage) *outMessage = msg;
            DebugLog("Vulkan RT smoke test FAIL: ");
            DebugLog(msg);
            DebugLog("\n");
            return false;
        };

        if (!m_hasVkKhrRayTracingPipeline || !m_pfnCreateRtPipeline ||
            !m_pfnGetRtHandles || !m_pfnCmdTraceRays)
            return fail("VK_KHR_ray_tracing_pipeline not available on this device.");
        if (!m_hasVkKhrAccelerationStructure)
            return fail("VK_KHR_acceleration_structure not available on this device.");

#if !defined(_WIN32)
        return fail("smoke test requires DXC (Windows) to compile the SPIR-V library.");
#else
        constexpr uint32_t W = 64, H = 64;

        // Resources tracked for cleanup on every exit path.
        VkRtBuffer vtxBuf{}, readBuf{};
        RhiAccelerationStructureHandle blas{}, tlas{};
        VkImage        image     = VK_NULL_HANDLE;
        VkDeviceMemory imageMem  = VK_NULL_HANDLE;
        VkImageView    imageView = VK_NULL_HANDLE;
        VkDescriptorPool pool    = VK_NULL_HANDLE;
        RhiRayTracingPipelineHandle pipeH{};
        RhiShaderBindingTableHandle sbtH{};

        auto cleanup = [&]() {
            if (readBuf.buffer)  FreeRtBuffer(m_device, readBuf);
            if (pool)            vkDestroyDescriptorPool(m_device, pool, nullptr);
            if (imageView)       vkDestroyImageView(m_device, imageView, nullptr);
            if (image)           vkDestroyImage(m_device, image, nullptr);
            if (imageMem)        vkFreeMemory(m_device, imageMem, nullptr);
            if (sbtH.id) {
                auto it = m_shaderBindingTables.find(sbtH.id);
                if (it != m_shaderBindingTables.end()) {
                    VkRtBuffer b{ it->second.buffer, it->second.memory };
                    FreeRtBuffer(m_device, b);
                    m_shaderBindingTables.erase(it);
                }
            }
            if (pipeH.id) {
                auto it = m_rtPipelines.find(pipeH.id);
                if (it != m_rtPipelines.end()) {
                    if (it->second.pipeline)       vkDestroyPipeline(m_device, it->second.pipeline, nullptr);
                    if (it->second.pipelineLayout) vkDestroyPipelineLayout(m_device, it->second.pipelineLayout, nullptr);
                    if (it->second.descSetLayout)  vkDestroyDescriptorSetLayout(m_device, it->second.descSetLayout, nullptr);
                    if (it->second.module)         vkDestroyShaderModule(m_device, it->second.module, nullptr);
                    m_rtPipelines.erase(it);
                }
            }
            if (tlas.id) DestroyRhiAccelerationStructure(tlas);
            if (blas.id) DestroyRhiAccelerationStructure(blas);
            if (vtxBuf.buffer) FreeRtBuffer(m_device, vtxBuf);
        };
        auto failClean = [&](const char* msg) -> bool { cleanup(); return fail(msg); };

        // 1. Compile the smoke-test shader to SPIR-V via DXC.
        std::vector<uint8_t> spirv;
        {
            namespace WRL = Microsoft::WRL;
            static HMODULE dxc = LoadLibraryW(L"dxcompiler.dll");
            auto createInst = dxc
                ? reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>(
                      GetProcAddress(dxc, "DxcCreateInstance"))
                : nullptr;
            if (!createInst) return fail("dxcompiler.dll / DxcCreateInstance not found.");

            WRL::ComPtr<IDxcUtils>    utils;
            WRL::ComPtr<IDxcCompiler3> compiler;
            if (FAILED(createInst(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
                FAILED(createInst(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))))
                return fail("failed to create DXC instances.");

            const std::filesystem::path shaderRoot = ShaderCompilationService::GetShaderSourceRoot();
            const std::filesystem::path shaderPath =
                shaderRoot / L"RayTracing" / L"DXR" / L"VulkanRtSmokeTest.hlsl";

            WRL::ComPtr<IDxcBlobEncoding> source;
            if (FAILED(utils->LoadFile(shaderPath.c_str(), nullptr, &source)) || !source)
                return fail("failed to load VulkanRtSmokeTest.hlsl.");

            WRL::ComPtr<IDxcIncludeHandler> includeHandler;
            utils->CreateDefaultIncludeHandler(&includeHandler);

            const std::wstring srcName = shaderPath.native();
            const std::wstring incDir  = shaderRoot.native();
            std::vector<LPCWSTR> args{
                srcName.c_str(),
                L"-T", L"lib_6_6",
                L"-I", incDir.c_str(),
                L"-spirv",
                L"-fspv-target-env=vulkan1.2",
                L"-HV", L"2021",
            };

            DxcBuffer buf{ source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_ACP };
            WRL::ComPtr<IDxcResult> result;
            if (FAILED(compiler->Compile(&buf, args.data(), static_cast<UINT32>(args.size()),
                                         includeHandler.Get(), IID_PPV_ARGS(&result))) || !result)
                return fail("DXC compile invocation failed.");

            WRL::ComPtr<IDxcBlobUtf8> errors;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
                errors && errors->GetStringLength() > 0) {
                DebugLog(errors->GetStringPointer());
                DebugLog("\n");
            }
            HRESULT status = S_OK;
            if (FAILED(result->GetStatus(&status)) || FAILED(status))
                return fail("SPIR-V compilation failed (see DXC errors above).");

            WRL::ComPtr<IDxcBlob> object;
            if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr)) || !object)
                return fail("missing SPIR-V object output.");
            spirv.resize(object->GetBufferSize());
            std::memcpy(spirv.data(), object->GetBufferPointer(), spirv.size());
        }

        // 2. Single triangle in the z = 1 plane → BLAS → TLAS.
        const float verts[9] = {
            -0.5f, -0.5f, 1.0f,
             0.5f, -0.5f, 1.0f,
             0.0f,  0.5f, 1.0f,
        };
        vtxBuf = AllocRtBufferHostVisible(
            m_device, m_physicalDevice, sizeof(verts),
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
        if (!vtxBuf.buffer) return failClean("vertex buffer allocation failed.");
        {
            void* m = nullptr;
            if (vkMapMemory(m_device, vtxBuf.memory, 0, sizeof(verts), 0, &m) != VK_SUCCESS)
                return failClean("vertex buffer map failed.");
            std::memcpy(m, verts, sizeof(verts));
            vkUnmapMemory(m_device, vtxBuf.memory);
        }
        const VkDeviceAddress vtxAddr = GetBufAddr(m_device, m_pfnGetBufAddr, vtxBuf.buffer);

        RhiRayTracingGeometryDesc geo{};
        geo.vertexBufferAddress = static_cast<RhiGpuAddress>(vtxAddr);
        geo.vertexFormat        = RhiFormat::R32G32B32Float;
        geo.vertexCount         = 3;
        geo.vertexStrideInBytes = 12;
        geo.opaque              = true;

        RhiBlasDesc blasDesc{};
        blasDesc.geometries     = &geo;
        blasDesc.geometryCount  = 1;
        blasDesc.preferFastTrace = true;
        if (!BuildRhiBlases(&blasDesc, 1, &blas) || blas.id == 0)
            return failClean("BLAS build failed.");

        RhiTlasInstanceDesc inst{};
        inst.blasHandle    = blas;
        inst.transform[0]  = 1.0f;  // identity 3×4 (row-major)
        inst.transform[5]  = 1.0f;
        inst.transform[10] = 1.0f;
        inst.instanceMask  = 0xFF;
        inst.forceOpaque   = true;

        RhiTlasDesc tlasDesc{};
        tlasDesc.instances      = &inst;
        tlasDesc.instanceCount  = 1;
        tlasDesc.preferFastTrace = true;
        tlas = BuildRhiTlas(tlasDesc);
        if (tlas.id == 0) return failClean("TLAS build failed.");

        const auto tlasIt = m_accelStructures.find(tlas.id);
        if (tlasIt == m_accelStructures.end()) return failClean("TLAS record missing.");
        const VkAccelerationStructureKHR tlasVk = tlasIt->second.as;

        // 3. Storage image (RGBA8) for the trace output.
        {
            VkImageCreateInfo ii{};
            ii.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ii.imageType     = VK_IMAGE_TYPE_2D;
            ii.format        = VK_FORMAT_R8G8B8A8_UNORM;
            ii.extent        = { W, H, 1 };
            ii.mipLevels     = 1;
            ii.arrayLayers   = 1;
            ii.samples       = VK_SAMPLE_COUNT_1_BIT;
            ii.tiling        = VK_IMAGE_TILING_OPTIMAL;
            ii.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_device, &ii, nullptr, &image) != VK_SUCCESS)
                return failClean("storage image creation failed.");

            VkMemoryRequirements mr{};
            vkGetImageMemoryRequirements(m_device, image, &mr);
            VkPhysicalDeviceMemoryProperties mp{};
            vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &mp);
            uint32_t mi = UINT32_MAX;
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
                if ((mr.memoryTypeBits & (1u << i)) &&
                    (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    mi = i;
                    break;
                }
            }
            VkMemoryAllocateInfo mai{};
            mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize  = mr.size;
            mai.memoryTypeIndex = mi;
            if (mi == UINT32_MAX ||
                vkAllocateMemory(m_device, &mai, nullptr, &imageMem) != VK_SUCCESS)
                return failClean("storage image memory allocation failed.");
            vkBindImageMemory(m_device, image, imageMem, 0);

            VkImageViewCreateInfo vi{};
            vi.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            vi.image            = image;
            vi.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            vi.format           = VK_FORMAT_R8G8B8A8_UNORM;
            vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (vkCreateImageView(m_device, &vi, nullptr, &imageView) != VK_SUCCESS)
                return failClean("storage image view creation failed.");
        }

        // Transition UNDEFINED → GENERAL for storage-image writes.
        {
            ScopedCmd sc(m_device, m_commandPool, m_graphicsQueue);
            if (!sc.ok) return failClean("command buffer allocation failed (layout).");
            VkImageMemoryBarrier b{};
            b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image               = image;
            b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            b.srcAccessMask       = 0;
            b.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(sc.cmd,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
            if (!sc.Submit()) return failClean("layout transition submit failed.");
        }

        // 4. Ray-tracing pipeline (raygen, miss, closest-hit).
        RhiRayTracingShaderGroupDesc groups[3]{};
        groups[0].type       = RhiRayTracingShaderGroupDesc::Type::General;
        groups[0].exportName = "RayGen";
        groups[1].type       = RhiRayTracingShaderGroupDesc::Type::General;
        groups[1].exportName = "Miss";
        groups[2].type            = RhiRayTracingShaderGroupDesc::Type::TrianglesHit;
        groups[2].hitGroupExport  = "HitGroup";
        groups[2].closestHitExport = "ClosestHit";

        RhiRayTracingPipelineDesc pd{};
        pd.spirvBytecode            = spirv.data();
        pd.spirvBytecodeSizeInBytes = spirv.size();
        pd.shaderGroups             = groups;
        pd.shaderGroupCount         = 3;
        pd.maxRecursionDepth        = 1;
        pd.maxPayloadSizeBytes      = 16;
        pd.maxAttributeSizeBytes    = 8;
        pipeH = CreateRhiRayTracingPipeline(pd);
        if (pipeH.id == 0) return failClean("RT pipeline creation failed.");
        auto pipeIt = m_rtPipelines.find(pipeH.id);
        if (pipeIt == m_rtPipelines.end()) return failClean("RT pipeline record missing.");

        // 5. Descriptor set: AS (binding 0) + storage image (binding 1).
        {
            VkDescriptorPoolSize ps[2]{};
            ps[0].type            = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            ps[0].descriptorCount = 1;
            ps[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            ps[1].descriptorCount = 1;
            VkDescriptorPoolCreateInfo dpi{};
            dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            dpi.maxSets       = 1;
            dpi.poolSizeCount = 2;
            dpi.pPoolSizes    = ps;
            if (vkCreateDescriptorPool(m_device, &dpi, nullptr, &pool) != VK_SUCCESS)
                return failClean("descriptor pool creation failed.");

            VkDescriptorSetAllocateInfo dsa{};
            dsa.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dsa.descriptorPool     = pool;
            dsa.descriptorSetCount = 1;
            dsa.pSetLayouts        = &pipeIt->second.descSetLayout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(m_device, &dsa, &set) != VK_SUCCESS)
                return failClean("descriptor set allocation failed.");

            VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
            asWrite.sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            asWrite.accelerationStructureCount = 1;
            asWrite.pAccelerationStructures    = &tlasVk;

            VkDescriptorImageInfo imgInfo{};
            imgInfo.imageView   = imageView;
            imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet writes[2]{};
            writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].pNext           = &asWrite;
            writes[0].dstSet          = set;
            writes[0].dstBinding      = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet          = set;
            writes[1].dstBinding      = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[1].pImageInfo      = &imgInfo;
            vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);

            // DispatchRays reads the descriptor set from the pipeline record.
            pipeIt->second.boundSet = set;
        }

        // 6. Shader binding table.
        const uint32_t missIdx[1] = { 1 };
        const uint32_t hitIdx[1]  = { 2 };
        RhiShaderBindingTableDesc sd{};
        sd.pipeline         = pipeH;
        sd.rayGenGroupIndex = 0;
        sd.missGroupIndices = missIdx;
        sd.missGroupCount   = 1;
        sd.hitGroupIndices  = hitIdx;
        sd.hitGroupCount    = 1;
        sbtH = CreateRhiShaderBindingTable(sd);
        if (sbtH.id == 0) return failClean("shader binding table creation failed.");
        const auto sbtIt = m_shaderBindingTables.find(sbtH.id);
        if (sbtIt == m_shaderBindingTables.end()) return failClean("SBT record missing.");

        // 7. Trace one ray per pixel. Uses the shared RecordTraceRays helper — the
        //    same code path the command encoder's DispatchRays records.
        {
            ScopedCmd sc(m_device, m_commandPool, m_graphicsQueue);
            if (!sc.ok) return failClean("command buffer allocation failed (dispatch).");
            RecordTraceRays(sc.cmd, pipeIt->second.pipeline, pipeIt->second.pipelineLayout,
                            pipeIt->second.boundSet,
                            sbtIt->second.raygen, sbtIt->second.miss,
                            sbtIt->second.hit, sbtIt->second.callable, W, H, 1);
            if (!sc.Submit()) return failClean("trace rays submit failed.");
        }

        // 8. Copy the image to a host-visible buffer and read it back.
        readBuf = AllocRtBufferHostVisible(m_device, m_physicalDevice,
                                           static_cast<VkDeviceSize>(W) * H * 4,
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        if (!readBuf.buffer) return failClean("readback buffer allocation failed.");
        {
            ScopedCmd sc(m_device, m_commandPool, m_graphicsQueue);
            if (!sc.ok) return failClean("command buffer allocation failed (readback).");
            VkImageMemoryBarrier b{};
            b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image               = image;
            b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            b.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(sc.cmd,
                                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent      = { W, H, 1 };
            vkCmdCopyImageToBuffer(sc.cmd, image, VK_IMAGE_LAYOUT_GENERAL,
                                   readBuf.buffer, 1, &copy);
            if (!sc.Submit()) return failClean("readback submit failed.");
        }

        bool centerRed = false, cornerBlue = false;
        {
            void* rd = nullptr;
            if (vkMapMemory(m_device, readBuf.memory, 0,
                            static_cast<VkDeviceSize>(W) * H * 4, 0, &rd) != VK_SUCCESS)
                return failClean("readback map failed.");
            const uint8_t* px = static_cast<const uint8_t*>(rd);
            auto at = [&](uint32_t x, uint32_t y) -> const uint8_t* {
                return px + (static_cast<size_t>(y) * W + x) * 4;
            };
            const uint8_t* c = at(W / 2, H / 2);
            const uint8_t* k = at(0, 0);
            centerRed  = (c[0] > 200 && c[2] < 60);
            cornerBlue = (k[2] > 200 && k[0] < 60);
            vkUnmapMemory(m_device, readBuf.memory);
        }

        const bool pass = centerRed && cornerBlue;
        cleanup();

        if (pass) {
            if (outMessage) *outMessage = "PASS (hit=red, miss=blue)";
            DebugLog("Vulkan RT smoke test: PASS (hit=red, miss=blue)\n");
            return true;
        }
        return fail(centerRed ? "corner pixel was not blue (miss shader)."
                              : "centre pixel was not red (hit shader).");
#endif // _WIN32
    }

#endif // RHI_VULKAN
} // namespace SasamiRenderer
