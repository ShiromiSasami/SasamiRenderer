// VulkanGraphicsDevice_RayTracing.cpp
// Vulkan implementation of RHI ray tracing device methods.
// Requires VK_KHR_acceleration_structure + VK_KHR_buffer_device_address +
// VK_EXT_descriptor_indexing + VK_KHR_deferred_host_operations (all detected
// and enabled at device creation; see CreateDevice() in VulkanGraphicsDevice.cpp).
//
// RT pipeline creation (CreateRhiRayTracingPipeline) is NOT implemented:
// the RHI interface passes pre-compiled DXIL bytecode; Vulkan requires SPIR-V.
// DxrRayTracer is a DX12-only class (runs in the render graph path which is gated
// by supportsFeatureRenderPasses=true), so this stub causes no regression.

#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"

#include <cstring>
#include <vector>

#include "Foundation/Tools/DebugOutput.h"

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
    // NOT IMPLEMENTED: interface expects pre-compiled DXIL; Vulkan needs SPIR-V.
    // DxrRayTracer (the only caller) is DX12-only (render graph path), so this
    // stub causes no regression on Vulkan.
    // -------------------------------------------------------------------------
    RhiRayTracingPipelineHandle VulkanGraphicsDevice::CreateRhiRayTracingPipeline(
        const RhiRayTracingPipelineDesc&)
    {
        DebugLog("VulkanGraphicsDevice::CreateRhiRayTracingPipeline: not supported "
                 "(DXIL-to-SPIR-V cross-compilation not implemented; "
                 "provide SPIR-V bytecode via an extended interface).\n");
        return {};
    }

    // -------------------------------------------------------------------------
    // CreateRhiShaderBindingTable — depends on pipeline creation, also deferred.
    // -------------------------------------------------------------------------
    RhiShaderBindingTableHandle VulkanGraphicsDevice::CreateRhiShaderBindingTable(
        const RhiShaderBindingTableDesc&)
    {
        return {};
    }

#endif // RHI_VULKAN
} // namespace SasamiRenderer
