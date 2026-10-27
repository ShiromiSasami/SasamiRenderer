// Back-buffer capture for the Vulkan backend.
//
// Split into its own file rather than added to VulkanGraphicsDevice.cpp, which is already past
// 2200 real lines and on the list of files to divide.
//
// The shape follows the DirectX 11 and OpenGL captures: the backend owns its whole frame, so the
// read has to happen after the UI overlay and before Present -- once presented, a discard swap
// effect leaves the back buffer undefined and there is nothing left to read.
//
// Vulkan needs more than those two did: the swapchain image has to be transitioned to
// TRANSFER_SRC, copied into a host-visible buffer, and the GPU waited on, which is why this is a
// record step inside the frame plus a read after the submit's fence.

#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"

// The header declares VulkanGraphicsDevice only under this guard, so a build configured for
// another backend compiles this file to nothing. Matches VulkanGraphicsDevice.cpp.
#if RHI_VULKAN

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Capture/PngWriter.h"

#include <cstring>
#include <string>
#include <vector>

namespace SasamiEngine::Render
{
    bool VulkanGraphicsDevice::EnsureCaptureBuffer(uint32_t width, uint32_t height)
    {
        const VkDeviceSize required = static_cast<VkDeviceSize>(width) * height * 4u;
        if (required == 0) {
            return false;
        }
        if (m_captureBuffer != VK_NULL_HANDLE && m_captureBytes >= required) {
            return true;
        }

        ReleaseCaptureBuffer();

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = required;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_captureBuffer) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice: capture buffer creation failed.\n");
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_device, m_captureBuffer, &requirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = requirements.size;
        // HOST_COHERENT so the read after the fence needs no explicit invalidate; a capture is rare
        // enough that the weaker memory type is not worth the extra call to get wrong.
        allocInfo.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &m_captureMemory) != VK_SUCCESS ||
            vkBindBufferMemory(m_device, m_captureBuffer, m_captureMemory, 0) != VK_SUCCESS ||
            vkMapMemory(m_device, m_captureMemory, 0, requirements.size, 0, &m_captureMapped) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice: capture buffer allocation or mapping failed.\n");
            ReleaseCaptureBuffer();
            return false;
        }

        m_captureBytes = required;
        return true;
    }

    void VulkanGraphicsDevice::ReleaseCaptureBuffer()
    {
        if (m_captureMapped != nullptr && m_captureMemory != VK_NULL_HANDLE) {
            vkUnmapMemory(m_device, m_captureMemory);
        }
        m_captureMapped = nullptr;
        if (m_captureBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_captureBuffer, nullptr);
            m_captureBuffer = VK_NULL_HANDLE;
        }
        if (m_captureMemory != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_captureMemory, nullptr);
            m_captureMemory = VK_NULL_HANDLE;
        }
        m_captureBytes = 0;
    }

    bool VulkanGraphicsDevice::RecordBackBufferCapture(VkCommandBuffer cmd,
                                                        uint32_t imageIndex,
                                                        VkImageLayout currentLayout,
                                                        VkAccessFlags currentAccess,
                                                        VkPipelineStageFlags currentStage)
    {
        if (!EnsureCaptureBuffer(m_swapchainExtent.width, m_swapchainExtent.height)) {
            return false;
        }

        VkImageMemoryBarrier toSource{};
        toSource.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSource.srcAccessMask = currentAccess;
        toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSource.oldLayout = currentLayout;
        toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSource.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSource.image = m_swapchainImages[imageIndex];
        toSource.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toSource.subresourceRange.levelCount = 1;
        toSource.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, currentStage, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toSource);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        // Zero means "tightly packed", which is what the PNG writer expects.
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = { 0, 0, 0 };
        region.imageExtent = { m_swapchainExtent.width, m_swapchainExtent.height, 1 };
        vkCmdCopyImageToBuffer(cmd,
                               m_swapchainImages[imageIndex],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               m_captureBuffer,
                               1,
                               &region);

        // Back to where the caller expects it, so the present transition it records afterwards
        // still starts from the layout it was written against.
        VkImageMemoryBarrier restore = toSource;
        restore.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        restore.dstAccessMask = currentAccess;
        restore.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        restore.newLayout = currentLayout;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, currentStage,
                             0, 0, nullptr, 0, nullptr, 1, &restore);
        return true;
    }

    bool VulkanGraphicsDevice::WriteCapturedBackBufferToPng(const wchar_t* path)
    {
        if (path == nullptr || m_captureMapped == nullptr) {
            return false;
        }

        const uint32_t width = m_swapchainExtent.width;
        const uint32_t height = m_swapchainExtent.height;
        const size_t pixelCount = static_cast<size_t>(width) * height;

        std::vector<uint8_t> pixels(pixelCount * 4u);
        std::memcpy(pixels.data(), m_captureMapped, pixels.size());

        // SavePngBgra8 hands the bytes to WIC as GUID_WICPixelFormat32bppBGRA, so a swapchain that
        // is already BGRA passes straight through and an RGBA one has to be swapped. This is the
        // exact trap the OpenGL capture fell into (red and blue reversed in every screenshot until
        // the function was renamed to say what it reads), so the format is checked rather than
        // assumed -- Vulkan swapchains are commonly B8G8R8A8 but nothing guarantees it.
        const bool needsSwizzle = (m_swapchainFormat == VK_FORMAT_R8G8B8A8_UNORM ||
                                   m_swapchainFormat == VK_FORMAT_R8G8B8A8_SRGB);
        if (needsSwizzle) {
            for (size_t i = 0; i < pixelCount; ++i) {
                std::swap(pixels[i * 4u + 0u], pixels[i * 4u + 2u]);
            }
        }

        return SavePngBgra8(std::wstring(path), pixels.data(), width, height);
    }
}
#endif // RHI_VULKAN
