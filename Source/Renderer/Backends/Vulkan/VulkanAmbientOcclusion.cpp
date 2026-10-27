// VulkanAmbientOcclusion.cpp
// Targets, render passes, descriptors and per-frame recording for the native Vulkan runtime-AO
// chain. The four pipelines live in VulkanAmbientOcclusionPipelines.cpp.
#include "Renderer/Backends/Vulkan/VulkanAmbientOcclusion.h"

#if RHI_VULKAN

#include "Renderer/Backends/Vulkan/VulkanAmbientOcclusionInternal.h"
#include "Renderer/Structures/Skeleton.h"

#include <cstring>
#include <iterator>

#include <windows.h>  // GetEnvironmentVariableA, for the debug-view opt-in below

// windows.h defines min/max as macros, which breaks any later use of the std:: templates. Same
// guard the DirectX 12 backend applies for the same reason.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "Foundation/Tools/DebugOutput.h"

namespace SasamiEngine::Render
{
    namespace
    {
        // Neutral encoded normal (N = 0 -> SSAO_NormalDecode.hlsli's lenSq <= 1e-5 branch -> its
        // (0,1,0) fallback) for pixels the prepass never covers.
        constexpr float kNormalClear[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
    } // namespace

    bool VulkanAmbientOcclusion::IsDebugViewEnabled()
    {
        // GetEnvironmentVariableA rather than std::getenv, matching SASAMI_GPU_VALIDATION and
        // DirectX 11's SASAMI_DX11_AO_DEBUG (std::getenv trips C4996, which this project treats
        // as an error). Read once: the value cannot change mid-run in any way worth honouring.
        static const bool enabled = [] {
            char value[8] = {};
            const DWORD length = GetEnvironmentVariableA("SASAMI_VK_AO_DEBUG", value, sizeof(value));
            return length > 0 && length < static_cast<DWORD>(sizeof(value)) && value[0] == '1';
        }();
        return enabled;
    }

