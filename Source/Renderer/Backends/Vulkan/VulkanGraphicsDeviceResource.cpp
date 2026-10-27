// VulkanGraphicsDeviceResource.cpp
// Creating, updating and destroying the RHI's textures and buffers on Vulkan -- allocation,
// memory type selection, staging uploads and readback.
//
// These lived in VulkanGraphicsDevicePipeline.cpp, whose name had stopped matching its contents:
// resources were more than half of it, and they have nothing to do with pipelines.
#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"
#include "Renderer/Backends/Vulkan/VulkanGraphicsDeviceUtils.h"

#if RHI_VULKAN

#include "Foundation/Tools/DebugOutput.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace SasamiEngine::Render
{
    namespace
    {
        // Texel size of the formats the staging-upload path accepts. VkBufferImageCopy wants
        // bufferRowLength in texels while callers supply a byte pitch, so the conversion needs
        // the format's stride -- hardcoding 4 silently corrupts every HDR (16-bit float) upload.
        uint32_t UploadBytesPerPixel(RhiFormat format)
        {
            switch (format) {
            case RhiFormat::R16G16B16A16Float: return 8u;
            case RhiFormat::R16Float: return 2u;
            default: return 4u;
            }
        }
    }

    RhiTextureHandle VulkanGraphicsDevice::UploadRhiTextureLayers(uint32_t width,
                                                                   uint32_t height,
                                                                   uint32_t layerCount,
                                                                   const void* const* layerPixels,
                                                                   uint32_t rowPitchBytes,
                                                                   RhiFormat format,
                                                                   bool cubeCompatible,
                                                                   uint32_t volumeDepth)
    {
        // volumeDepth > 0 makes a single-layer 3D image whose slices are packed back to back
        // (slice pitch == rowPitchBytes * height) in layerPixels[0].
        const bool volume = volumeDepth > 0;
        const uint32_t depth = volume ? volumeDepth : 1u;
        const uint32_t bytesPerPixel = UploadBytesPerPixel(format);
        const VkDeviceSize layerSize =
            static_cast<VkDeviceSize>(rowPitchBytes) * static_cast<VkDeviceSize>(height) * depth;
        const VkDeviceSize uploadSize = layerSize * static_cast<VkDeviceSize>(layerCount);

        VulkanRhiResource texture{};
        texture.extent = { width, height, depth };
        texture.format = format;
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.flags = cubeCompatible ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u;
        imageInfo.imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        imageInfo.format = ToVkFormat(format);
        imageInfo.extent = { width, height, depth };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = layerCount;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &imageInfo, nullptr, &texture.image) != VK_SUCCESS) {
            return {};
        }

        VkMemoryRequirements imageRequirements{};
        vkGetImageMemoryRequirements(m_device, texture.image, &imageRequirements);

        VkMemoryAllocateInfo imageAllocateInfo{};
        imageAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        imageAllocateInfo.allocationSize = imageRequirements.size;
        imageAllocateInfo.memoryTypeIndex =
            FindMemoryType(imageRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (imageAllocateInfo.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(m_device, &imageAllocateInfo, nullptr, &texture.memory) != VK_SUCCESS) {
            vkDestroyImage(m_device, texture.image, nullptr);
            return {};
        }
        if (vkBindImageMemory(m_device, texture.image, texture.memory, 0) != VK_SUCCESS) {
            vkFreeMemory(m_device, texture.memory, nullptr);
            vkDestroyImage(m_device, texture.image, nullptr);
            return {};
        }

        VulkanRhiResource staging{};
        VkBufferCreateInfo stagingInfo{};
        stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        stagingInfo.size = uploadSize;
        stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &stagingInfo, nullptr, &staging.buffer) != VK_SUCCESS) {
            vkFreeMemory(m_device, texture.memory, nullptr);
            vkDestroyImage(m_device, texture.image, nullptr);
            return {};
        }

        VkMemoryRequirements stagingRequirements{};
        vkGetBufferMemoryRequirements(m_device, staging.buffer, &stagingRequirements);

        VkMemoryAllocateInfo stagingAllocateInfo{};
        stagingAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        stagingAllocateInfo.allocationSize = stagingRequirements.size;
        stagingAllocateInfo.memoryTypeIndex =
            FindMemoryType(stagingRequirements.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (stagingAllocateInfo.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(m_device, &stagingAllocateInfo, nullptr, &staging.memory) != VK_SUCCESS) {
            vkDestroyBuffer(m_device, staging.buffer, nullptr);
            vkFreeMemory(m_device, texture.memory, nullptr);
            vkDestroyImage(m_device, texture.image, nullptr);
            return {};
        }
        if (vkBindBufferMemory(m_device, staging.buffer, staging.memory, 0) != VK_SUCCESS) {
            vkFreeMemory(m_device, staging.memory, nullptr);
            vkDestroyBuffer(m_device, staging.buffer, nullptr);
            vkFreeMemory(m_device, texture.memory, nullptr);
            vkDestroyImage(m_device, texture.image, nullptr);
            return {};
        }

        void* mapped = nullptr;
        if (vkMapMemory(m_device, staging.memory, 0, uploadSize, 0, &mapped) != VK_SUCCESS || !mapped) {
            vkFreeMemory(m_device, staging.memory, nullptr);
            vkDestroyBuffer(m_device, staging.buffer, nullptr);
            vkFreeMemory(m_device, texture.memory, nullptr);
            vkDestroyImage(m_device, texture.image, nullptr);
            return {};
        }
        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            std::memcpy(static_cast<uint8_t*>(mapped) + layer * layerSize,
                       layerPixels[layer],
                       static_cast<size_t>(layerSize));
        }
        vkUnmapMemory(m_device, staging.memory);

        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo commandAllocateInfo{};
        commandAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandAllocateInfo.commandPool = m_commandPool;
        commandAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandAllocateInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(m_device, &commandAllocateInfo, &commandBuffer) != VK_SUCCESS) {
            vkFreeMemory(m_device, staging.memory, nullptr);
            vkDestroyBuffer(m_device, staging.buffer, nullptr);
            vkFreeMemory(m_device, texture.memory, nullptr);
            vkDestroyImage(m_device, texture.image, nullptr);
            return {};
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        bool uploadSucceeded = vkBeginCommandBuffer(commandBuffer, &beginInfo) == VK_SUCCESS;
        if (uploadSucceeded) {
            VkImageMemoryBarrier toTransfer{};
            toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toTransfer.srcAccessMask = 0;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.image = texture.image;
            toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toTransfer.subresourceRange.baseMipLevel = 0;
            toTransfer.subresourceRange.levelCount = 1;
            toTransfer.subresourceRange.baseArrayLayer = 0;
            toTransfer.subresourceRange.layerCount = layerCount;
            vkCmdPipelineBarrier(commandBuffer,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0,
                                 0,
                                 nullptr,
                                 0,
                                 nullptr,
                                 1,
                                 &toTransfer);

            // One region per layer: each face/layer has its own byte offset into the staging
            // buffer, so a single region covering all layers cannot express the copy.
            std::vector<VkBufferImageCopy> copyRegions(layerCount);
            for (uint32_t layer = 0; layer < layerCount; ++layer) {
                VkBufferImageCopy& copyRegion = copyRegions[layer];
                copyRegion.bufferOffset = layer * layerSize;
                copyRegion.bufferRowLength = rowPitchBytes == width * bytesPerPixel ? 0u : rowPitchBytes / bytesPerPixel;
                copyRegion.bufferImageHeight = 0;
                copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                copyRegion.imageSubresource.mipLevel = 0;
                copyRegion.imageSubresource.baseArrayLayer = layer;
                copyRegion.imageSubresource.layerCount = 1;
                copyRegion.imageOffset = { 0, 0, 0 };
                copyRegion.imageExtent = { width, height, depth };
            }
            vkCmdCopyBufferToImage(commandBuffer,
                                   staging.buffer,
                                   texture.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   layerCount,
                                   copyRegions.data());

            VkImageMemoryBarrier toShaderRead{};
            toShaderRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toShaderRead.image = texture.image;
            toShaderRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toShaderRead.subresourceRange.baseMipLevel = 0;
            toShaderRead.subresourceRange.levelCount = 1;
            toShaderRead.subresourceRange.baseArrayLayer = 0;
            toShaderRead.subresourceRange.layerCount = layerCount;
            vkCmdPipelineBarrier(commandBuffer,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0,
                                 0,
                                 nullptr,
                                 0,
                                 nullptr,
                                 1,
                                 &toShaderRead);

            uploadSucceeded = vkEndCommandBuffer(commandBuffer) == VK_SUCCESS;
        }

        if (uploadSucceeded) {
            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commandBuffer;
            uploadSucceeded =
                vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS &&
                vkQueueWaitIdle(m_graphicsQueue) == VK_SUCCESS;
        }

        vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
        vkFreeMemory(m_device, staging.memory, nullptr);
        vkDestroyBuffer(m_device, staging.buffer, nullptr);

        if (!uploadSucceeded) {
            vkFreeMemory(m_device, texture.memory, nullptr);
            vkDestroyImage(m_device, texture.image, nullptr);
            return {};
        }

        const uint64_t id = m_nextRhiResourceHandle++;
        m_rhiResources.emplace(id, texture);
        return RhiTextureHandle{ id };
    }

    RhiTextureHandle VulkanGraphicsDevice::CreateRhiTexture2DFromRgba8(uint32_t width,
                                                                       uint32_t height,
                                                                       const void* pixels,
                                                                       uint32_t rowPitchBytes,
                                                                       RhiFormat format)
    {
        if (format != RhiFormat::R8G8B8A8UNorm && format != RhiFormat::R8G8B8A8UNormSrgb) {
            DebugLog("VulkanGraphicsDevice::CreateRhiTexture2DFromRgba8: unsupported format, only "
                     "R8G8B8A8UNorm and R8G8B8A8UNormSrgb are accepted.\n");
            return {};
        }
        if (m_device == VK_NULL_HANDLE ||
            m_graphicsQueue == VK_NULL_HANDLE ||
            m_commandPool == VK_NULL_HANDLE ||
            width == 0 ||
            height == 0 ||
            !pixels ||
            rowPitchBytes < width * 4u ||
            (rowPitchBytes % 4u) != 0u) {
            return {};
        }

        return UploadRhiTextureLayers(width, height, 1u, &pixels, rowPitchBytes, format, false);
    }

    RhiTextureHandle VulkanGraphicsDevice::CreateRhiTextureCubeFromFaces(uint32_t faceSize,
                                                                         const void* const faces[6],
                                                                         uint32_t rowPitchBytes,
                                                                         RhiFormat format)
    {
        // R16G16B16A16Float is here because the HDR skybox is the primary caller: its faces come
        // from RendererMathUtility::GenerateSkyCubemapFromEquirect as float RGBA.
        if (format != RhiFormat::R8G8B8A8UNorm && format != RhiFormat::R8G8B8A8UNormSrgb &&
            format != RhiFormat::R16G16B16A16Float) {
            DebugLog("VulkanGraphicsDevice::CreateRhiTextureCubeFromFaces: unsupported format, only "
                     "R8G8B8A8UNorm, R8G8B8A8UNormSrgb and R16G16B16A16Float are accepted.\n");
            return {};
        }
        const uint32_t faceBytesPerPixel = UploadBytesPerPixel(format);
        if (m_device == VK_NULL_HANDLE ||
            m_graphicsQueue == VK_NULL_HANDLE ||
            m_commandPool == VK_NULL_HANDLE ||
            faceSize == 0 ||
            !faces ||
            rowPitchBytes < faceSize * faceBytesPerPixel ||
            (rowPitchBytes % faceBytesPerPixel) != 0u) {
            return {};
        }
        for (uint32_t face = 0; face < 6u; ++face) {
            if (!faces[face]) {
                return {};
            }
        }

        return UploadRhiTextureLayers(faceSize, faceSize, 6u, faces, rowPitchBytes, format, true);
    }

    RhiTextureHandle VulkanGraphicsDevice::CreateRhiTexture3DFromData(uint32_t width,
                                                                      uint32_t height,
                                                                      uint32_t depth,
                                                                      const void* texels,
                                                                      uint32_t rowPitchBytes,
                                                                      uint32_t slicePitchBytes,
                                                                      RhiFormat format)
    {
        if (format != RhiFormat::R32Float && format != RhiFormat::R16Float) {
            DebugLog("VulkanGraphicsDevice::CreateRhiTexture3DFromData: unsupported format, only R32Float/R16Float are accepted.\n");
            return {};
        }
        const uint32_t texelBytes = UploadBytesPerPixel(format);
        if (m_device == VK_NULL_HANDLE ||
            m_graphicsQueue == VK_NULL_HANDLE ||
            m_commandPool == VK_NULL_HANDLE ||
            width == 0 || height == 0 || depth == 0 || !texels ||
            rowPitchBytes < width * texelBytes ||
            (rowPitchBytes % texelBytes) != 0u ||
            static_cast<uint64_t>(slicePitchBytes) < static_cast<uint64_t>(rowPitchBytes) * height) {
            return {};
        }

        // The staging copy assumes slices sit exactly rowPitchBytes * height apart; padded
        // slices are packed first.
        const void* uploadTexels = texels;
        std::vector<uint8_t> packed;
        const size_t packedSlice = static_cast<size_t>(rowPitchBytes) * height;
        if (slicePitchBytes != packedSlice) {
            packed.resize(packedSlice * depth);
            const auto* src = static_cast<const uint8_t*>(texels);
            for (uint32_t z = 0; z < depth; ++z) {
                std::memcpy(packed.data() + packedSlice * z, src + static_cast<size_t>(slicePitchBytes) * z, packedSlice);
            }
            uploadTexels = packed.data();
        }
        return UploadRhiTextureLayers(width, height, 1u, &uploadTexels, rowPitchBytes, format, false, depth);
    }

    RhiTextureHandle VulkanGraphicsDevice::CreateRhiTexture(const RhiTextureDesc& desc)
    {
        if (m_device == VK_NULL_HANDLE || desc.extent.width == 0 || desc.extent.height == 0) {
            return {};
        }
        if (desc.dimension != RhiResourceDimension::Texture2D) {
            return {};
        }
        // A cube view is only legal over an image created CUBE_COMPATIBLE, and that needs at least
        // one full set of 6 faces. Reject the mismatch here rather than letting vkCreateImageView
        // fail later, where the cause is much harder to see.
        if (desc.cubeCompatible && (desc.arrayLayers < 6u || (desc.arrayLayers % 6u) != 0u)) {
            DebugLog("VulkanGraphicsDevice::CreateRhiTexture: cubeCompatible requires arrayLayers to "
                     "be a non-zero multiple of 6.\n");
            return {};
        }

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.flags = desc.cubeCompatible ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = ToVkFormat(desc.format);
        imageInfo.extent = { desc.extent.width, desc.extent.height, 1u };
        imageInfo.mipLevels = desc.mipLevels;
        imageInfo.arrayLayers = desc.arrayLayers;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = ToVkImageUsage(desc.usage);
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VulkanRhiResource resource{};
        resource.extent = desc.extent;
        resource.format = desc.format;
        if (vkCreateImage(m_device, &imageInfo, nullptr, &resource.image) != VK_SUCCESS) {
            return {};
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(m_device, resource.image, &requirements);

        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, ToVkMemoryProperties(desc.memoryUsage));
        if (allocateInfo.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(m_device, &allocateInfo, nullptr, &resource.memory) != VK_SUCCESS) {
            vkDestroyImage(m_device, resource.image, nullptr);
            return {};
        }
        if (vkBindImageMemory(m_device, resource.image, resource.memory, 0) != VK_SUCCESS) {
            vkFreeMemory(m_device, resource.memory, nullptr);
            vkDestroyImage(m_device, resource.image, nullptr);
            return {};
        }

        const uint64_t id = m_nextRhiResourceHandle++;
        m_rhiResources.emplace(id, resource);
        return RhiTextureHandle{ id };
    }

    RhiBufferHandle VulkanGraphicsDevice::CreateRhiBuffer(const RhiBufferDesc& desc, const void* initialData)
    {
        if (m_device == VK_NULL_HANDLE || desc.sizeInBytes == 0) {
            return {};
        }

        VulkanRhiResource resource{};
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = desc.sizeInBytes;
        bufferInfo.usage = ToVkBufferUsage(desc.usage);
        const bool needsStagingUpload = initialData && desc.memoryUsage == RhiMemoryUsage::GpuOnly;
        if (needsStagingUpload || desc.memoryUsage == RhiMemoryUsage::GpuToCpu) {
            bufferInfo.usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        }
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        // When buffer device address is available, every buffer gets the flag so callers
        // can obtain VkDeviceAddress for BLAS/TLAS geometry descriptors.
        if (m_hasVkKhrBufferDeviceAddress) {
            bufferInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        }
        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &resource.buffer) != VK_SUCCESS) {
            return {};
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_device, resource.buffer, &requirements);

        VkMemoryAllocateFlagsInfo allocFlagsInfo{};
        allocFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        allocFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.pNext = m_hasVkKhrBufferDeviceAddress ? &allocFlagsInfo : nullptr;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, ToVkMemoryProperties(desc.memoryUsage));
        if (allocateInfo.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(m_device, &allocateInfo, nullptr, &resource.memory) != VK_SUCCESS) {
            vkDestroyBuffer(m_device, resource.buffer, nullptr);
            return {};
        }
        if (vkBindBufferMemory(m_device, resource.buffer, resource.memory, 0) != VK_SUCCESS) {
            vkFreeMemory(m_device, resource.memory, nullptr);
            vkDestroyBuffer(m_device, resource.buffer, nullptr);
            return {};
        }

        resource.sizeInBytes = desc.sizeInBytes;
        resource.memoryUsage = desc.memoryUsage;
        if (initialData && desc.memoryUsage == RhiMemoryUsage::CpuToGpu) {
            void* mapped = nullptr;
            if (vkMapMemory(m_device, resource.memory, 0, desc.sizeInBytes, 0, &mapped) == VK_SUCCESS && mapped) {
                std::memcpy(mapped, initialData, static_cast<size_t>(desc.sizeInBytes));
                vkUnmapMemory(m_device, resource.memory);
            }
        }
        if (needsStagingUpload) {
            VulkanRhiResource staging{};
            VkBufferCreateInfo stagingInfo{};
            stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            stagingInfo.size = desc.sizeInBytes;
            stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(m_device, &stagingInfo, nullptr, &staging.buffer) != VK_SUCCESS) {
                vkFreeMemory(m_device, resource.memory, nullptr);
                vkDestroyBuffer(m_device, resource.buffer, nullptr);
                return {};
            }

            VkMemoryRequirements stagingRequirements{};
            vkGetBufferMemoryRequirements(m_device, staging.buffer, &stagingRequirements);
            VkMemoryAllocateInfo stagingAllocate{};
            stagingAllocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            stagingAllocate.allocationSize = stagingRequirements.size;
            stagingAllocate.memoryTypeIndex = FindMemoryType(
                stagingRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            bool uploadSucceeded = stagingAllocate.memoryTypeIndex != UINT32_MAX &&
                vkAllocateMemory(m_device, &stagingAllocate, nullptr, &staging.memory) == VK_SUCCESS &&
                vkBindBufferMemory(m_device, staging.buffer, staging.memory, 0) == VK_SUCCESS;
            void* mapped = nullptr;
            if (uploadSucceeded) {
                uploadSucceeded = vkMapMemory(m_device, staging.memory, 0, desc.sizeInBytes, 0, &mapped) == VK_SUCCESS && mapped;
            }
            if (uploadSucceeded) {
                std::memcpy(mapped, initialData, static_cast<size_t>(desc.sizeInBytes));
                vkUnmapMemory(m_device, staging.memory);
            }

            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            if (uploadSucceeded && m_commandPool != VK_NULL_HANDLE) {
                VkCommandBufferAllocateInfo commandAllocate{};
                commandAllocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                commandAllocate.commandPool = m_commandPool;
                commandAllocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                commandAllocate.commandBufferCount = 1;
                uploadSucceeded = vkAllocateCommandBuffers(m_device, &commandAllocate, &commandBuffer) == VK_SUCCESS;
            } else {
                uploadSucceeded = false;
            }
            if (uploadSucceeded) {
                VkCommandBufferBeginInfo beginInfo{};
                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                uploadSucceeded = vkBeginCommandBuffer(commandBuffer, &beginInfo) == VK_SUCCESS;
            }
            if (uploadSucceeded) {
                VkBufferCopy copy{};
                copy.size = desc.sizeInBytes;
                vkCmdCopyBuffer(commandBuffer, staging.buffer, resource.buffer, 1, &copy);

                VkBufferMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = ToVkAccessFlags(desc.initialState);
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.buffer = resource.buffer;
                barrier.offset = 0;
                barrier.size = desc.sizeInBytes;
                vkCmdPipelineBarrier(commandBuffer,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     ToVkPipelineStage(desc.initialState),
                                     0,
                                     0,
                                     nullptr,
                                     1,
                                     &barrier,
                                     0,
                                     nullptr);
                uploadSucceeded = vkEndCommandBuffer(commandBuffer) == VK_SUCCESS;
            }
            if (uploadSucceeded) {
                VkSubmitInfo submitInfo{};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &commandBuffer;
                uploadSucceeded = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS &&
                    vkQueueWaitIdle(m_graphicsQueue) == VK_SUCCESS;
            }
            if (commandBuffer != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(m_device, m_commandPool, 1, &commandBuffer);
            }
            if (staging.memory != VK_NULL_HANDLE) {
                vkFreeMemory(m_device, staging.memory, nullptr);
            }
            vkDestroyBuffer(m_device, staging.buffer, nullptr);
            if (!uploadSucceeded) {
                vkFreeMemory(m_device, resource.memory, nullptr);
                vkDestroyBuffer(m_device, resource.buffer, nullptr);
                return {};
            }
        }

        const uint64_t id = m_nextRhiResourceHandle++;
        m_rhiResources.emplace(id, resource);
        return RhiBufferHandle{ id };
    }

    bool VulkanGraphicsDevice::UpdateRhiBuffer(RhiBufferHandle bufferHandle,
                                               uint64_t offsetInBytes,
                                               const void* data,
                                               uint64_t sizeInBytes)
    {
        const auto resourceIt = m_rhiResources.find(bufferHandle.id);
        if (resourceIt == m_rhiResources.end() || !data || sizeInBytes == 0 ||
            resourceIt->second.buffer == VK_NULL_HANDLE ||
            resourceIt->second.memory == VK_NULL_HANDLE ||
            resourceIt->second.memoryUsage != RhiMemoryUsage::CpuToGpu ||
            offsetInBytes >= resourceIt->second.sizeInBytes ||
            sizeInBytes > resourceIt->second.sizeInBytes - offsetInBytes) {
            return false;
        }

        void* mapped = nullptr;
        if (vkMapMemory(m_device,
                        resourceIt->second.memory,
                        0,
                        resourceIt->second.sizeInBytes,
                        0,
                        &mapped) != VK_SUCCESS ||
            !mapped) {
            return false;
        }
        std::memcpy(static_cast<uint8_t*>(mapped) + offsetInBytes,
                    data,
                    static_cast<size_t>(sizeInBytes));
        vkUnmapMemory(m_device, resourceIt->second.memory);
        return true;
    }

    bool VulkanGraphicsDevice::ReadRhiBuffer(RhiBufferHandle bufferHandle,
                                             uint64_t offsetInBytes,
                                             void* data,
                                             uint64_t sizeInBytes)
    {
        const auto resourceIt = m_rhiResources.find(bufferHandle.id);
        if (resourceIt == m_rhiResources.end() || !data || sizeInBytes == 0 ||
            resourceIt->second.buffer == VK_NULL_HANDLE ||
            resourceIt->second.memory == VK_NULL_HANDLE ||
            resourceIt->second.memoryUsage != RhiMemoryUsage::GpuToCpu ||
            offsetInBytes >= resourceIt->second.sizeInBytes ||
            sizeInBytes > resourceIt->second.sizeInBytes - offsetInBytes) {
            return false;
        }

        void* mapped = nullptr;
        if (vkMapMemory(m_device,
                        resourceIt->second.memory,
                        0,
                        resourceIt->second.sizeInBytes,
                        0,
                        &mapped) != VK_SUCCESS ||
            !mapped) {
            return false;
        }
        std::memcpy(data,
                    static_cast<const uint8_t*>(mapped) + offsetInBytes,
                    static_cast<size_t>(sizeInBytes));
        vkUnmapMemory(m_device, resourceIt->second.memory);
        return true;
    }

    bool VulkanGraphicsDevice::DestroyRhiResource(RhiResourceHandle resource)
    {
        const auto resourceIt = m_rhiResources.find(resource.id);
        if (!resource.IsValid() || resourceIt == m_rhiResources.end() || m_device == VK_NULL_HANDLE) {
            return false;
        }

        for (auto it = m_rhiImageViewResources.begin(); it != m_rhiImageViewResources.end();) {
            if (it->second == resource.id) {
                const auto viewIt = m_rhiImageViews.find(it->first);
                if (viewIt != m_rhiImageViews.end()) {
                    if (viewIt->second != VK_NULL_HANDLE) {
                        vkDestroyImageView(m_device, viewIt->second, nullptr);
                    }
                    m_rhiImageViews.erase(viewIt);
                }
                it = m_rhiImageViewResources.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = m_rhiDescriptors.begin(); it != m_rhiDescriptors.end();) {
            if (it->second.resourceId == resource.id) {
                it = m_rhiDescriptors.erase(it);
            } else {
                ++it;
            }
        }

        const VulkanRhiResource nativeResource = resourceIt->second;
        if (nativeResource.image != VK_NULL_HANDLE) {
            vkDestroyImage(m_device, nativeResource.image, nullptr);
        }
        if (nativeResource.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, nativeResource.buffer, nullptr);
        }
        if (nativeResource.memory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, nativeResource.memory, nullptr);
        }
        m_rhiResources.erase(resourceIt);
        return true;
    }
} // namespace SasamiEngine::Render
#endif
