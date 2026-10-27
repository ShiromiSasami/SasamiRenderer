// VulkanGraphicsDeviceNativeMesh.cpp
// Pipeline, descriptor sets and per-material texture sets for the static native mesh path.
#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"
#include "Renderer/Backends/Vulkan/VulkanGraphicsDeviceUtils.h"
#include "Renderer/Resources/ShaderCompilationService.h"
#include "Renderer/Structures/Skeleton.h"

#if RHI_VULKAN

#include "Foundation/Tools/DebugOutput.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

namespace SasamiEngine::Render
{
    void VulkanGraphicsDevice::DestroyNativeMeshResources()
    {
        if (m_device != VK_NULL_HANDLE) {
            if (m_nativeMeshPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(m_device, m_nativeMeshPipeline, nullptr);
            }
            if (m_nativeMeshTransparentPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(m_device, m_nativeMeshTransparentPipeline, nullptr);
            }
            if (m_nativeMeshPipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(m_device, m_nativeMeshPipelineLayout, nullptr);
            }
            // Destroy descriptor pool first (implicitly frees all descriptor sets)
            if (m_nativeMeshDescPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(m_device, m_nativeMeshDescPool, nullptr);
            }
            if (m_nativeMeshDescSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(m_device, m_nativeMeshDescSetLayout, nullptr);
            }
            if (m_nativeMeshSampler != VK_NULL_HANDLE) {
                vkDestroySampler(m_device, m_nativeMeshSampler, nullptr);
            }
            if (m_nativeMeshDummyImageView != VK_NULL_HANDLE) {
                vkDestroyImageView(m_device, m_nativeMeshDummyImageView, nullptr);
            }
            if (m_nativeMeshDummyImage != VK_NULL_HANDLE) {
                vkDestroyImage(m_device, m_nativeMeshDummyImage, nullptr);
            }
            if (m_nativeMeshDummyMemory != VK_NULL_HANDLE) {
                vkFreeMemory(m_device, m_nativeMeshDummyMemory, nullptr);
            }
            if (m_nativeMeshAmbientCubeSampler != VK_NULL_HANDLE) {
                vkDestroySampler(m_device, m_nativeMeshAmbientCubeSampler, nullptr);
            }
            if (m_nativeMeshDummyAmbientCubeImageView != VK_NULL_HANDLE) {
                vkDestroyImageView(m_device, m_nativeMeshDummyAmbientCubeImageView, nullptr);
            }
            if (m_nativeMeshDummyAmbientCubeImage != VK_NULL_HANDLE) {
                vkDestroyImage(m_device, m_nativeMeshDummyAmbientCubeImage, nullptr);
            }
            if (m_nativeMeshDummyAmbientCubeMemory != VK_NULL_HANDLE) {
                vkFreeMemory(m_device, m_nativeMeshDummyAmbientCubeMemory, nullptr);
            }
            for (VkFramebuffer framebuffer : m_nativeMeshFramebuffers) {
                if (framebuffer != VK_NULL_HANDLE) {
                    vkDestroyFramebuffer(m_device, framebuffer, nullptr);
                }
            }
            if (m_nativeMeshRenderPass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(m_device, m_nativeMeshRenderPass, nullptr);
            }
            for (VkImageView view : m_nativeMeshDepthViews) {
                if (view != VK_NULL_HANDLE) {
                    vkDestroyImageView(m_device, view, nullptr);
                }
            }
            for (VkImage image : m_nativeMeshDepthImages) {
                if (image != VK_NULL_HANDLE) {
                    vkDestroyImage(m_device, image, nullptr);
                }
            }
            for (VkDeviceMemory memory : m_nativeMeshDepthMemory) {
                if (memory != VK_NULL_HANDLE) {
                    vkFreeMemory(m_device, memory, nullptr);
                }
            }
            if (m_nativeMeshFrameDescPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(m_device, m_nativeMeshFrameDescPool, nullptr);
            }
            if (m_nativeMeshFrameDescSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(m_device, m_nativeMeshFrameDescSetLayout, nullptr);
            }
            if (m_nativeMeshFrameUboMapped != nullptr && m_nativeMeshFrameUboMemory != VK_NULL_HANDLE) {
                vkUnmapMemory(m_device, m_nativeMeshFrameUboMemory);
            }
            if (m_nativeMeshFrameUbo != VK_NULL_HANDLE) {
                vkDestroyBuffer(m_device, m_nativeMeshFrameUbo, nullptr);
            }
            if (m_nativeMeshFrameUboMemory != VK_NULL_HANDLE) {
                vkFreeMemory(m_device, m_nativeMeshFrameUboMemory, nullptr);
            }
        }

        m_nativeShadowMap.Destroy();
        m_nativeAmbientOcclusion.Destroy();
        m_nativeAmbientOcclusionReadyThisFrame = false;

        m_nativeMeshPipeline         = VK_NULL_HANDLE;
        m_nativeMeshTransparentPipeline = VK_NULL_HANDLE;
        m_nativeMeshPipelineLayout   = VK_NULL_HANDLE;
        m_nativeMeshDescPool         = VK_NULL_HANDLE;
        m_nativeMeshDescSetLayout    = VK_NULL_HANDLE;
        m_nativeMeshSampler          = VK_NULL_HANDLE;
        m_nativeMeshDummyImageView   = VK_NULL_HANDLE;
        m_nativeMeshDummyImage       = VK_NULL_HANDLE;
        m_nativeMeshDummyMemory      = VK_NULL_HANDLE;
        m_nativeMeshAmbientCubeSampler        = VK_NULL_HANDLE;
        m_nativeMeshDummyAmbientCubeImageView = VK_NULL_HANDLE;
        m_nativeMeshDummyAmbientCubeImage     = VK_NULL_HANDLE;
        m_nativeMeshDummyAmbientCubeMemory    = VK_NULL_HANDLE;
        m_nativeMeshDescSets.clear();
        m_nativeMeshRenderPass       = VK_NULL_HANDLE;
        m_nativeMeshFramebuffers.clear();
        m_nativeMeshDepthImages.clear();
        m_nativeMeshDepthMemory.clear();
        m_nativeMeshDepthViews.clear();
        m_nativeMeshFrameDescPool       = VK_NULL_HANDLE;
        m_nativeMeshFrameDescSetLayout  = VK_NULL_HANDLE;
        m_nativeMeshFrameDescSet        = VK_NULL_HANDLE;
        m_nativeMeshFrameUbo            = VK_NULL_HANDLE;
        m_nativeMeshFrameUboMemory      = VK_NULL_HANDLE;
        m_nativeMeshFrameUboMapped      = nullptr;
    }