    uint32_t VulkanAmbientOcclusion::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const
    {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            const bool typeAllowed = (typeBits & (1u << i)) != 0u;
            const bool hasProperties =
                (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
            if (typeAllowed && hasProperties) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool VulkanAmbientOcclusion::CreateImage(VkFormat format,
                                             VkImageUsageFlags usage,
                                             VkImageAspectFlags aspect,
                                             VkExtent2D extent,
                                             VkImage& outImage,
                                             VkDeviceMemory& outMemory,
                                             VkImageView& outView) const
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { extent.width, extent.height, 1u };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &imageInfo, nullptr, &outImage) != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: image creation failed.\n");
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(m_device, outImage, &requirements);
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex =
            FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocateInfo.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(m_device, &allocateInfo, nullptr, &outMemory) != VK_SUCCESS ||
            vkBindImageMemory(m_device, outImage, outMemory, 0) != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: image memory allocation failed.\n");
            return false;
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = outImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = { aspect, 0, 1, 0, 1 };
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &outView) != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: image view creation failed.\n");
            return false;
        }
        return true;
    }

    bool VulkanAmbientOcclusion::TransitionImageToShaderReadOnly(VkImage image, VkImageAspectFlags aspect) const
    {
        if (m_commandPool == VK_NULL_HANDLE || m_graphicsQueue == VK_NULL_HANDLE) {
            return false;
        }

        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = m_commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(m_device, &allocateInfo, &cmd) != VK_SUCCESS) {
            return false;
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
            vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
            return false;
        }

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = { aspect, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
            vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
            return false;
        }

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        const bool submitted = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS &&
                               vkQueueWaitIdle(m_graphicsQueue) == VK_SUCCESS;
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
        return submitted;
    }

    bool VulkanAmbientOcclusion::CreateWhiteFallback()
    {
        // Cleared to opaque white, so .r reads as 1.0 -- runtimeAo = 1.0, i.e. no occlusion. Same
        // "a bind always has something valid behind it" contract as VulkanShadowMap's dummy depth.
        if (!CreateImage(VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT,
                         { 1u, 1u },
                         m_whiteImage, m_whiteMemory, m_whiteView)) {
            return false;
        }

        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = m_commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(m_device, &allocateInfo, &cmd) != VK_SUCCESS) {
            return false;
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
            vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
            return false;
        }

        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = m_whiteImage;
        toTransfer.subresourceRange = range;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        VkClearColorValue white{};
        white.float32[0] = 1.0f;
        white.float32[1] = 1.0f;
        white.float32[2] = 1.0f;
        white.float32[3] = 1.0f;
        vkCmdClearColorImage(cmd, m_whiteImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);

        VkImageMemoryBarrier toRead = toTransfer;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toRead);

        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
            vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
            return false;
        }

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        const bool submitted = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS &&
                               vkQueueWaitIdle(m_graphicsQueue) == VK_SUCCESS;
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
        if (!submitted) {
            DebugLog("VulkanAmbientOcclusion: white fallback clear failed.\n");
        }
        return submitted;
    }

    bool VulkanAmbientOcclusion::CreateSamplers()
    {
        // Point-clamp: what SSAO_PS.hlsl and SSAO_Blur_PS.hlsl expect at s0. Clamping matters --
        // the kernel walks off the edge of the depth buffer at screen borders and a repeat wrap
        // would read the opposite side of the scene as a near occluder.
        VkSamplerCreateInfo pointInfo{};
        pointInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        pointInfo.magFilter = VK_FILTER_NEAREST;
        pointInfo.minFilter = VK_FILTER_NEAREST;
        pointInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        pointInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        pointInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        pointInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        pointInfo.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(m_device, &pointInfo, nullptr, &m_pointClampSampler) != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: point-clamp sampler creation failed.\n");
            return false;
        }

        VkSamplerCreateInfo linearInfo = pointInfo;
        linearInfo.magFilter = VK_FILTER_LINEAR;
        linearInfo.minFilter = VK_FILTER_LINEAR;
        linearInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        if (vkCreateSampler(m_device, &linearInfo, nullptr, &m_linearClampSampler) != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: linear-clamp sampler creation failed.\n");
            return false;
        }
        return true;
    }

    bool VulkanAmbientOcclusion::CreateTargets()
    {
        if (!CreateImage(VK_FORMAT_D32_SFLOAT,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT,
                         m_extent,
                         m_depthImage, m_depthMemory, m_depthView)) {
            return false;
        }
        if (!CreateImage(VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT,
                         m_extent,
                         m_normalImage, m_normalMemory, m_normalView)) {
            return false;
        }
        if (!CreateImage(VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT,
                         m_extent,
                         m_ssaoImage, m_ssaoMemory, m_ssaoView)) {
            return false;
        }
        // TRANSFER_SRC as well: this is the one target RecordDebugBlit copies to the back buffer.
        if (!CreateImage(VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT,
                         m_extent,
                         m_blurImage, m_blurMemory, m_blurView)) {
            return false;
        }

        // The blurred target is what AoImageView() hands out, and a caller may bind it before the
        // first Render() ever runs, so give it a defined layout up front. Its contents stay
        // undefined until the first Render(), which is why callers gate on the device's
        // "AO refreshed this frame" flag rather than on the view being non-null.
        return TransitionImageToShaderReadOnly(m_blurImage, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    bool VulkanAmbientOcclusion::CreateRenderPasses()
    {
        // --- pass 1: depth + encoded world normal ---
        {
            VkAttachmentDescription normalAttachment{};
            normalAttachment.format = VK_FORMAT_R8G8B8A8_UNORM;
            normalAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
            normalAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            normalAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            normalAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            normalAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            // Always cleared, so the previous contents (and layout) never need preserving and the
            // render pass can do its own transition from UNDEFINED -- same reasoning as
            // VulkanShadowMap's depth attachment.
            normalAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            normalAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkAttachmentDescription depthAttachment{};
            depthAttachment.format = VK_FORMAT_D32_SFLOAT;
            depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
            depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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

            // A render pass's final-layout transition orders nothing against later commands on its
            // own, so without this the SSAO pass's texture reads race the attachment writes. The
            // outgoing dependency is what makes "pass 1 writes, pass 2 samples" actually safe.
            VkSubpassDependency outgoing{};
            outgoing.srcSubpass = 0;
            outgoing.dstSubpass = VK_SUBPASS_EXTERNAL;
            outgoing.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            outgoing.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            outgoing.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            outgoing.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            const VkAttachmentDescription attachments[] = { normalAttachment, depthAttachment };
            VkRenderPassCreateInfo renderPassInfo{};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            renderPassInfo.attachmentCount = 2;
            renderPassInfo.pAttachments = attachments;
            renderPassInfo.subpassCount = 1;
            renderPassInfo.pSubpasses = &subpass;
            renderPassInfo.dependencyCount = 1;
            renderPassInfo.pDependencies = &outgoing;
            if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_prepassRenderPass) != VK_SUCCESS) {
                DebugLog("VulkanAmbientOcclusion: prepass render pass creation failed.\n");
                return false;
            }
        }

        // --- passes 2 and 3: single colour attachment, fully overwritten every frame ---
        const auto createFullscreenRenderPass = [this](VkImageUsageFlags readerUsage,
                                                       VkRenderPass& outRenderPass) -> bool {
            VkAttachmentDescription colorAttachment{};
            colorAttachment.format = VK_FORMAT_R8G8B8A8_UNORM;
            colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
            // DONT_CARE, not CLEAR: the fullscreen triangle covers every pixel in the render area.
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkAttachmentReference colorRef{};
            colorRef.attachment = 0;
            colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorRef;

            VkSubpassDependency outgoing{};
            outgoing.srcSubpass = 0;
            outgoing.dstSubpass = VK_SUBPASS_EXTERNAL;
            outgoing.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            outgoing.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            outgoing.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            outgoing.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            if ((readerUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0u) {
                // The blurred target is additionally read by RecordDebugBlit's vkCmdBlitImage.
                outgoing.dstStageMask |= VK_PIPELINE_STAGE_TRANSFER_BIT;
                outgoing.dstAccessMask |= VK_ACCESS_TRANSFER_READ_BIT;
            }

            VkRenderPassCreateInfo renderPassInfo{};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            renderPassInfo.attachmentCount = 1;
            renderPassInfo.pAttachments = &colorAttachment;
            renderPassInfo.subpassCount = 1;
            renderPassInfo.pSubpasses = &subpass;
            renderPassInfo.dependencyCount = 1;
            renderPassInfo.pDependencies = &outgoing;
            if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &outRenderPass) != VK_SUCCESS) {
                DebugLog("VulkanAmbientOcclusion: fullscreen render pass creation failed.\n");
                return false;
            }
            return true;
        };

        return createFullscreenRenderPass(0, m_ssaoRenderPass) &&
               createFullscreenRenderPass(VK_IMAGE_USAGE_TRANSFER_SRC_BIT, m_blurRenderPass);
    }

    bool VulkanAmbientOcclusion::CreateFramebuffers()
    {
        const auto createFramebuffer = [this](VkRenderPass renderPass,
                                              const VkImageView* views,
                                              uint32_t viewCount,
                                              VkFramebuffer& outFramebuffer) -> bool {
            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass;
            framebufferInfo.attachmentCount = viewCount;
            framebufferInfo.pAttachments = views;
            framebufferInfo.width = m_extent.width;
            framebufferInfo.height = m_extent.height;
            framebufferInfo.layers = 1;
            if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &outFramebuffer) != VK_SUCCESS) {
                DebugLog("VulkanAmbientOcclusion: framebuffer creation failed.\n");
                return false;
            }
            return true;
        };

        const VkImageView prepassViews[] = { m_normalView, m_depthView };
        return createFramebuffer(m_prepassRenderPass, prepassViews, 2, m_prepassFramebuffer) &&
               createFramebuffer(m_ssaoRenderPass, &m_ssaoView, 1, m_ssaoFramebuffer) &&
               createFramebuffer(m_blurRenderPass, &m_blurView, 1, m_blurFramebuffer);
    }

    bool VulkanAmbientOcclusion::CreateConstantBuffer()
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = sizeof(VulkanSsaoConstants);
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_constantBuffer) != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: constant buffer creation failed.\n");
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_device, m_constantBuffer, &requirements);
        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (allocateInfo.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(m_device, &allocateInfo, nullptr, &m_constantMemory) != VK_SUCCESS ||
            vkBindBufferMemory(m_device, m_constantBuffer, m_constantMemory, 0) != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: constant buffer memory failed.\n");
            return false;
        }
        if (vkMapMemory(m_device, m_constantMemory, 0, bufferInfo.size, 0, &m_constantMapped) != VK_SUCCESS ||
            m_constantMapped == nullptr) {
            DebugLog("VulkanAmbientOcclusion: constant buffer map failed.\n");
            return false;
        }
        return true;
    }

    bool VulkanAmbientOcclusion::CreateDescriptorSets()
    {
        // Binding numbers follow DXC's Vulkan register-shift table (ShaderCompilationService.cpp):
        // b# -> #, t# -> 100 + #, s# -> 200 + #. So SSAO_PS.hlsl's b0/t0/t1/s0 land on 0/100/101/200
        // and SSAO_Blur_PS.hlsl's b0/t0/t1/t2/s0 on 0/100/101/102/200.
        const auto makeImageBinding = [](uint32_t binding) {
            VkDescriptorSetLayoutBinding result{};
            result.binding = binding;
            result.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            result.descriptorCount = 1;
            result.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            return result;
        };

        VkDescriptorSetLayoutBinding uniformBinding{};
        uniformBinding.binding = 0;
        uniformBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uniformBinding.descriptorCount = 1;
        // DirectX 12 declares the SSAO constant buffer visible to all stages; keep the vertex stage
        // in the mask so the shared SSAO_VS.hlsl remains free to read it later without a layout change.
        uniformBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding samplerBinding{};
        samplerBinding.binding = 200;
        samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        samplerBinding.descriptorCount = 1;
        samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        {
            const VkDescriptorSetLayoutBinding bindings[] = {
                uniformBinding, makeImageBinding(100), makeImageBinding(101), samplerBinding,
            };
            VkDescriptorSetLayoutCreateInfo layoutInfo{};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.bindingCount = 4;
            layoutInfo.pBindings = bindings;
            if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_ssaoDescSetLayout) != VK_SUCCESS) {
                DebugLog("VulkanAmbientOcclusion: SSAO descriptor set layout failed.\n");
                return false;
            }
        }
        {
            const VkDescriptorSetLayoutBinding bindings[] = {
                uniformBinding, makeImageBinding(100), makeImageBinding(101), makeImageBinding(102),
                samplerBinding,
            };
            VkDescriptorSetLayoutCreateInfo layoutInfo{};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.bindingCount = 5;
            layoutInfo.pBindings = bindings;
            if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_blurDescSetLayout) != VK_SUCCESS) {
                DebugLog("VulkanAmbientOcclusion: blur descriptor set layout failed.\n");
                return false;
            }
        }

        VkDescriptorPoolSize poolSizes[3]{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = 2;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        poolSizes[1].descriptorCount = 5;  // 2 for SSAO + 3 for the blur
        poolSizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
        poolSizes[2].descriptorCount = 2;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 2;
        poolInfo.poolSizeCount = 3;
        poolInfo.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_fullscreenDescPool) != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: fullscreen descriptor pool failed.\n");
            return false;
        }

        const VkDescriptorSetLayout layouts[] = { m_ssaoDescSetLayout, m_blurDescSetLayout };
        VkDescriptorSet sets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkDescriptorSetAllocateInfo setAlloc{};
        setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAlloc.descriptorPool = m_fullscreenDescPool;
        setAlloc.descriptorSetCount = 2;
        setAlloc.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(m_device, &setAlloc, sets) != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: fullscreen descriptor set allocation failed.\n");
            return false;
        }
        m_ssaoDescSet = sets[0];
        m_blurDescSet = sets[1];

        // Every binding references a fixed resource for the module's lifetime, so the sets are
        // written once here and never touched again per frame.
        VkDescriptorBufferInfo constantInfo{};
        constantInfo.buffer = m_constantBuffer;
        constantInfo.offset = 0;
        constantInfo.range = sizeof(VulkanSsaoConstants);

        const auto imageInfoFor = [](VkImageView view) {
            VkDescriptorImageInfo info{};
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            info.imageView = view;
            return info;
        };
        const VkDescriptorImageInfo depthInfo = imageInfoFor(m_depthView);
        const VkDescriptorImageInfo normalInfo = imageInfoFor(m_normalView);
        const VkDescriptorImageInfo ssaoInfo = imageInfoFor(m_ssaoView);
        VkDescriptorImageInfo samplerInfo{};
        samplerInfo.sampler = m_pointClampSampler;

        const auto makeWrite = [](VkDescriptorSet set,
                                  uint32_t binding,
                                  VkDescriptorType type,
                                  const VkDescriptorImageInfo* image,
                                  const VkDescriptorBufferInfo* buffer) {
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = set;
            write.dstBinding = binding;
            write.descriptorType = type;
            write.descriptorCount = 1;
            write.pImageInfo = image;
            write.pBufferInfo = buffer;
            return write;
        };

        const VkWriteDescriptorSet writes[] = {
            makeWrite(m_ssaoDescSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &constantInfo),
            makeWrite(m_ssaoDescSet, 100, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &depthInfo, nullptr),
            makeWrite(m_ssaoDescSet, 101, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &normalInfo, nullptr),
            makeWrite(m_ssaoDescSet, 200, VK_DESCRIPTOR_TYPE_SAMPLER, &samplerInfo, nullptr),
            makeWrite(m_blurDescSet, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &constantInfo),
            makeWrite(m_blurDescSet, 100, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ssaoInfo, nullptr),
            makeWrite(m_blurDescSet, 101, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &depthInfo, nullptr),
            makeWrite(m_blurDescSet, 102, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &normalInfo, nullptr),
            makeWrite(m_blurDescSet, 200, VK_DESCRIPTOR_TYPE_SAMPLER, &samplerInfo, nullptr),
        };
        vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
        return true;
    }

    bool VulkanAmbientOcclusion::Initialize(VkDevice device,
                                            VkPhysicalDevice physicalDevice,
                                            VkCommandPool commandPool,
                                            VkQueue graphicsQueue,
                                            VkExtent2D extent)
    {
        Destroy();

        m_device = device;
        m_physicalDevice = physicalDevice;
        m_commandPool = commandPool;
        m_graphicsQueue = graphicsQueue;
        m_extent = extent;

        if (m_device == VK_NULL_HANDLE) {
            return false;
        }

        // The white stand-in and its sampler come first and are kept even when the rest fails, so
        // AoImageView()/AoSampler() always answer with something bindable (VulkanShadowMap applies
        // the same rule to its dummy depth view).
        if (!CreateSamplers() || !CreateWhiteFallback()) {
            DebugLog("VulkanAmbientOcclusion::Initialize: fallback resources failed.\n");
            Destroy();
            return false;
        }

        if (extent.width == 0u || extent.height == 0u) {
            return true;
        }

        if (!CreateTargets() || !CreateRenderPasses() || !CreateFramebuffers() ||
            !CreateConstantBuffer() || !CreateDescriptorSets() ||
            !CreatePrepassStaticPipeline() || !CreatePrepassSkinnedPipeline() ||
            !CreateSsaoPipeline() || !CreateBlurPipeline()) {
            // Non-fatal, exactly as a failed shadow map is: the frame renders without runtime AO
            // rather than not rendering. Everything half-built is torn down and the fallback
            // resources are rebuilt so the bindable-view contract still holds.
            DebugLog("VulkanAmbientOcclusion::Initialize: runtime AO disabled after resource "
                     "creation failure.\n");
            Destroy();
            m_device = device;
            m_physicalDevice = physicalDevice;
            m_commandPool = commandPool;
            m_graphicsQueue = graphicsQueue;
            m_extent = { 0u, 0u };
            if (!CreateSamplers() || !CreateWhiteFallback()) {
                Destroy();
                return false;
            }
            return true;
        }

        m_valid = true;
        return true;
    }

    void VulkanAmbientOcclusion::Destroy()
    {
        if (m_device != VK_NULL_HANDLE) {
            if (m_ssaoPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_ssaoPipeline, nullptr);
            if (m_ssaoPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_ssaoPipelineLayout, nullptr);
            if (m_blurPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_blurPipeline, nullptr);
            if (m_blurPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_blurPipelineLayout, nullptr);
            if (m_prepassStaticPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_prepassStaticPipeline, nullptr);
            if (m_prepassStaticPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_prepassStaticPipelineLayout, nullptr);
            if (m_prepassSkinnedPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_prepassSkinnedPipeline, nullptr);
            if (m_prepassSkinnedPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_prepassSkinnedPipelineLayout, nullptr);

            if (m_fullscreenDescPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_fullscreenDescPool, nullptr);
            if (m_ssaoDescSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_ssaoDescSetLayout, nullptr);
            if (m_blurDescSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_blurDescSetLayout, nullptr);
            if (m_boneDescPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_boneDescPool, nullptr);
            if (m_boneDescSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_boneDescSetLayout, nullptr);

            if (m_boneUboMapped != nullptr && m_boneUboMemory != VK_NULL_HANDLE) vkUnmapMemory(m_device, m_boneUboMemory);
            if (m_boneUbo != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_boneUbo, nullptr);
            if (m_boneUboMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_boneUboMemory, nullptr);
            if (m_constantMapped != nullptr && m_constantMemory != VK_NULL_HANDLE) vkUnmapMemory(m_device, m_constantMemory);
            if (m_constantBuffer != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_constantBuffer, nullptr);
            if (m_constantMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_constantMemory, nullptr);

            if (m_prepassFramebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(m_device, m_prepassFramebuffer, nullptr);
            if (m_ssaoFramebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(m_device, m_ssaoFramebuffer, nullptr);
            if (m_blurFramebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(m_device, m_blurFramebuffer, nullptr);
            if (m_prepassRenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_prepassRenderPass, nullptr);
            if (m_ssaoRenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_ssaoRenderPass, nullptr);
            if (m_blurRenderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_blurRenderPass, nullptr);

            const auto destroyImage = [this](VkImage& image, VkImageView& view, VkDeviceMemory& memory) {
                if (view != VK_NULL_HANDLE) vkDestroyImageView(m_device, view, nullptr);
                if (image != VK_NULL_HANDLE) vkDestroyImage(m_device, image, nullptr);
                if (memory != VK_NULL_HANDLE) vkFreeMemory(m_device, memory, nullptr);
                view = VK_NULL_HANDLE;
                image = VK_NULL_HANDLE;
                memory = VK_NULL_HANDLE;
            };
            destroyImage(m_depthImage, m_depthView, m_depthMemory);
            destroyImage(m_normalImage, m_normalView, m_normalMemory);
            destroyImage(m_ssaoImage, m_ssaoView, m_ssaoMemory);
            destroyImage(m_blurImage, m_blurView, m_blurMemory);
            destroyImage(m_whiteImage, m_whiteView, m_whiteMemory);

            if (m_pointClampSampler != VK_NULL_HANDLE) vkDestroySampler(m_device, m_pointClampSampler, nullptr);
            if (m_linearClampSampler != VK_NULL_HANDLE) vkDestroySampler(m_device, m_linearClampSampler, nullptr);
        }

        m_ssaoPipeline = VK_NULL_HANDLE;
        m_ssaoPipelineLayout = VK_NULL_HANDLE;
        m_blurPipeline = VK_NULL_HANDLE;
        m_blurPipelineLayout = VK_NULL_HANDLE;
        m_prepassStaticPipeline = VK_NULL_HANDLE;
        m_prepassStaticPipelineLayout = VK_NULL_HANDLE;
        m_prepassSkinnedPipeline = VK_NULL_HANDLE;
        m_prepassSkinnedPipelineLayout = VK_NULL_HANDLE;
        m_fullscreenDescPool = VK_NULL_HANDLE;
        m_ssaoDescSetLayout = VK_NULL_HANDLE;
        m_blurDescSetLayout = VK_NULL_HANDLE;
        m_ssaoDescSet = VK_NULL_HANDLE;
        m_blurDescSet = VK_NULL_HANDLE;
        m_boneDescPool = VK_NULL_HANDLE;
        m_boneDescSetLayout = VK_NULL_HANDLE;
        m_boneDescSet = VK_NULL_HANDLE;
        m_boneUbo = VK_NULL_HANDLE;
        m_boneUboMemory = VK_NULL_HANDLE;
        m_boneUboMapped = nullptr;
        m_boneSlotStride = 0;
        m_constantBuffer = VK_NULL_HANDLE;
        m_constantMemory = VK_NULL_HANDLE;
        m_constantMapped = nullptr;
        m_prepassFramebuffer = VK_NULL_HANDLE;
        m_ssaoFramebuffer = VK_NULL_HANDLE;
        m_blurFramebuffer = VK_NULL_HANDLE;
        m_prepassRenderPass = VK_NULL_HANDLE;
        m_ssaoRenderPass = VK_NULL_HANDLE;
        m_blurRenderPass = VK_NULL_HANDLE;
        m_pointClampSampler = VK_NULL_HANDLE;
        m_linearClampSampler = VK_NULL_HANDLE;
        m_extent = { 0u, 0u };
        m_valid = false;
    }

    void VulkanAmbientOcclusion::RecordPrepass(VkCommandBuffer cmd,
                                               const VulkanAoPrepassGeometry* geometry,
                                               uint32_t geometryCount,
                                               const Params& params)
    {
        VkClearValue clearValues[2]{};
        clearValues[0].color.float32[0] = kNormalClear[0];
        clearValues[0].color.float32[1] = kNormalClear[1];
        clearValues[0].color.float32[2] = kNormalClear[2];
        clearValues[0].color.float32[3] = kNormalClear[3];
        clearValues[1].depthStencil.depth = 0.0f;  // Reverse-Z: the far plane is 0.0.
        clearValues[1].depthStencil.stencil = 0;

        VkRenderPassBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = m_prepassRenderPass;
        beginInfo.framebuffer = m_prepassFramebuffer;
        beginInfo.renderArea.offset = { 0, 0 };
        beginInfo.renderArea.extent = m_extent;
        beginInfo.clearValueCount = 2;
        beginInfo.pClearValues = clearValues;
        vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

        SetFlippedViewportAndScissor(cmd, m_extent);

        VkPipeline boundPipeline = VK_NULL_HANDLE;
        uint32_t skinnedSlot = 0;
        for (uint32_t i = 0; i < geometryCount; ++i) {
            const VulkanAoPrepassGeometry& draw = geometry[i];
            if (draw.vertexBuffer == VK_NULL_HANDLE) {
                continue;
            }
            const bool wantsSkinned = draw.boneMatrices != nullptr && m_prepassSkinnedPipeline != VK_NULL_HANDLE;
            if (wantsSkinned && skinnedSlot >= kMaxAoSkinnedOccluders) {
                continue;
            }

            VulkanAoPrepassPushConstants push{};
            std::memcpy(push.viewProjection, params.viewProjection, sizeof(push.viewProjection));
            std::memcpy(push.model, draw.model != nullptr ? draw.model : kAoIdentity4x4, sizeof(push.model));

            VkPipelineLayout activeLayout = VK_NULL_HANDLE;
            if (wantsSkinned) {
                if (boundPipeline != m_prepassSkinnedPipeline) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_prepassSkinnedPipeline);
                    boundPipeline = m_prepassSkinnedPipeline;
                }
                activeLayout = m_prepassSkinnedPipelineLayout;
                const VkDeviceSize slotOffset = static_cast<VkDeviceSize>(skinnedSlot) * m_boneSlotStride;
                std::memcpy(static_cast<uint8_t*>(m_boneUboMapped) + slotOffset,
                            draw.boneMatrices,
                            static_cast<size_t>(Skeleton::kMaxBones) * 16 * sizeof(float));
                const uint32_t dynamicOffset = static_cast<uint32_t>(slotOffset);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        activeLayout, 0, 1, &m_boneDescSet, 1, &dynamicOffset);
                ++skinnedSlot;
            } else {
                if (boundPipeline != m_prepassStaticPipeline) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_prepassStaticPipeline);
                    boundPipeline = m_prepassStaticPipeline;
                }
                activeLayout = m_prepassStaticPipelineLayout;
            }
            vkCmdPushConstants(cmd, activeLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

            VkBuffer vertexBuffer = draw.vertexBuffer;
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);
            if (draw.indexCount > 0 && draw.indexBuffer != VK_NULL_HANDLE) {
                vkCmdBindIndexBuffer(cmd, draw.indexBuffer, 0,
                                     draw.index32Bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
                vkCmdDrawIndexed(cmd, draw.indexCount, 1, 0, 0, 0);
            } else if (draw.vertexCount > 0) {
                vkCmdDraw(cmd, draw.vertexCount, 1, 0, 0);
            }
        }

        vkCmdEndRenderPass(cmd);
    }

    void VulkanAmbientOcclusion::RecordFullscreenPass(VkCommandBuffer cmd,
                                                      VkRenderPass renderPass,
                                                      VkFramebuffer framebuffer,
                                                      VkPipeline pipeline,
                                                      VkPipelineLayout pipelineLayout,
                                                      VkDescriptorSet descriptorSet) const
    {
        VkRenderPassBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = renderPass;
        beginInfo.framebuffer = framebuffer;
        beginInfo.renderArea.offset = { 0, 0 };
        beginInfo.renderArea.extent = m_extent;
        beginInfo.clearValueCount = 0;  // loadOp is DONT_CARE
        vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

        SetFlippedViewportAndScissor(cmd, m_extent);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
        // SSAO_VS.hlsl builds its own oversized triangle from SV_VertexID; no vertex buffer.
        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmd);
    }

    bool VulkanAmbientOcclusion::Render(VkCommandBuffer cmd,
                                        const VulkanAoPrepassGeometry* geometry,
                                        uint32_t geometryCount,
                                        const Params& params)
    {
        if (!m_valid || geometry == nullptr || geometryCount == 0 || m_constantMapped == nullptr ||
            params.viewProjection == nullptr || params.invViewProjection == nullptr ||
            params.cameraPos == nullptr) {
            return false;
        }

        // Same field order and meaning as the SSAOCB Renderer.cpp pushes for DirectX 12, because
        // the shaders reading it are literally the same files.
        VulkanSsaoConstants constants{};
        std::memcpy(constants.cameraPV, params.viewProjection, sizeof(constants.cameraPV));
        std::memcpy(constants.cameraInvPV, params.invViewProjection, sizeof(constants.cameraInvPV));
        constants.ssaoParams[0] = params.radius;
        constants.ssaoParams[1] = params.bias;
        constants.ssaoParams[2] = params.intensity;
        constants.ssaoParams[3] = params.thickness;
        const float width = static_cast<float>(m_extent.width);
        const float height = static_cast<float>(m_extent.height);
        constants.screenSize[0] = width;
        constants.screenSize[1] = height;
        constants.screenSize[2] = width > 0.0f ? 1.0f / width : 0.0f;
        constants.screenSize[3] = height > 0.0f ? 1.0f / height : 0.0f;
        constants.cameraPos[0] = params.cameraPos[0];
        constants.cameraPos[1] = params.cameraPos[1];
        constants.cameraPos[2] = params.cameraPos[2];
        constants.cameraPos[3] = static_cast<float>(params.quality);
        std::memcpy(m_constantMapped, &constants, sizeof(constants));

        RecordPrepass(cmd, geometry, geometryCount, params);
        RecordFullscreenPass(cmd, m_ssaoRenderPass, m_ssaoFramebuffer,
                             m_ssaoPipeline, m_ssaoPipelineLayout, m_ssaoDescSet);
        RecordFullscreenPass(cmd, m_blurRenderPass, m_blurFramebuffer,
                             m_blurPipeline, m_blurPipelineLayout, m_blurDescSet);
        return true;
    }

    void VulkanAmbientOcclusion::RecordDebugBlit(VkCommandBuffer cmd, VkImage dstImage, VkExtent2D dstExtent) const
    {
        if (!m_valid || dstImage == VK_NULL_HANDLE) {
            return;
        }

        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        VkImageMemoryBarrier toSource{};
        toSource.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSource.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toSource.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSource.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toSource.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSource.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSource.image = m_blurImage;
        toSource.subresourceRange = range;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toSource);

        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[0] = { 0, 0, 0 };
        blit.srcOffsets[1] = { static_cast<int32_t>(m_extent.width), static_cast<int32_t>(m_extent.height), 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[0] = { 0, 0, 0 };
        blit.dstOffsets[1] = { static_cast<int32_t>(dstExtent.width), static_cast<int32_t>(dstExtent.height), 1 };
        vkCmdBlitImage(cmd,
                       m_blurImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_NEAREST);

        // Put it straight back: the descriptor sets written in CreateDescriptorSets() (and any
        // future consumer binding AoImageView()) declare SHADER_READ_ONLY_OPTIMAL.
        VkImageMemoryBarrier toRead = toSource;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toRead);
    }
} // namespace SasamiEngine::Render
#endif
