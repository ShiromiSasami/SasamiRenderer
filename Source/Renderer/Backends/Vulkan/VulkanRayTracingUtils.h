#pragma once

// Helpers shared by the Vulkan ray-tracing translation units: scratch/device buffer allocation,
// a one-shot command scope, device-address lookup and the SPIR-V entry-point scan.
//
// They were in the anonymous namespace of VulkanGraphicsDeviceRayTracing.cpp, which is what kept
// the acceleration-structure half from moving into its own file.

#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"

#if RHI_VULKAN

#include "Foundation/Tools/DebugOutput.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace SasamiEngine::Render
{
    // Helper: allocate a VkBuffer + VkDeviceMemory without going through the
    // normal RHI resource path (used for internal scratch / AS storage).
    struct VkRtBuffer {
        VkBuffer       buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    inline VkRtBuffer AllocRtBuffer(VkDevice device,
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

    inline void FreeRtBuffer(VkDevice device, VkRtBuffer& buf)
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

    inline VkDeviceAddress GetBufAddr(VkDevice device,
                                      PFN_vkGetBufferDeviceAddressKHR pfn,
                                      VkBuffer buffer)
    {
        VkBufferDeviceAddressInfoKHR info{};
        info.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR;
        info.buffer = buffer;
        return pfn(device, &info);
    }

    inline VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment)
    {
        if (alignment == 0) return value;
        return (value + alignment - 1) & ~(alignment - 1);
    }

    // Allocate a host-visible + coherent buffer (optionally with a device
    // address). Used for the SBT and the smoke test's vertex/staging buffers,
    // where AllocRtBuffer's device-local memory would not be mappable.
    inline VkRtBuffer AllocRtBufferHostVisible(VkDevice device,
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
    inline void CollectSpirvEntryStages(
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

#endif // RHI_VULKAN