    bool VulkanGraphicsDevice::EnsureNativeMeshResources()
    {
        if (m_device == VK_NULL_HANDLE || m_swapchainImageViews.empty()) {
            return false;
        }
        if (m_nativeMeshPipeline != VK_NULL_HANDLE &&
            m_nativeMeshTransparentPipeline != VK_NULL_HANDLE &&
            m_nativeMeshPipelineLayout != VK_NULL_HANDLE &&
            m_nativeMeshRenderPass != VK_NULL_HANDLE &&
            m_nativeMeshFramebuffers.size() == m_swapchainImageViews.size() &&
            m_nativeMeshDepthViews.size() == m_swapchainImageViews.size() &&
            m_nativeMeshFrameDescSet != VK_NULL_HANDLE) {
            return true;
        }

        DestroyNativeMeshResources();

        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = m_swapchainFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = VK_FORMAT_D32_SFLOAT;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthRef{};
        depthRef.attachment = 1;
        depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;

        const VkAttachmentDescription attachments[] = { colorAttachment, depthAttachment };
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(std::size(attachments));
        renderPassInfo.pAttachments = attachments;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_nativeMeshRenderPass) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: vkCreateRenderPass failed.\n");
            return false;
        }

        // --- default linear-repeat sampler ---
        {
            VkSamplerCreateInfo samplerInfo{};
            samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerInfo.magFilter    = VK_FILTER_LINEAR;
            samplerInfo.minFilter    = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.maxLod       = VK_LOD_CLAMP_NONE;
            if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_nativeMeshSampler) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: sampler creation failed.\n");
                DestroyNativeMeshResources();
                return false;
            }
        }

        // --- ambient-cube sampler (clamp to edge, single mip -- same convention as the sky's
        // m_nativeSkyCubeSampler; a repeat-wrap 2D sampler would seam a cube's face edges) ---
        {
            VkSamplerCreateInfo samplerInfo{};
            samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerInfo.magFilter    = VK_FILTER_LINEAR;
            samplerInfo.minFilter    = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.maxLod       = 0.0f;
            if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_nativeMeshAmbientCubeSampler) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: ambient cube sampler creation failed.\n");
                DestroyNativeMeshResources();
                return false;
            }
        }

        // --- 1×1 white dummy texture (fallback when no albedo SRV is provided) ---
        {
            constexpr uint32_t w = 1, h = 1;
            const uint8_t whitePixel[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
            const VkDeviceSize uploadSize = 4;

            VkImageCreateInfo dummyImageInfo{};
            dummyImageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            dummyImageInfo.imageType     = VK_IMAGE_TYPE_2D;
            dummyImageInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
            dummyImageInfo.extent        = { w, h, 1u };
            dummyImageInfo.mipLevels     = 1;
            dummyImageInfo.arrayLayers   = 1;
            dummyImageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
            dummyImageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
            dummyImageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            dummyImageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
            dummyImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_device, &dummyImageInfo, nullptr, &m_nativeMeshDummyImage) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: dummy image create failed.\n");
                DestroyNativeMeshResources();
                return false;
            }

            VkMemoryRequirements dummyReqs{};
            vkGetImageMemoryRequirements(m_device, m_nativeMeshDummyImage, &dummyReqs);
            VkMemoryAllocateInfo dummyAllocInfo{};
            dummyAllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            dummyAllocInfo.allocationSize  = dummyReqs.size;
            dummyAllocInfo.memoryTypeIndex = FindMemoryType(dummyReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (dummyAllocInfo.memoryTypeIndex == UINT32_MAX ||
                vkAllocateMemory(m_device, &dummyAllocInfo, nullptr, &m_nativeMeshDummyMemory) != VK_SUCCESS ||
                vkBindImageMemory(m_device, m_nativeMeshDummyImage, m_nativeMeshDummyMemory, 0) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: dummy image memory failed.\n");
                DestroyNativeMeshResources();
                return false;
            }

            // Upload via staging buffer
            VkBuffer stagingBuf = VK_NULL_HANDLE;
            VkDeviceMemory stagingMem = VK_NULL_HANDLE;
            VkBufferCreateInfo stagingBufInfo{};
            stagingBufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            stagingBufInfo.size        = uploadSize;
            stagingBufInfo.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            stagingBufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(m_device, &stagingBufInfo, nullptr, &stagingBuf) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: dummy staging buffer failed.\n");
                DestroyNativeMeshResources();
                return false;
            }
            VkMemoryRequirements stagingReqs{};
            vkGetBufferMemoryRequirements(m_device, stagingBuf, &stagingReqs);
            VkMemoryAllocateInfo stagingAllocInfo{};
            stagingAllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            stagingAllocInfo.allocationSize  = stagingReqs.size;
            stagingAllocInfo.memoryTypeIndex = FindMemoryType(stagingReqs.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            bool stagingOk =
                stagingAllocInfo.memoryTypeIndex != UINT32_MAX &&
                vkAllocateMemory(m_device, &stagingAllocInfo, nullptr, &stagingMem) == VK_SUCCESS &&
                vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0) == VK_SUCCESS;
            if (stagingOk) {
                void* mapped = nullptr;
                stagingOk = vkMapMemory(m_device, stagingMem, 0, uploadSize, 0, &mapped) == VK_SUCCESS && mapped;
                if (stagingOk) {
                    std::memcpy(mapped, whitePixel, 4);
                    vkUnmapMemory(m_device, stagingMem);
                }
            }
            if (stagingOk) {
                VkCommandBuffer uploadCmd = VK_NULL_HANDLE;
                VkCommandBufferAllocateInfo uploadAllocInfo{};
                uploadAllocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                uploadAllocInfo.commandPool        = m_commandPool;
                uploadAllocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                uploadAllocInfo.commandBufferCount = 1;
                stagingOk = vkAllocateCommandBuffers(m_device, &uploadAllocInfo, &uploadCmd) == VK_SUCCESS;
                if (stagingOk) {
                    VkCommandBufferBeginInfo uploadBegin{};
                    uploadBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    uploadBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    stagingOk = vkBeginCommandBuffer(uploadCmd, &uploadBegin) == VK_SUCCESS;
                    if (stagingOk) {
                        VkImageMemoryBarrier toTransfer{};
                        toTransfer.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                        toTransfer.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
                        toTransfer.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
                        toTransfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        toTransfer.image               = m_nativeMeshDummyImage;
                        toTransfer.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                        vkCmdPipelineBarrier(uploadCmd,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &toTransfer);

                        VkBufferImageCopy copyRegion{};
                        copyRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                        copyRegion.imageExtent      = { w, h, 1u };
                        vkCmdCopyBufferToImage(uploadCmd, stagingBuf, m_nativeMeshDummyImage,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

                        VkImageMemoryBarrier toRead{};
                        toRead.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                        toRead.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
                        toRead.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
                        toRead.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        toRead.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        toRead.image               = m_nativeMeshDummyImage;
                        toRead.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                        vkCmdPipelineBarrier(uploadCmd,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &toRead);

                        stagingOk = vkEndCommandBuffer(uploadCmd) == VK_SUCCESS;
                    }
                    if (stagingOk) {
                        VkSubmitInfo uploadSubmit{};
                        uploadSubmit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                        uploadSubmit.commandBufferCount = 1;
                        uploadSubmit.pCommandBuffers    = &uploadCmd;
                        stagingOk =
                            vkQueueSubmit(m_graphicsQueue, 1, &uploadSubmit, VK_NULL_HANDLE) == VK_SUCCESS &&
                            vkQueueWaitIdle(m_graphicsQueue) == VK_SUCCESS;
                    }
                    vkFreeCommandBuffers(m_device, m_commandPool, 1, &uploadCmd);
                }
            }
            if (stagingMem != VK_NULL_HANDLE) vkFreeMemory(m_device, stagingMem, nullptr);
            if (stagingBuf != VK_NULL_HANDLE) vkDestroyBuffer(m_device, stagingBuf, nullptr);
            if (!stagingOk) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: dummy texture upload failed.\n");
                DestroyNativeMeshResources();
                return false;
            }

            VkImageViewCreateInfo dummyViewInfo{};
            dummyViewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            dummyViewInfo.image            = m_nativeMeshDummyImage;
            dummyViewInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
            dummyViewInfo.format           = VK_FORMAT_R8G8B8A8_UNORM;
            dummyViewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (vkCreateImageView(m_device, &dummyViewInfo, nullptr, &m_nativeMeshDummyImageView) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: dummy image view failed.\n");
                DestroyNativeMeshResources();
                return false;
            }
        }

        // --- 1x1x6 white dummy ambient cube (bound when desc.ambientCubeSrv is 0 or unresolved;
        // same shape as the sky's dummy cube in VulkanGraphicsDeviceSky.cpp) ---
        {
            constexpr uint32_t w = 1, h = 1, faceCount = 6;
            const uint8_t whitePixel[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
            const VkDeviceSize uploadSize = 4 * faceCount;

            VkImageCreateInfo dummyCubeImageInfo{};
            dummyCubeImageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            dummyCubeImageInfo.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            dummyCubeImageInfo.imageType     = VK_IMAGE_TYPE_2D;
            dummyCubeImageInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
            dummyCubeImageInfo.extent        = { w, h, 1u };
            dummyCubeImageInfo.mipLevels     = 1;
            dummyCubeImageInfo.arrayLayers   = faceCount;
            dummyCubeImageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
            dummyCubeImageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
            dummyCubeImageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            dummyCubeImageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
            dummyCubeImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_device, &dummyCubeImageInfo, nullptr, &m_nativeMeshDummyAmbientCubeImage) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: dummy ambient cube image create failed.\n");
                DestroyNativeMeshResources();
                return false;
            }

            VkMemoryRequirements dummyCubeReqs{};
            vkGetImageMemoryRequirements(m_device, m_nativeMeshDummyAmbientCubeImage, &dummyCubeReqs);
            VkMemoryAllocateInfo dummyCubeAllocInfo{};
            dummyCubeAllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            dummyCubeAllocInfo.allocationSize  = dummyCubeReqs.size;
            dummyCubeAllocInfo.memoryTypeIndex = FindMemoryType(dummyCubeReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (dummyCubeAllocInfo.memoryTypeIndex == UINT32_MAX ||
                vkAllocateMemory(m_device, &dummyCubeAllocInfo, nullptr, &m_nativeMeshDummyAmbientCubeMemory) != VK_SUCCESS ||
                vkBindImageMemory(m_device, m_nativeMeshDummyAmbientCubeImage, m_nativeMeshDummyAmbientCubeMemory, 0) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: dummy ambient cube image memory failed.\n");
                DestroyNativeMeshResources();
                return false;
            }

            // Upload via staging buffer: the same 4-byte pixel repeated once per face.
            VkBuffer stagingBuf = VK_NULL_HANDLE;
            VkDeviceMemory stagingMem = VK_NULL_HANDLE;
            VkBufferCreateInfo stagingBufInfo{};
            stagingBufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            stagingBufInfo.size        = uploadSize;
            stagingBufInfo.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            stagingBufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(m_device, &stagingBufInfo, nullptr, &stagingBuf) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: dummy ambient cube staging buffer failed.\n");
                DestroyNativeMeshResources();
                return false;
            }
            VkMemoryRequirements stagingReqs{};
            vkGetBufferMemoryRequirements(m_device, stagingBuf, &stagingReqs);
            VkMemoryAllocateInfo stagingAllocInfo{};
            stagingAllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            stagingAllocInfo.allocationSize  = stagingReqs.size;
            stagingAllocInfo.memoryTypeIndex = FindMemoryType(stagingReqs.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            bool stagingOk =
                stagingAllocInfo.memoryTypeIndex != UINT32_MAX &&
                vkAllocateMemory(m_device, &stagingAllocInfo, nullptr, &stagingMem) == VK_SUCCESS &&
                vkBindBufferMemory(m_device, stagingBuf, stagingMem, 0) == VK_SUCCESS;
            if (stagingOk) {
                void* mapped = nullptr;
                stagingOk = vkMapMemory(m_device, stagingMem, 0, uploadSize, 0, &mapped) == VK_SUCCESS && mapped;
                if (stagingOk) {
                    uint8_t* dst = static_cast<uint8_t*>(mapped);
                    for (uint32_t face = 0; face < faceCount; ++face) {
                        std::memcpy(dst + face * 4, whitePixel, 4);
                    }
                    vkUnmapMemory(m_device, stagingMem);
                }
            }
            if (stagingOk) {
                VkCommandBuffer uploadCmd = VK_NULL_HANDLE;
                VkCommandBufferAllocateInfo uploadAllocInfo{};
                uploadAllocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                uploadAllocInfo.commandPool        = m_commandPool;
                uploadAllocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                uploadAllocInfo.commandBufferCount = 1;
                stagingOk = vkAllocateCommandBuffers(m_device, &uploadAllocInfo, &uploadCmd) == VK_SUCCESS;
                if (stagingOk) {
                    VkCommandBufferBeginInfo uploadBegin{};
                    uploadBegin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    uploadBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    stagingOk = vkBeginCommandBuffer(uploadCmd, &uploadBegin) == VK_SUCCESS;
                    if (stagingOk) {
                        VkImageMemoryBarrier toTransfer{};
                        toTransfer.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                        toTransfer.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
                        toTransfer.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
                        toTransfer.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        toTransfer.image               = m_nativeMeshDummyAmbientCubeImage;
                        toTransfer.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, faceCount };
                        vkCmdPipelineBarrier(uploadCmd,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &toTransfer);

                        VkBufferImageCopy copyRegion{};
                        copyRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, faceCount };
                        copyRegion.imageExtent      = { w, h, 1u };
                        vkCmdCopyBufferToImage(uploadCmd, stagingBuf, m_nativeMeshDummyAmbientCubeImage,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

                        VkImageMemoryBarrier toRead{};
                        toRead.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                        toRead.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
                        toRead.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
                        toRead.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        toRead.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        toRead.image               = m_nativeMeshDummyAmbientCubeImage;
                        toRead.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, faceCount };
                        vkCmdPipelineBarrier(uploadCmd,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &toRead);

                        stagingOk = vkEndCommandBuffer(uploadCmd) == VK_SUCCESS;
                    }
                    if (stagingOk) {
                        VkSubmitInfo uploadSubmit{};
                        uploadSubmit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                        uploadSubmit.commandBufferCount = 1;
                        uploadSubmit.pCommandBuffers    = &uploadCmd;
                        stagingOk =
                            vkQueueSubmit(m_graphicsQueue, 1, &uploadSubmit, VK_NULL_HANDLE) == VK_SUCCESS &&
                            vkQueueWaitIdle(m_graphicsQueue) == VK_SUCCESS;
                    }
                    vkFreeCommandBuffers(m_device, m_commandPool, 1, &uploadCmd);
                }
            }
            if (stagingMem != VK_NULL_HANDLE) vkFreeMemory(m_device, stagingMem, nullptr);
            if (stagingBuf != VK_NULL_HANDLE) vkDestroyBuffer(m_device, stagingBuf, nullptr);
            if (!stagingOk) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: dummy ambient cube upload failed.\n");
                DestroyNativeMeshResources();
                return false;
            }

            VkImageViewCreateInfo dummyCubeViewInfo{};
            dummyCubeViewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            dummyCubeViewInfo.image            = m_nativeMeshDummyAmbientCubeImage;
            dummyCubeViewInfo.viewType         = VK_IMAGE_VIEW_TYPE_CUBE;
            dummyCubeViewInfo.format           = VK_FORMAT_R8G8B8A8_UNORM;
            dummyCubeViewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, faceCount };
            if (vkCreateImageView(m_device, &dummyCubeViewInfo, nullptr, &m_nativeMeshDummyAmbientCubeImageView) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: dummy ambient cube image view failed.\n");
                DestroyNativeMeshResources();
                return false;
            }
        }

        // --- descriptor set layout (t0=binding100 sampled image, s0=binding200 sampler,
        // t1=binding101 sampled ambient cube, s1=binding201 ambient sampler,
        // t2=binding102 sampled occlusion image, s2=binding202 occlusion sampler,
        // t3=binding103 sampled shadow depth, s3=binding203 shadow comparison sampler,
        // t4=binding104 sampled runtime-AO image, s4=binding204 runtime-AO sampler) ---
        {
            VkDescriptorSetLayoutBinding bindings[10]{};
            bindings[0].binding         = 100;
            bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[1].binding         = 200;
            bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[2].binding         = 101;
            bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[3].binding         = 201;
            bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
            bindings[3].descriptorCount = 1;
            bindings[3].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[4].binding         = 102;
            bindings[4].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            bindings[4].descriptorCount = 1;
            bindings[4].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[5].binding         = 202;
            bindings[5].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
            bindings[5].descriptorCount = 1;
            bindings[5].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[6].binding         = 103;
            bindings[6].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            bindings[6].descriptorCount = 1;
            bindings[6].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[7].binding         = 203;
            bindings[7].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
            bindings[7].descriptorCount = 1;
            bindings[7].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[8].binding         = 104;
            bindings[8].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            bindings[8].descriptorCount = 1;
            bindings[8].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[9].binding         = 204;
            bindings[9].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
            bindings[9].descriptorCount = 1;
            bindings[9].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo descLayoutInfo{};
            descLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            descLayoutInfo.bindingCount = static_cast<uint32_t>(std::size(bindings));
            descLayoutInfo.pBindings    = bindings;
            if (vkCreateDescriptorSetLayout(m_device, &descLayoutInfo, nullptr, &m_nativeMeshDescSetLayout) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: descriptor set layout failed.\n");
                DestroyNativeMeshResources();
                return false;
            }
        }

        // --- directional shadow map (offscreen depth target + caster pipelines + comparison
        // sampler). Non-fatal on failure: VulkanShadowMap always leaves a valid dummy depth
        // view/sampler behind even when the real map couldn't be built, so the texture descriptor
        // set below always has something valid to bind at t3/s3, and rendering simply falls back
        // to unshadowed lighting (same contract as DirectX 11's m_shadowMap). ---
        {
            // Kept in step with kNativeShadowMapResolution in RendererNativeFrame.cpp, which sizes
            // desc.shadowMapResolution and the light frustum's texel-world size to match.
            constexpr uint32_t kNativeShadowMapResolution = 2048u;
            if (!m_nativeShadowMap.Initialize(m_device, m_physicalDevice, m_commandPool, m_graphicsQueue,
                                              kNativeShadowMapResolution)) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: shadow map unavailable, "
                         "rendering without cast shadows.\n");
            }
        }

        // --- runtime (screen-space) ambient occlusion: its own depth+normal prepass, SSAO pass and
        // blur, all sized to the swapchain. Lives here, next to the shadow map, for the same
        // reason: this function is already the one that reruns whenever the swapchain image views
        // change, so a resize rebuilds the AO targets at the new extent without any extra
        // bookkeeping. Non-fatal on failure, exactly like the shadow map -- VulkanAmbientOcclusion
        // keeps a white (no-occlusion) view bindable and the frame renders without runtime AO. ---
        if (!m_nativeAmbientOcclusion.Initialize(m_device, m_physicalDevice, m_commandPool,
                                                 m_graphicsQueue, m_swapchainExtent)) {
            DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: runtime ambient occlusion "
                     "unavailable, rendering without it.\n");
        }

        // --- per-frame constants UBO + its dedicated set1 (static mesh pipeline only; the skinned
        // pipeline instead adds a second binding to its existing set1 bone descriptor set -- see
        // VulkanGraphicsDeviceSkinnedMesh.cpp) ---
        {
            VkBufferCreateInfo frameBufInfo{};
            frameBufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            frameBufInfo.size = sizeof(VulkanNativeMeshFrameConstants);
            frameBufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            frameBufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(m_device, &frameBufInfo, nullptr, &m_nativeMeshFrameUbo) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: frame UBO creation failed.\n");
                DestroyNativeMeshResources();
                return false;
            }

            VkMemoryRequirements frameReqs{};
            vkGetBufferMemoryRequirements(m_device, m_nativeMeshFrameUbo, &frameReqs);
            VkMemoryAllocateInfo frameAllocInfo{};
            frameAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            frameAllocInfo.allocationSize = frameReqs.size;
            frameAllocInfo.memoryTypeIndex = FindMemoryType(frameReqs.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (frameAllocInfo.memoryTypeIndex == UINT32_MAX ||
                vkAllocateMemory(m_device, &frameAllocInfo, nullptr, &m_nativeMeshFrameUboMemory) != VK_SUCCESS ||
                vkBindBufferMemory(m_device, m_nativeMeshFrameUbo, m_nativeMeshFrameUboMemory, 0) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: frame UBO memory failed.\n");
                DestroyNativeMeshResources();
                return false;
            }
            if (vkMapMemory(m_device, m_nativeMeshFrameUboMemory, 0, frameBufInfo.size, 0,
                            &m_nativeMeshFrameUboMapped) != VK_SUCCESS || !m_nativeMeshFrameUboMapped) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: frame UBO map failed.\n");
                DestroyNativeMeshResources();
                return false;
            }

            VkDescriptorSetLayoutBinding frameBinding{};
            frameBinding.binding = 0;
            frameBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            frameBinding.descriptorCount = 1;
            frameBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo frameLayoutInfo{};
            frameLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            frameLayoutInfo.bindingCount = 1;
            frameLayoutInfo.pBindings = &frameBinding;
            if (vkCreateDescriptorSetLayout(m_device, &frameLayoutInfo, nullptr, &m_nativeMeshFrameDescSetLayout) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: frame descriptor set layout failed.\n");
                DestroyNativeMeshResources();
                return false;
            }

            VkDescriptorPoolSize framePoolSize{};
            framePoolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            framePoolSize.descriptorCount = 1;
            VkDescriptorPoolCreateInfo framePoolInfo{};
            framePoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            framePoolInfo.maxSets = 1;
            framePoolInfo.poolSizeCount = 1;
            framePoolInfo.pPoolSizes = &framePoolSize;
            if (vkCreateDescriptorPool(m_device, &framePoolInfo, nullptr, &m_nativeMeshFrameDescPool) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: frame descriptor pool failed.\n");
                DestroyNativeMeshResources();
                return false;
            }

            VkDescriptorSetAllocateInfo frameSetAlloc{};
            frameSetAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            frameSetAlloc.descriptorPool = m_nativeMeshFrameDescPool;
            frameSetAlloc.descriptorSetCount = 1;
            frameSetAlloc.pSetLayouts = &m_nativeMeshFrameDescSetLayout;
            if (vkAllocateDescriptorSets(m_device, &frameSetAlloc, &m_nativeMeshFrameDescSet) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: frame descriptor set alloc failed.\n");
                DestroyNativeMeshResources();
                return false;
            }

            VkDescriptorBufferInfo frameBufferInfo{};
            frameBufferInfo.buffer = m_nativeMeshFrameUbo;
            frameBufferInfo.range = sizeof(VulkanNativeMeshFrameConstants);
            VkWriteDescriptorSet frameWrite{};
            frameWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            frameWrite.dstSet = m_nativeMeshFrameDescSet;
            frameWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            frameWrite.descriptorCount = 1;
            frameWrite.pBufferInfo = &frameBufferInfo;
            vkUpdateDescriptorSets(m_device, 1, &frameWrite, 0, nullptr);
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(VulkanNativeMeshPushConstants);

        const VkDescriptorSetLayout nativeMeshSetLayouts[] = {
            m_nativeMeshDescSetLayout,       // set0: textures (albedo/ambient/occlusion/shadow)
            m_nativeMeshFrameDescSetLayout,  // set1: per-frame constants
        };
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount         = static_cast<uint32_t>(std::size(nativeMeshSetLayouts));
        layoutInfo.pSetLayouts            = nativeMeshSetLayouts;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges    = &pushRange;
        if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_nativeMeshPipelineLayout) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: vkCreatePipelineLayout failed.\n");
            return false;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
        const std::filesystem::path shaderPath =
            ShaderCompilationService::GetShaderSourceRoot() / L"Backend" / L"Native" / L"NativeMesh.hlsl";
        if (!ShaderCompilationService::CompileShader(shaderPath, "VSMain", "vs_6_0", vsBlob, true) ||
            !ShaderCompilationService::CompileShader(shaderPath, "PSMain", "ps_6_0", psBlob, true)) {
            DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: SPIR-V shader compile failed.\n");
            return false;
        }

        VkShaderModuleCreateInfo vsInfo{};
        vsInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vsInfo.codeSize = vsBlob->GetBufferSize();
        vsInfo.pCode = static_cast<const uint32_t*>(vsBlob->GetBufferPointer());
        VkShaderModule vsModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &vsInfo, nullptr, &vsModule) != VK_SUCCESS) {
            return false;
        }

        VkShaderModuleCreateInfo psInfo{};
        psInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        psInfo.codeSize = psBlob->GetBufferSize();
        psInfo.pCode = static_cast<const uint32_t*>(psBlob->GetBufferPointer());
        VkShaderModule psModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &psInfo, nullptr, &psModule) != VK_SUCCESS) {
            vkDestroyShaderModule(m_device, vsModule, nullptr);
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vsModule;
        stages[0].pName = "VSMain";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = psModule;
        stages[1].pName = "PSMain";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = 48;
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 4> attributes = {
            MakeNativeMeshAttribute(0, VK_FORMAT_R32G32B32_SFLOAT, 0),
            MakeNativeMeshAttribute(1, VK_FORMAT_R32G32B32_SFLOAT, 12),
            MakeNativeMeshAttribute(2, VK_FORMAT_R32G32B32A32_SFLOAT, 24),
            MakeNativeMeshAttribute(3, VK_FORMAT_R32G32_SFLOAT, 40),
        };

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;  // Reverse-Z: near=1.0, far=0.0, so the nearest fragment wins with >=

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        // Opaque pass: no blending. The transparent copy below flips this on.
        blendAttachment.blendEnable = VK_FALSE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = m_nativeMeshPipelineLayout;
        pipelineInfo.renderPass = m_nativeMeshRenderPass;

        // Opaque pipeline: depthStencil/blendAttachment above are still in their opaque state
        // (depth write on, blend off). vkCreateGraphicsPipelines reads them by pointer at call
        // time, so the transparent variant below is built by mutating the same structs in place.
        const VkResult pipelineResult =
            vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_nativeMeshPipeline);
        if (pipelineResult != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: vkCreateGraphicsPipelines failed.\n");
            vkDestroyShaderModule(m_device, psModule, nullptr);
            vkDestroyShaderModule(m_device, vsModule, nullptr);
            return false;
        }

        // Transparent pipeline: keep depth testing so opaque geometry still occludes it, but
        // stop writing depth so overlapping transparent draws don't occlude one another out of
        // back-to-front order, and turn on standard alpha blending.
        blendAttachment.blendEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_FALSE;
        const VkResult transparentPipelineResult = vkCreateGraphicsPipelines(
            m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_nativeMeshTransparentPipeline);
        vkDestroyShaderModule(m_device, psModule, nullptr);
        vkDestroyShaderModule(m_device, vsModule, nullptr);
        if (transparentPipelineResult != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: transparent vkCreateGraphicsPipelines failed.\n");
            return false;
        }

        const size_t imageCount = m_swapchainImageViews.size();
        m_nativeMeshDepthImages.resize(imageCount, VK_NULL_HANDLE);
        m_nativeMeshDepthMemory.resize(imageCount, VK_NULL_HANDLE);
        m_nativeMeshDepthViews.resize(imageCount, VK_NULL_HANDLE);
        m_nativeMeshFramebuffers.resize(imageCount, VK_NULL_HANDLE);

        for (size_t i = 0; i < m_swapchainImageViews.size(); ++i) {
            VkImageCreateInfo depthImageInfo{};
            depthImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            depthImageInfo.imageType = VK_IMAGE_TYPE_2D;
            depthImageInfo.format = VK_FORMAT_D32_SFLOAT;
            depthImageInfo.extent = { m_swapchainExtent.width, m_swapchainExtent.height, 1u };
            depthImageInfo.mipLevels = 1;
            depthImageInfo.arrayLayers = 1;
            depthImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            depthImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            // SAMPLED_BIT so a later screen-space pass (ambient occlusion) can read this depth
            // back as a texture. Requesting it at creation costs nothing when unused, and the flag
            // cannot be added afterwards without recreating every swapchain-sized image.
            depthImageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            depthImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            depthImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_device, &depthImageInfo, nullptr, &m_nativeMeshDepthImages[i]) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: depth image creation failed.\n");
                DestroyNativeMeshResources();
                return false;
            }

            VkMemoryRequirements depthRequirements{};
            vkGetImageMemoryRequirements(m_device, m_nativeMeshDepthImages[i], &depthRequirements);
            VkMemoryAllocateInfo depthAllocateInfo{};
            depthAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            depthAllocateInfo.allocationSize = depthRequirements.size;
            depthAllocateInfo.memoryTypeIndex =
                FindMemoryType(depthRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (depthAllocateInfo.memoryTypeIndex == UINT32_MAX ||
                vkAllocateMemory(m_device, &depthAllocateInfo, nullptr, &m_nativeMeshDepthMemory[i]) != VK_SUCCESS ||
                vkBindImageMemory(m_device, m_nativeMeshDepthImages[i], m_nativeMeshDepthMemory[i], 0) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: depth memory allocation failed.\n");
                DestroyNativeMeshResources();
                return false;
            }

            VkImageViewCreateInfo depthViewInfo{};
            depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            depthViewInfo.image = m_nativeMeshDepthImages[i];
            depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            depthViewInfo.format = VK_FORMAT_D32_SFLOAT;
            depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            depthViewInfo.subresourceRange.baseMipLevel = 0;
            depthViewInfo.subresourceRange.levelCount = 1;
            depthViewInfo.subresourceRange.baseArrayLayer = 0;
            depthViewInfo.subresourceRange.layerCount = 1;
            if (vkCreateImageView(m_device, &depthViewInfo, nullptr, &m_nativeMeshDepthViews[i]) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: depth view creation failed.\n");
                DestroyNativeMeshResources();
                return false;
            }

            VkImageView framebufferAttachments[] = {
                m_swapchainImageViews[i],
                m_nativeMeshDepthViews[i],
            };
            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = m_nativeMeshRenderPass;
            framebufferInfo.attachmentCount = static_cast<uint32_t>(std::size(framebufferAttachments));
            framebufferInfo.pAttachments = framebufferAttachments;
            framebufferInfo.width = m_swapchainExtent.width;
            framebufferInfo.height = m_swapchainExtent.height;
            framebufferInfo.layers = 1;
            if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_nativeMeshFramebuffers[i]) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: vkCreateFramebuffer failed.\n");
                DestroyNativeMeshResources();
                return false;
            }
        }

        // --- descriptor pool (1024 sets for unique albedo/ambient-cube/occlusion combinations; each
        // set now consumes five SAMPLED_IMAGE and five SAMPLER descriptors, one pair per binding
        // pair -- the fourth being the shadow depth view/comparison sampler at t3/s3 and the fifth
        // the runtime-AO view/sampler at t4/s4. The counts are 1024 x 5, so they have to grow with
        // every binding pair added or allocation starts failing part-way through a scene. ---
        {
            VkDescriptorPoolSize poolSizes[2]{};
            poolSizes[0].type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            poolSizes[0].descriptorCount = 5120;
            poolSizes[1].type            = VK_DESCRIPTOR_TYPE_SAMPLER;
            poolSizes[1].descriptorCount = 5120;
            VkDescriptorPoolCreateInfo poolInfo{};
            poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.maxSets       = 1024;
            poolInfo.poolSizeCount = 2;
            poolInfo.pPoolSizes    = poolSizes;
            if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_nativeMeshDescPool) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: descriptor pool failed.\n");
                DestroyNativeMeshResources();
                return false;
            }
        }

        // Pre-allocate descriptor set for key=(0, 0, 0) (no albedo / no ambient cube / no occlusion
        // -- the dummy fallback for all three binding pairs). The occlusion binding reuses the same
        // 1x1 white dummy as albedo: a white R channel reads as materialAo=1.0, i.e. "fully
        // unoccluded", which is exactly the neutral value needed when there is no AO texture.
        {
            VkDescriptorSetAllocateInfo dummyAlloc{};
            dummyAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dummyAlloc.descriptorPool     = m_nativeMeshDescPool;
            dummyAlloc.descriptorSetCount = 1;
            dummyAlloc.pSetLayouts        = &m_nativeMeshDescSetLayout;
            VkDescriptorSet dummySet = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(m_device, &dummyAlloc, &dummySet) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: dummy descriptor set alloc failed.\n");
                DestroyNativeMeshResources();
                return false;
            }
            VkDescriptorImageInfo dummyImageInfo{};
            dummyImageInfo.imageView   = m_nativeMeshDummyImageView;
            dummyImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorImageInfo dummySamplerInfo{};
            dummySamplerInfo.sampler = m_nativeMeshSampler;
            VkDescriptorImageInfo dummyAmbientImageInfo{};
            dummyAmbientImageInfo.imageView   = m_nativeMeshDummyAmbientCubeImageView;
            dummyAmbientImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorImageInfo dummyAmbientSamplerInfo{};
            dummyAmbientSamplerInfo.sampler = m_nativeMeshAmbientCubeSampler;
            // Always valid regardless of whether the real shadow map exists this run -- see
            // VulkanShadowMap::ImageView()/ComparisonSampler().
            VkDescriptorImageInfo dummyShadowImageInfo{};
            dummyShadowImageInfo.imageView   = m_nativeShadowMap.ImageView();
            dummyShadowImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorImageInfo dummyShadowSamplerInfo{};
            dummyShadowSamplerInfo.sampler = m_nativeShadowMap.ComparisonSampler();
            // Normally always valid, real or white-dummy (see VulkanAmbientOcclusion::AoImageView()),
            // but fall back to this path's own 1x1 white image if even the AO module's fallback
            // failed to build: a null imageView in a descriptor write is undefined behaviour, and
            // the albedo dummy reads 1.0 in R, which is the same no-occlusion value.
            VkDescriptorImageInfo dummyAoImageInfo{};
            dummyAoImageInfo.imageView   = m_nativeAmbientOcclusion.AoImageView() != VK_NULL_HANDLE
                ? m_nativeAmbientOcclusion.AoImageView()
                : m_nativeMeshDummyImageView;
            dummyAoImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorImageInfo dummyAoSamplerInfo{};
            dummyAoSamplerInfo.sampler = m_nativeAmbientOcclusion.AoSampler() != VK_NULL_HANDLE
                ? m_nativeAmbientOcclusion.AoSampler()
                : m_nativeMeshSampler;
            VkWriteDescriptorSet writes[10]{};
            writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet          = dummySet;
            writes[0].dstBinding      = 100;
            writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[0].descriptorCount = 1;
            writes[0].pImageInfo      = &dummyImageInfo;
            writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet          = dummySet;
            writes[1].dstBinding      = 200;
            writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[1].descriptorCount = 1;
            writes[1].pImageInfo      = &dummySamplerInfo;
            writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet          = dummySet;
            writes[2].dstBinding      = 101;
            writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[2].descriptorCount = 1;
            writes[2].pImageInfo      = &dummyAmbientImageInfo;
            writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[3].dstSet          = dummySet;
            writes[3].dstBinding      = 201;
            writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[3].descriptorCount = 1;
            writes[3].pImageInfo      = &dummyAmbientSamplerInfo;
            writes[4].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[4].dstSet          = dummySet;
            writes[4].dstBinding      = 102;
            writes[4].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[4].descriptorCount = 1;
            writes[4].pImageInfo      = &dummyImageInfo;
            writes[5].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[5].dstSet          = dummySet;
            writes[5].dstBinding      = 202;
            writes[5].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[5].descriptorCount = 1;
            writes[5].pImageInfo      = &dummySamplerInfo;
            writes[6].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[6].dstSet          = dummySet;
            writes[6].dstBinding      = 103;
            writes[6].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[6].descriptorCount = 1;
            writes[6].pImageInfo      = &dummyShadowImageInfo;
            writes[7].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[7].dstSet          = dummySet;
            writes[7].dstBinding      = 203;
            writes[7].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[7].descriptorCount = 1;
            writes[7].pImageInfo      = &dummyShadowSamplerInfo;
            writes[8].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[8].dstSet          = dummySet;
            writes[8].dstBinding      = 104;
            writes[8].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[8].descriptorCount = 1;
            writes[8].pImageInfo      = &dummyAoImageInfo;
            writes[9].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[9].dstSet          = dummySet;
            writes[9].dstBinding      = 204;
            writes[9].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[9].descriptorCount = 1;
            writes[9].pImageInfo      = &dummyAoSamplerInfo;
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
            m_nativeMeshDescSets[0][0][0] = dummySet;
        }

        return true;
    }

    VkDescriptorSet VulkanGraphicsDevice::GetOrCreateNativeMeshTextureDescriptorSet(
        uint64_t albedoSrv, uint64_t ambientCubeSrv, uint64_t occlusionSrv)
    {
        // Resolve each srv to its actual view up front; an unresolved handle (0, or a stale one
        // that failed to look up) folds into the (0, 0, 0) key below, same fallback the
        // single-binding cache always had. occlusionSrv is expected to already be 0 when the caller
        // decided not to bind a real AO texture (occlusionSlotHasAo is false) -- see the call sites
        // in VulkanGraphicsDevice.cpp.
        VkImageView albedoView = VK_NULL_HANDLE;
        if (albedoSrv != 0) {
            auto it = m_rhiImageViews.find(albedoSrv);
            if (it != m_rhiImageViews.end() && it->second != VK_NULL_HANDLE) {
                albedoView = it->second;
            } else {
                albedoSrv = 0;
            }
        }
        VkImageView ambientView = VK_NULL_HANDLE;
        if (ambientCubeSrv != 0) {
            auto it = m_rhiImageViews.find(ambientCubeSrv);
            if (it != m_rhiImageViews.end() && it->second != VK_NULL_HANDLE) {
                ambientView = it->second;
            } else {
                ambientCubeSrv = 0;
            }
        }
        VkImageView occlusionView = VK_NULL_HANDLE;
        if (occlusionSrv != 0) {
            auto it = m_rhiImageViews.find(occlusionSrv);
            if (it != m_rhiImageViews.end() && it->second != VK_NULL_HANDLE) {
                occlusionView = it->second;
            } else {
                occlusionSrv = 0;
            }
        }

        auto& byAmbient = m_nativeMeshDescSets[albedoSrv];
        auto& byOcclusion = byAmbient[ambientCubeSrv];
        auto found = byOcclusion.find(occlusionSrv);
        if (found != byOcclusion.end()) {
            return found->second;
        }
        if (albedoSrv == 0 && ambientCubeSrv == 0 && occlusionSrv == 0) {
            return byOcclusion[0]; // pre-allocated above in EnsureNativeMeshResources
        }
        if (albedoView == VK_NULL_HANDLE) {
            albedoView = m_nativeMeshDummyImageView;
        }
        if (ambientView == VK_NULL_HANDLE) {
            ambientView = m_nativeMeshDummyAmbientCubeImageView;
        }
        // Same 1x1 white dummy as albedo: an unoccluded material reads materialAo=1.0 from it (see
        // NativeMesh.hlsl), matching "AO texture absent" rendering exactly as before this existed.
        if (occlusionView == VK_NULL_HANDLE) {
            occlusionView = m_nativeMeshDummyImageView;
        }

        VkDescriptorSetAllocateInfo texAlloc{};
        texAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        texAlloc.descriptorPool     = m_nativeMeshDescPool;
        texAlloc.descriptorSetCount = 1;
        texAlloc.pSetLayouts        = &m_nativeMeshDescSetLayout;
        VkDescriptorSet newSet = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &texAlloc, &newSet) != VK_SUCCESS) {
            return m_nativeMeshDescSets[0][0][0];
        }

        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageView   = albedoView;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo smpInfo{};
        smpInfo.sampler = m_nativeMeshSampler;
        VkDescriptorImageInfo ambientImgInfo{};
        ambientImgInfo.imageView   = ambientView;
        ambientImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo ambientSmpInfo{};
        ambientSmpInfo.sampler = m_nativeMeshAmbientCubeSampler;
        VkDescriptorImageInfo occlusionImgInfo{};
        occlusionImgInfo.imageView   = occlusionView;
        occlusionImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo occlusionSmpInfo{};
        // Same linear/repeat sampler as albedo: the occlusion image shares the albedo's UV set and
        // wrap conventions (it's either the same packed ORM texture or absent).
        occlusionSmpInfo.sampler = m_nativeMeshSampler;
        // Shadow map view/sampler are the same for every texture-set combination (there is only
        // ever one shadow map), so every newly created set just points at VulkanShadowMap's
        // current view/sampler -- always valid, real or dummy (see ImageView()'s comment).
        VkDescriptorImageInfo shadowImgInfo{};
        shadowImgInfo.imageView   = m_nativeShadowMap.ImageView();
        shadowImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo shadowSmpInfo{};
        shadowSmpInfo.sampler = m_nativeShadowMap.ComparisonSampler();
        // Likewise one runtime-AO texture for the whole frame, bound into every set. Linear-clamp
        // rather than the albedo's linear-repeat: this is a screen-space lookup, so a wrap at the
        // edges would read the opposite side of the frame.
        VkDescriptorImageInfo aoImgInfo{};
        aoImgInfo.imageView   = m_nativeAmbientOcclusion.AoImageView() != VK_NULL_HANDLE
            ? m_nativeAmbientOcclusion.AoImageView()
            : m_nativeMeshDummyImageView;
        aoImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo aoSmpInfo{};
        aoSmpInfo.sampler = m_nativeAmbientOcclusion.AoSampler() != VK_NULL_HANDLE
            ? m_nativeAmbientOcclusion.AoSampler()
            : m_nativeMeshSampler;

        VkWriteDescriptorSet writes[10]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = newSet;
        writes[0].dstBinding      = 100;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo      = &imgInfo;
        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = newSet;
        writes[1].dstBinding      = 200;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo      = &smpInfo;
        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = newSet;
        writes[2].dstBinding      = 101;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[2].descriptorCount = 1;
        writes[2].pImageInfo      = &ambientImgInfo;
        writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet          = newSet;
        writes[3].dstBinding      = 201;
        writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[3].descriptorCount = 1;
        writes[3].pImageInfo      = &ambientSmpInfo;
        writes[4].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet          = newSet;
        writes[4].dstBinding      = 102;
        writes[4].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[4].descriptorCount = 1;
        writes[4].pImageInfo      = &occlusionImgInfo;
        writes[5].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[5].dstSet          = newSet;
        writes[5].dstBinding      = 202;
        writes[5].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[5].descriptorCount = 1;
        writes[5].pImageInfo      = &occlusionSmpInfo;
        writes[6].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[6].dstSet          = newSet;
        writes[6].dstBinding      = 103;
        writes[6].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[6].descriptorCount = 1;
        writes[6].pImageInfo      = &shadowImgInfo;
        writes[7].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[7].dstSet          = newSet;
        writes[7].dstBinding      = 203;
        writes[7].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[7].descriptorCount = 1;
        writes[7].pImageInfo      = &shadowSmpInfo;
        writes[8].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[8].dstSet          = newSet;
        writes[8].dstBinding      = 104;
        writes[8].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[8].descriptorCount = 1;
        writes[8].pImageInfo      = &aoImgInfo;
        writes[9].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[9].dstSet          = newSet;
        writes[9].dstBinding      = 204;
        writes[9].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[9].descriptorCount = 1;
        writes[9].pImageInfo      = &aoSmpInfo;
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);

        byOcclusion[occlusionSrv] = newSet;
        return newSet;
    }
} // namespace SasamiEngine::Render
#endif
