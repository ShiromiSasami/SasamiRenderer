// VulkanGraphicsDevice.cpp
// Vulkan ExecuteBackendFrame, D3D12 compatibility wrappers.
#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"
#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice_Utils.h"
#include "Renderer/Resources/ShaderCompilationService.h"
#include "Foundation/Math/MathUtil.h"

#if RHI_VULKAN

#include "Foundation/Tools/DebugOutput.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>
#include <wrl/client.h>


namespace SasamiRenderer
{
    namespace
    {
        struct VulkanNativeMeshPushConstants
        {
            float modelViewProjection[16];
            float baseColor[4];
            float lightDirIntensity[4];
            float lightColor[4];
            float emissiveRoughness[4];
        };

        static_assert(sizeof(VulkanNativeMeshPushConstants) == 128,
                      "Native Vulkan mesh push constants must stay within the guaranteed 128-byte minimum.");

        VkVertexInputAttributeDescription MakeNativeMeshAttribute(uint32_t location,
                                                                  VkFormat format,
                                                                  uint32_t offset)
        {
            VkVertexInputAttributeDescription attribute{};
            attribute.location = location;
            attribute.binding = 0;
            attribute.format = format;
            attribute.offset = offset;
            return attribute;
        }
    }

    bool VulkanGraphicsDevice::ExecuteBackendFrame(const RhiBackendFrameDesc& frameDesc)
    {
        if (m_device == VK_NULL_HANDLE ||
            m_swapchain == VK_NULL_HANDLE ||
            m_commandBuffers.empty() ||
            m_frameFences.empty() ||
            m_swapchainImages.empty() ||
            !frameDesc.present) {
            return false;
        }

        const uint32_t frame = m_currentFrame % static_cast<uint32_t>(m_commandBuffers.size());
        vkWaitForFences(m_device, 1, &m_frameFences[frame], VK_TRUE, UINT64_MAX);

        uint32_t imageIndex = 0;
        VkResult result = vkAcquireNextImageKHR(m_device,
                                                m_swapchain,
                                                UINT64_MAX,
                                                m_imageAvailableSemaphores[frame],
                                                VK_NULL_HANDLE,
                                                &imageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            ResizeBackendSwapChain(m_swapchainExtent.width, m_swapchainExtent.height);
            return false;
        }
        const bool recreateAfterPresent = result == VK_SUBOPTIMAL_KHR;
        if (result != VK_SUCCESS && !recreateAfterPresent) {
            return false;
        }
        if (imageIndex >= m_swapchainImages.size()) {
            return false;
        }

        VkCommandBuffer cmd = m_commandBuffers[frame];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(cmd, &beginInfo);
        if (result != VK_SUCCESS) {
            return false;
        }

        VkImageLayout frameOutputLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkAccessFlags frameOutputAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
        VkPipelineStageFlags frameOutputStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

        if (frameDesc.rayMarch.enabled &&
            RenderRayMarchFrame(imageIndex, cmd, frameDesc.rayMarch, frameDesc.clearColor)) {
            frameOutputLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            frameOutputAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            frameOutputStage  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        } else if (frameDesc.mesh.enabled && frameDesc.mesh.draws && frameDesc.mesh.drawCount > 0 &&
            RenderMeshFrame(frame, imageIndex, cmd, frameDesc.mesh, frameDesc.clearColor)) {
            frameOutputLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            frameOutputAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            frameOutputStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        } else {
            VkImageMemoryBarrier toTransfer{};
            toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toTransfer.srcAccessMask = 0;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toTransfer.oldLayout = m_swapchainImageLayouts[imageIndex];
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.image = m_swapchainImages[imageIndex];
            toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toTransfer.subresourceRange.baseMipLevel = 0;
            toTransfer.subresourceRange.levelCount = 1;
            toTransfer.subresourceRange.baseArrayLayer = 0;
            toTransfer.subresourceRange.layerCount = 1;

            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0,
                                 0,
                                 nullptr,
                                 0,
                                 nullptr,
                                 1,
                                 &toTransfer);

            VkClearColorValue vkClear{};
            vkClear.float32[0] = frameDesc.clearColor.r;
            vkClear.float32[1] = frameDesc.clearColor.g;
            vkClear.float32[2] = frameDesc.clearColor.b;
            vkClear.float32[3] = frameDesc.clearColor.a;

            VkImageSubresourceRange colorRange{};
            colorRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            colorRange.baseMipLevel = 0;
            colorRange.levelCount = 1;
            colorRange.baseArrayLayer = 0;
            colorRange.layerCount = 1;
            vkCmdClearColorImage(cmd,
                                 m_swapchainImages[imageIndex],
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &vkClear,
                                 1,
                                 &colorRange);
        }

        VkImageMemoryBarrier toPresent{};
        toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toPresent.srcAccessMask = frameOutputAccess;
        toPresent.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        toPresent.oldLayout = frameOutputLayout;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image = m_swapchainImages[imageIndex];
        toPresent.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toPresent.subresourceRange.baseMipLevel = 0;
        toPresent.subresourceRange.levelCount = 1;
        toPresent.subresourceRange.baseArrayLayer = 0;
        toPresent.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd,
                             frameOutputStage,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &toPresent);

        result = vkEndCommandBuffer(cmd);
        if (result != VK_SUCCESS) {
            return false;
        }

        const VkPipelineStageFlags waitStage = frameOutputStage;
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &m_imageAvailableSemaphores[frame];
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &m_renderFinishedSemaphores[frame];

        vkResetFences(m_device, 1, &m_frameFences[frame]);
        result = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_frameFences[frame]);
        if (result != VK_SUCCESS) {
            return false;
        }

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &m_renderFinishedSemaphores[frame];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &m_swapchain;
        presentInfo.pImageIndices = &imageIndex;
        result = vkQueuePresentKHR(m_presentQueue, &presentInfo);
        const bool presentNeedsRecreate = result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR;
        if (result != VK_SUCCESS && !presentNeedsRecreate) {
            return false;
        }

        m_swapchainImageLayouts[imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        m_currentFrame = (m_currentFrame + 1u) % static_cast<UINT>(m_commandBuffers.size());
        if (recreateAfterPresent || presentNeedsRecreate) {
            return ResizeBackendSwapChain(m_swapchainExtent.width, m_swapchainExtent.height);
        }
        return true;
    }

    bool VulkanGraphicsDevice::CreateSwapChainImageViews()
    {
        if (m_device == VK_NULL_HANDLE || m_swapchainImages.empty() || m_swapchainFormat == VK_FORMAT_UNDEFINED) {
            return false;
        }

        m_swapchainImageViews.resize(m_swapchainImages.size(), VK_NULL_HANDLE);
        for (size_t i = 0; i < m_swapchainImages.size(); ++i) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = m_swapchainImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = m_swapchainFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_swapchainImageViews[i]) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::CreateSwapChainImageViews: vkCreateImageView failed.\n");
                for (VkImageView imageView : m_swapchainImageViews) {
                    if (imageView != VK_NULL_HANDLE) {
                        vkDestroyImageView(m_device, imageView, nullptr);
                    }
                }
                m_swapchainImageViews.clear();
                return false;
            }
        }
        return true;
    }

    void VulkanGraphicsDevice::DestroyNativeMeshResources()
    {
        if (m_device != VK_NULL_HANDLE) {
            if (m_nativeMeshPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(m_device, m_nativeMeshPipeline, nullptr);
            }
            if (m_nativeMeshPipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(m_device, m_nativeMeshPipelineLayout, nullptr);
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
        }

        m_nativeMeshPipeline = VK_NULL_HANDLE;
        m_nativeMeshPipelineLayout = VK_NULL_HANDLE;
        m_nativeMeshRenderPass = VK_NULL_HANDLE;
        m_nativeMeshFramebuffers.clear();
        m_nativeMeshDepthImages.clear();
        m_nativeMeshDepthMemory.clear();
        m_nativeMeshDepthViews.clear();
    }

    bool VulkanGraphicsDevice::EnsureNativeMeshResources()
    {
        if (m_device == VK_NULL_HANDLE || m_swapchainImageViews.empty()) {
            return false;
        }
        if (m_nativeMeshPipeline != VK_NULL_HANDLE &&
            m_nativeMeshPipelineLayout != VK_NULL_HANDLE &&
            m_nativeMeshRenderPass != VK_NULL_HANDLE &&
            m_nativeMeshFramebuffers.size() == m_swapchainImageViews.size() &&
            m_nativeMeshDepthViews.size() == m_swapchainImageViews.size()) {
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

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(VulkanNativeMeshPushConstants);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
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
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blendAttachment.blendEnable = VK_TRUE;
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

        const VkResult pipelineResult =
            vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_nativeMeshPipeline);
        vkDestroyShaderModule(m_device, psModule, nullptr);
        vkDestroyShaderModule(m_device, vsModule, nullptr);
        if (pipelineResult != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureNativeMeshResources: vkCreateGraphicsPipelines failed.\n");
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
            depthImageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
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

        return true;
    }

    // -------------------------------------------------------------------------
    // Native ray march frame resources
    // -------------------------------------------------------------------------

    namespace
    {
        struct VulkanRayMarchConstants
        {
            float invVP[16];
            float camPos[3];    float time;
            float sunDir[3];    float sunI;
            float sunColor[3];  float cloudCover;
            float renderW;      float renderH; float fluidMode; float cloudDensity;
            float extra0[4];
            float extra1[4];
            float extra2[4];
        };
        static_assert(sizeof(VulkanRayMarchConstants) == 176, "ray march CB size mismatch");
    }

    void VulkanGraphicsDevice::DestroyRayMarchResources()
    {
        if (m_device != VK_NULL_HANDLE) {
            if (m_nativeRayMarchPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(m_device, m_nativeRayMarchPipeline, nullptr);
            }
            if (m_nativeRayMarchPipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(m_device, m_nativeRayMarchPipelineLayout, nullptr);
            }
            if (m_nativeRayMarchDescPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(m_device, m_nativeRayMarchDescPool, nullptr);
            }
            if (m_nativeRayMarchDescSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(m_device, m_nativeRayMarchDescSetLayout, nullptr);
            }
            if (m_nativeRayMarchUboMapped && m_nativeRayMarchUboMemory != VK_NULL_HANDLE) {
                vkUnmapMemory(m_device, m_nativeRayMarchUboMemory);
                m_nativeRayMarchUboMapped = nullptr;
            }
            if (m_nativeRayMarchUbo != VK_NULL_HANDLE) {
                vkDestroyBuffer(m_device, m_nativeRayMarchUbo, nullptr);
            }
            if (m_nativeRayMarchUboMemory != VK_NULL_HANDLE) {
                vkFreeMemory(m_device, m_nativeRayMarchUboMemory, nullptr);
            }
            for (VkFramebuffer fb : m_nativeRayMarchFramebuffers) {
                if (fb != VK_NULL_HANDLE) {
                    vkDestroyFramebuffer(m_device, fb, nullptr);
                }
            }
            if (m_nativeRayMarchRenderPass != VK_NULL_HANDLE) {
                vkDestroyRenderPass(m_device, m_nativeRayMarchRenderPass, nullptr);
            }
        }
        m_nativeRayMarchPipeline       = VK_NULL_HANDLE;
        m_nativeRayMarchPipelineLayout = VK_NULL_HANDLE;
        m_nativeRayMarchDescPool       = VK_NULL_HANDLE;
        m_nativeRayMarchDescSet        = VK_NULL_HANDLE;
        m_nativeRayMarchDescSetLayout  = VK_NULL_HANDLE;
        m_nativeRayMarchUbo            = VK_NULL_HANDLE;
        m_nativeRayMarchUboMemory      = VK_NULL_HANDLE;
        m_nativeRayMarchUboMapped      = nullptr;
        m_nativeRayMarchFramebuffers.clear();
        m_nativeRayMarchRenderPass     = VK_NULL_HANDLE;
    }

    bool VulkanGraphicsDevice::EnsureRayMarchResources()
    {
        if (m_device == VK_NULL_HANDLE || m_swapchainImageViews.empty()) {
            return false;
        }
        if (m_nativeRayMarchPipeline != VK_NULL_HANDLE &&
            m_nativeRayMarchFramebuffers.size() == m_swapchainImageViews.size()) {
            return true;
        }

        DestroyRayMarchResources();

        // Render pass — color attachment only (no depth for fullscreen effect)
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format        = m_swapchainFormat;
        colorAttachment.samples       = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp        = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp       = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &colorRef;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments    = &colorAttachment;
        renderPassInfo.subpassCount    = 1;
        renderPassInfo.pSubpasses      = &subpass;
        if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_nativeRayMarchRenderPass) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureRayMarchResources: vkCreateRenderPass failed.\n");
            DestroyRayMarchResources();
            return false;
        }

        // UBO — 176 bytes, host-visible + coherent, persistently mapped
        VkBufferCreateInfo uboInfo{};
        uboInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        uboInfo.size        = sizeof(VulkanRayMarchConstants);
        uboInfo.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        uboInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &uboInfo, nullptr, &m_nativeRayMarchUbo) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureRayMarchResources: UBO vkCreateBuffer failed.\n");
            DestroyRayMarchResources();
            return false;
        }
        VkMemoryRequirements uboReqs{};
        vkGetBufferMemoryRequirements(m_device, m_nativeRayMarchUbo, &uboReqs);
        const uint32_t uboMemType = FindMemoryType(
            uboReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (uboMemType == UINT32_MAX) {
            DebugLog("VulkanGraphicsDevice::EnsureRayMarchResources: no suitable UBO memory type.\n");
            DestroyRayMarchResources();
            return false;
        }
        VkMemoryAllocateInfo uboAllocInfo{};
        uboAllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        uboAllocInfo.allocationSize  = uboReqs.size;
        uboAllocInfo.memoryTypeIndex = uboMemType;
        if (vkAllocateMemory(m_device, &uboAllocInfo, nullptr, &m_nativeRayMarchUboMemory) != VK_SUCCESS ||
            vkBindBufferMemory(m_device, m_nativeRayMarchUbo, m_nativeRayMarchUboMemory, 0) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureRayMarchResources: UBO memory allocation failed.\n");
            DestroyRayMarchResources();
            return false;
        }
        if (vkMapMemory(m_device, m_nativeRayMarchUboMemory, 0, sizeof(VulkanRayMarchConstants),
                        0, &m_nativeRayMarchUboMapped) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureRayMarchResources: UBO map failed.\n");
            DestroyRayMarchResources();
            return false;
        }

        // Descriptor set layout — binding 0: uniform buffer (fragment stage)
        VkDescriptorSetLayoutBinding uboBinding{};
        uboBinding.binding         = 0;
        uboBinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboBinding.descriptorCount = 1;
        uboBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo descLayoutInfo{};
        descLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descLayoutInfo.bindingCount = 1;
        descLayoutInfo.pBindings    = &uboBinding;
        if (vkCreateDescriptorSetLayout(m_device, &descLayoutInfo, nullptr,
                                        &m_nativeRayMarchDescSetLayout) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureRayMarchResources: vkCreateDescriptorSetLayout failed.\n");
            DestroyRayMarchResources();
            return false;
        }

        // Descriptor pool (1 uniform buffer slot)
        VkDescriptorPoolSize poolSize{};
        poolSize.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSize.descriptorCount = 1;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets       = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_nativeRayMarchDescPool) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureRayMarchResources: vkCreateDescriptorPool failed.\n");
            DestroyRayMarchResources();
            return false;
        }

        // Allocate descriptor set
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = m_nativeRayMarchDescPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts        = &m_nativeRayMarchDescSetLayout;
        if (vkAllocateDescriptorSets(m_device, &allocInfo, &m_nativeRayMarchDescSet) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureRayMarchResources: vkAllocateDescriptorSets failed.\n");
            DestroyRayMarchResources();
            return false;
        }

        // Write UBO into descriptor set
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = m_nativeRayMarchUbo;
        bufInfo.offset = 0;
        bufInfo.range  = sizeof(VulkanRayMarchConstants);
        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = m_nativeRayMarchDescSet;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo     = &bufInfo;
        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        // Pipeline layout — one descriptor set, no push constants
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts    = &m_nativeRayMarchDescSetLayout;
        if (vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr,
                                   &m_nativeRayMarchPipelineLayout) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureRayMarchResources: vkCreatePipelineLayout failed.\n");
            DestroyRayMarchResources();
            return false;
        }

        // Compile ray march HLSL shaders to SPIR-V
        Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
        const std::filesystem::path shaderRoot = ShaderCompilationService::GetShaderSourceRoot();
        if (!ShaderCompilationService::CompileShader(
                shaderRoot / L"Effects" / L"RayMarch" / L"RayMarch_VS.hlsl",
                "VSMain", "vs_6_0", vsBlob, true) ||
            !ShaderCompilationService::CompileShader(
                shaderRoot / L"Effects" / L"RayMarch" / L"RayMarch_PS.hlsl",
                "PSMain", "ps_6_0", psBlob, true)) {
            DebugLog("VulkanGraphicsDevice::EnsureRayMarchResources: SPIR-V shader compile failed.\n");
            DestroyRayMarchResources();
            return false;
        }

        VkShaderModuleCreateInfo vsInfo{};
        vsInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vsInfo.codeSize = vsBlob->GetBufferSize();
        vsInfo.pCode    = static_cast<const uint32_t*>(vsBlob->GetBufferPointer());
        VkShaderModule vsModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &vsInfo, nullptr, &vsModule) != VK_SUCCESS) {
            DestroyRayMarchResources();
            return false;
        }

        VkShaderModuleCreateInfo psInfo{};
        psInfo.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        psInfo.codeSize = psBlob->GetBufferSize();
        psInfo.pCode    = static_cast<const uint32_t*>(psBlob->GetBufferPointer());
        VkShaderModule psModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &psInfo, nullptr, &psModule) != VK_SUCCESS) {
            vkDestroyShaderModule(m_device, vsModule, nullptr);
            DestroyRayMarchResources();
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vsModule;
        stages[0].pName  = "VSMain";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = psModule;
        stages[1].pName  = "PSMain";

        // No vertex input — fullscreen triangle generated in VS from SV_VertexID
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType     = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode  = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments    = &blendAttachment;

        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates    = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount          = 2;
        pipelineInfo.pStages             = stages;
        pipelineInfo.pVertexInputState   = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState      = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState   = &multisample;
        pipelineInfo.pDepthStencilState  = &depthStencil;
        pipelineInfo.pColorBlendState    = &blend;
        pipelineInfo.pDynamicState       = &dynamic;
        pipelineInfo.layout              = m_nativeRayMarchPipelineLayout;
        pipelineInfo.renderPass          = m_nativeRayMarchRenderPass;

        const VkResult pipelineResult =
            vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                      nullptr, &m_nativeRayMarchPipeline);
        vkDestroyShaderModule(m_device, psModule, nullptr);
        vkDestroyShaderModule(m_device, vsModule, nullptr);
        if (pipelineResult != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureRayMarchResources: vkCreateGraphicsPipelines failed.\n");
            DestroyRayMarchResources();
            return false;
        }

        // Framebuffers — one per swapchain image, color only (no depth)
        const size_t imageCount = m_swapchainImageViews.size();
        m_nativeRayMarchFramebuffers.resize(imageCount, VK_NULL_HANDLE);
        for (size_t i = 0; i < imageCount; ++i) {
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = m_nativeRayMarchRenderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments    = &m_swapchainImageViews[i];
            fbInfo.width           = m_swapchainExtent.width;
            fbInfo.height          = m_swapchainExtent.height;
            fbInfo.layers          = 1;
            if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_nativeRayMarchFramebuffers[i]) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureRayMarchResources: vkCreateFramebuffer failed.\n");
                DestroyRayMarchResources();
                return false;
            }
        }

        return true;
    }

    bool VulkanGraphicsDevice::RenderRayMarchFrame(uint32_t imageIndex,
                                                   VkCommandBuffer cmd,
                                                   const RhiBackendRayMarchFrameDesc& desc,
                                                   const RhiClearColor& clearColor)
    {
        if (imageIndex >= m_swapchainImages.size() || !EnsureRayMarchResources()) {
            return false;
        }
        if (imageIndex >= m_nativeRayMarchFramebuffers.size()) {
            return false;
        }

        // Transition swapchain image to color attachment layout
        VkImageMemoryBarrier toColor{};
        toColor.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toColor.srcAccessMask       = 0;
        toColor.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toColor.oldLayout           = m_swapchainImageLayouts[imageIndex];
        toColor.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.image               = m_swapchainImages[imageIndex];
        toColor.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        toColor.subresourceRange.baseMipLevel   = 0;
        toColor.subresourceRange.levelCount     = 1;
        toColor.subresourceRange.baseArrayLayer = 0;
        toColor.subresourceRange.layerCount     = 1;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toColor);

        // Upload constants to persistently mapped UBO
        VulkanRayMarchConstants constants{};
        std::memcpy(constants.invVP, desc.invViewProjection, sizeof(constants.invVP));
        constants.camPos[0]   = desc.cameraPos[0];
        constants.camPos[1]   = desc.cameraPos[1];
        constants.camPos[2]   = desc.cameraPos[2];
        constants.time        = desc.sceneTimeSec;
        constants.sunDir[0]   = desc.sunDir[0];
        constants.sunDir[1]   = desc.sunDir[1];
        constants.sunDir[2]   = desc.sunDir[2];
        constants.sunI        = desc.sunIntensity;
        constants.sunColor[0] = desc.sunColor[0];
        constants.sunColor[1] = desc.sunColor[1];
        constants.sunColor[2] = desc.sunColor[2];
        constants.cloudCover  = desc.cloudCover;
        constants.renderW     = desc.renderWidth;
        constants.renderH     = desc.renderHeight;
        constants.fluidMode   = desc.fluidMode;
        constants.cloudDensity = desc.cloudDensity;
        constants.extra0[0]   = desc.debugMode;
        constants.extra0[1]   = desc.tanHalfFovY * desc.aspectRatio;
        constants.extra0[2]   = desc.tanHalfFovY;
        constants.extra0[3]   = desc.explicitCameraBasis ? 1.0f : 0.0f;
        constants.extra1[0]   = desc.cameraRight[0];
        constants.extra1[1]   = desc.cameraRight[1];
        constants.extra1[2]   = desc.cameraRight[2];
        constants.extra1[3]   = 0.0f;
        constants.extra2[0]   = desc.cameraUp[0];
        constants.extra2[1]   = desc.cameraUp[1];
        constants.extra2[2]   = desc.cameraUp[2];
        constants.extra2[3]   = 0.0f;
        std::memcpy(m_nativeRayMarchUboMapped, &constants, sizeof(constants));

        // Begin render pass and draw fullscreen triangle
        VkClearValue clearValue{};
        clearValue.color.float32[0] = clearColor.r;
        clearValue.color.float32[1] = clearColor.g;
        clearValue.color.float32[2] = clearColor.b;
        clearValue.color.float32[3] = clearColor.a;

        VkRenderPassBeginInfo beginInfo{};
        beginInfo.sType                    = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass               = m_nativeRayMarchRenderPass;
        beginInfo.framebuffer              = m_nativeRayMarchFramebuffers[imageIndex];
        beginInfo.renderArea.offset        = { 0, 0 };
        beginInfo.renderArea.extent        = m_swapchainExtent;
        beginInfo.clearValueCount          = 1;
        beginInfo.pClearValues             = &clearValue;
        vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.x        = 0.0f;
        viewport.y        = 0.0f;
        viewport.width    = desc.renderWidth > 0.0f ? desc.renderWidth
                                                    : static_cast<float>(m_swapchainExtent.width);
        viewport.height   = desc.renderHeight > 0.0f ? desc.renderHeight
                                                     : static_cast<float>(m_swapchainExtent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.offset = { 0, 0 };
        scissor.extent = {
            static_cast<uint32_t>(viewport.width),
            static_cast<uint32_t>(viewport.height),
        };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_nativeRayMarchPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                m_nativeRayMarchPipelineLayout, 0, 1,
                                &m_nativeRayMarchDescSet, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmd);
        return true;
    }

    bool VulkanGraphicsDevice::RenderMeshFrame(uint32_t,
                                               uint32_t imageIndex,
                                               VkCommandBuffer cmd,
                                               const RhiBackendMeshFrameDesc& desc,
                                               const RhiClearColor& clearColor)
    {
        if (imageIndex >= m_swapchainImages.size() ||
            !EnsureNativeMeshResources()) {
            return false;
        }
        if (imageIndex >= m_nativeMeshFramebuffers.size()) {
            return false;
        }

        VkImageMemoryBarrier toColor{};
        toColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toColor.srcAccessMask = 0;
        toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toColor.oldLayout = m_swapchainImageLayouts[imageIndex];
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.image = m_swapchainImages[imageIndex];
        toColor.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toColor.subresourceRange.baseMipLevel = 0;
        toColor.subresourceRange.levelCount = 1;
        toColor.subresourceRange.baseArrayLayer = 0;
        toColor.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &toColor);

        VkClearValue clearValues[2]{};
        clearValues[0].color.float32[0] = clearColor.r;
        clearValues[0].color.float32[1] = clearColor.g;
        clearValues[0].color.float32[2] = clearColor.b;
        clearValues[0].color.float32[3] = clearColor.a;
        clearValues[1].depthStencil.depth = 1.0f;
        clearValues[1].depthStencil.stencil = 0;

        VkRenderPassBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = m_nativeMeshRenderPass;
        beginInfo.framebuffer = m_nativeMeshFramebuffers[imageIndex];
        beginInfo.renderArea.offset = { 0, 0 };
        beginInfo.renderArea.extent = m_swapchainExtent;
        beginInfo.clearValueCount = static_cast<uint32_t>(std::size(clearValues));
        beginInfo.pClearValues = clearValues;
        vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = desc.renderWidth > 0.0f ? desc.renderWidth : static_cast<float>(m_swapchainExtent.width);
        viewport.height = desc.renderHeight > 0.0f ? desc.renderHeight : static_cast<float>(m_swapchainExtent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.offset = { 0, 0 };
        scissor.extent = {
            static_cast<uint32_t>(viewport.width),
            static_cast<uint32_t>(viewport.height),
        };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_nativeMeshPipeline);

        for (uint32_t i = 0; i < desc.drawCount; ++i) {
            const RhiBackendMeshDrawDesc& draw = desc.draws[i];
            const auto vbIt = m_rhiResources.find(draw.vertexBufferHandle);
            if (vbIt == m_rhiResources.end() || vbIt->second.buffer == VK_NULL_HANDLE) {
                continue;
            }

            VulkanNativeMeshPushConstants constants{};
            Math::Mul4x4(draw.model, desc.viewProjection, constants.modelViewProjection);
            std::memcpy(constants.baseColor, draw.baseColor, sizeof(constants.baseColor));
            constants.lightDirIntensity[0] = desc.sunDir[0];
            constants.lightDirIntensity[1] = desc.sunDir[1];
            constants.lightDirIntensity[2] = desc.sunDir[2];
            constants.lightDirIntensity[3] = desc.sunIntensity;
            constants.lightColor[0] = desc.sunColor[0];
            constants.lightColor[1] = desc.sunColor[1];
            constants.lightColor[2] = desc.sunColor[2];
            constants.lightColor[3] = 1.0f;
            constants.emissiveRoughness[0] = draw.emissive[0];
            constants.emissiveRoughness[1] = draw.emissive[1];
            constants.emissiveRoughness[2] = draw.emissive[2];
            constants.emissiveRoughness[3] = draw.roughness;
            vkCmdPushConstants(cmd,
                               m_nativeMeshPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0,
                               sizeof(constants),
                               &constants);

            VkBuffer vertexBuffer = vbIt->second.buffer;
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);

            const auto ibIt = m_rhiResources.find(draw.indexBufferHandle);
            if (draw.indexCount > 0 && ibIt != m_rhiResources.end() && ibIt->second.buffer != VK_NULL_HANDLE) {
                vkCmdBindIndexBuffer(cmd,
                                     ibIt->second.buffer,
                                     0,
                                     draw.index32Bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
                vkCmdDrawIndexed(cmd, draw.indexCount, 1, 0, 0, 0);
            } else if (draw.vertexCount > 0) {
                vkCmdDraw(cmd, draw.vertexCount, 1, 0, 0);
            }
        }

        vkCmdEndRenderPass(cmd);
        return true;
    }

    HRESULT VulkanGraphicsDevice::CreateDescriptorHeap(const DescriptorHeapDesc&, DescriptorHeap&)
    {
        return E_NOTIMPL;
    }

    HRESULT VulkanGraphicsDevice::CreateCommittedResource(const HeapProperties*,
                                                          HeapFlags,
                                                          const ResourceDesc*,
                                                          ResourceState,
                                                          const ClearValue*,
                                                          Resource&)
    {
        return E_NOTIMPL;
    }

    HRESULT VulkanGraphicsDevice::CreateCommandAllocator(CommandListType, CommandAllocator&)
    {
        return E_NOTIMPL;
    }

    HRESULT VulkanGraphicsDevice::CreateCommandList(UINT, CommandListType, CommandAllocator&, PipelineState*, CommandList&)
    {
        return E_NOTIMPL;
    }

    HRESULT VulkanGraphicsDevice::CreateGraphicsPipelineState(const GraphicsPipelineDesc&, PipelineState&)
    {
        return E_NOTIMPL;
    }

    HRESULT VulkanGraphicsDevice::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC&, PipelineState&)
    {
        return E_NOTIMPL;
    }

    HRESULT VulkanGraphicsDevice::CreatePipelineStateFromStream(const void*, size_t, PipelineState&)
    {
        return E_NOTIMPL;
    }

    HRESULT VulkanGraphicsDevice::CreateRootSignature(UINT, const void*, size_t, RootSignature&)
    {
        return E_NOTIMPL;
    }

    void VulkanGraphicsDevice::CreateShaderResourceView(Resource&, const ShaderResourceViewDesc*, CpuDescriptorHandle)
    {
    }

    void VulkanGraphicsDevice::CreateDepthStencilView(Resource&, const DepthStencilViewDesc*, CpuDescriptorHandle)
    {
    }

    void VulkanGraphicsDevice::CreateRenderTargetView(Resource&, const D3D12_RENDER_TARGET_VIEW_DESC*, CpuDescriptorHandle)
    {
    }

    void VulkanGraphicsDevice::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC*, CpuDescriptorHandle)
    {
    }

    void VulkanGraphicsDevice::CreateSampler(const D3D12_SAMPLER_DESC*, CpuDescriptorHandle)
    {
    }

    HRESULT VulkanGraphicsDevice::CreateFence(UINT64, D3D12_FENCE_FLAGS, ID3D12Fence**)
    {
        return E_NOTIMPL;
    }

    bool VulkanGraphicsDevice::CreateInstance()
    {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "SasamiRenderer";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "SasamiRenderer";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_3;

        const std::array<const char*, 2> extensions = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        const VkResult result = vkCreateInstance(&createInfo, nullptr, &m_instance);
        if (result != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateInstance: vkCreateInstance failed.\n");
            return false;
        }
        return true;
    }

    bool VulkanGraphicsDevice::CreateSurface(HWND hWnd)
    {
        VkWin32SurfaceCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        createInfo.hinstance = GetModuleHandle(nullptr);
        createInfo.hwnd = hWnd;

        const VkResult result = vkCreateWin32SurfaceKHR(m_instance, &createInfo, nullptr, &m_surface);
        if (result != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateSurface: vkCreateWin32SurfaceKHR failed.\n");
            return false;
        }
        return true;
    }

    bool VulkanGraphicsDevice::PickPhysicalDevice()
    {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            DebugLog("VulkanGraphicsDevice::PickPhysicalDevice: no Vulkan physical devices found.\n");
            return false;
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

        for (VkPhysicalDevice device : devices) {
            const std::vector<VkExtensionProperties> extensions = EnumerateDeviceExtensions(device);
            if (!HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                continue;
            }

            uint32_t queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
            std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

            bool foundGraphics = false;
            bool foundPresent = false;
            bool foundCompute = false;
            for (uint32_t i = 0; i < queueFamilyCount; ++i) {
                if (!foundGraphics && (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                    m_graphicsQueueFamily = i;
                    foundGraphics = true;
                }

                VkBool32 presentSupported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &presentSupported);
                if (!foundPresent && presentSupported) {
                    m_presentQueueFamily = i;
                    foundPresent = true;
                }

                if ((queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                    !(queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                    m_computeQueueFamily = i;
                    m_hasDedicatedComputeQueue = true;
                    foundCompute = true;
                }
            }

            if (!foundCompute) {
                for (uint32_t i = 0; i < queueFamilyCount; ++i) {
                    if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                        m_computeQueueFamily = i;
                        foundCompute = true;
                        break;
                    }
                }
            }

            if (foundGraphics && foundPresent && foundCompute) {
                m_physicalDevice = device;

                // Detect optional extensions for capabilities
                m_hasVkKhrDynamicRendering   = HasExtension(extensions, "VK_KHR_dynamic_rendering");
                m_hasVkExtDescriptorIndexing = HasExtension(extensions, "VK_EXT_descriptor_indexing");
                m_hasVkKhrTimelineSemaphore  = HasExtension(extensions, "VK_KHR_timeline_semaphore");
                m_hasVkKhrBufferDeviceAddress = HasExtension(extensions, "VK_KHR_buffer_device_address");
                m_hasVkKhrDeferredHostOps    = HasExtension(extensions, "VK_KHR_deferred_host_operations");
                m_hasVkKhrAccelerationStructure = HasExtension(extensions, "VK_KHR_acceleration_structure")
                    && m_hasVkKhrBufferDeviceAddress
                    && m_hasVkExtDescriptorIndexing
                    && m_hasVkKhrDeferredHostOps;
                m_hasVkKhrRayTracingPipeline = m_hasVkKhrAccelerationStructure
                    && HasExtension(extensions, "VK_KHR_ray_tracing_pipeline")
                    && HasExtension(extensions, "VK_KHR_spirv_1_4");
                m_hasVkKhrRayQuery           = m_hasVkKhrAccelerationStructure
                    && HasExtension(extensions, "VK_KHR_ray_query");

                return true;
            }
        }

        DebugLog("VulkanGraphicsDevice::PickPhysicalDevice: no suitable Vulkan physical device found.\n");
        return false;
    }

    bool VulkanGraphicsDevice::CreateDevice()
    {
        std::vector<uint32_t> uniqueFamilies = { m_graphicsQueueFamily, m_presentQueueFamily, m_computeQueueFamily };
        std::sort(uniqueFamilies.begin(), uniqueFamilies.end());
        uniqueFamilies.erase(std::unique(uniqueFamilies.begin(), uniqueFamilies.end()), uniqueFamilies.end());

        const float priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queues;
        queues.reserve(uniqueFamilies.size());
        for (uint32_t family : uniqueFamilies) {
            VkDeviceQueueCreateInfo queueInfo{};
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = family;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &priority;
            queues.push_back(queueInfo);
        }

        std::vector<const char*> extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

        if (m_hasVkKhrDynamicRendering)      extensions.push_back("VK_KHR_dynamic_rendering");
        if (m_hasVkExtDescriptorIndexing)    extensions.push_back("VK_EXT_descriptor_indexing");
        if (m_hasVkKhrTimelineSemaphore)     extensions.push_back("VK_KHR_timeline_semaphore");
        if (m_hasVkKhrBufferDeviceAddress)   extensions.push_back("VK_KHR_buffer_device_address");
        if (m_hasVkKhrDeferredHostOps)       extensions.push_back("VK_KHR_deferred_host_operations");
        if (m_hasVkKhrAccelerationStructure) extensions.push_back("VK_KHR_acceleration_structure");
        if (m_hasVkKhrRayTracingPipeline) {
            extensions.push_back("VK_KHR_ray_tracing_pipeline");
            extensions.push_back("VK_KHR_spirv_1_4");
        }
        if (m_hasVkKhrRayQuery) extensions.push_back("VK_KHR_ray_query");

        // Build pNext chain for optional device features
        void* featureChainHead = nullptr;

        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
        if (m_hasVkKhrRayQuery) {
            rayQueryFeatures.sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
            rayQueryFeatures.rayQuery = VK_TRUE;
            rayQueryFeatures.pNext    = featureChainHead;
            featureChainHead          = &rayQueryFeatures;
        }

        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures{};
        if (m_hasVkKhrRayTracingPipeline) {
            rtPipelineFeatures.sType              = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
            rtPipelineFeatures.rayTracingPipeline = VK_TRUE;
            rtPipelineFeatures.pNext              = featureChainHead;
            featureChainHead                      = &rtPipelineFeatures;
        }

        VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
        if (m_hasVkKhrAccelerationStructure) {
            asFeatures.sType                 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
            asFeatures.accelerationStructure = VK_TRUE;
            asFeatures.pNext                 = featureChainHead;
            featureChainHead                 = &asFeatures;
        }

        VkPhysicalDeviceBufferDeviceAddressFeaturesKHR bufAddrFeatures{};
        if (m_hasVkKhrBufferDeviceAddress) {
            bufAddrFeatures.sType               = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR;
            bufAddrFeatures.bufferDeviceAddress = VK_TRUE;
            bufAddrFeatures.pNext               = featureChainHead;
            featureChainHead                    = &bufAddrFeatures;
        }

        VkPhysicalDeviceFeatures features{};
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = featureChainHead;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queues.size());
        createInfo.pQueueCreateInfos = queues.data();
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.pEnabledFeatures = &features;

        const VkResult result = vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device);
        if (result != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateDevice: vkCreateDevice failed.\n");
            return false;
        }

        // Load RT function pointers if extensions are available
        if (m_hasVkKhrBufferDeviceAddress) {
            m_pfnGetBufAddr = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(
                vkGetDeviceProcAddr(m_device, "vkGetBufferDeviceAddressKHR"));
        }
        if (m_hasVkKhrAccelerationStructure) {
            m_pfnGetAsBuildSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
                vkGetDeviceProcAddr(m_device, "vkGetAccelerationStructureBuildSizesKHR"));
            m_pfnCreateAs = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
                vkGetDeviceProcAddr(m_device, "vkCreateAccelerationStructureKHR"));
            m_pfnDestroyAs = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
                vkGetDeviceProcAddr(m_device, "vkDestroyAccelerationStructureKHR"));
            m_pfnCmdBuildAs = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
                vkGetDeviceProcAddr(m_device, "vkCmdBuildAccelerationStructuresKHR"));
            m_pfnGetAsAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
                vkGetDeviceProcAddr(m_device, "vkGetAccelerationStructureDeviceAddressKHR"));
        }
        if (m_hasVkKhrRayTracingPipeline) {
            m_pfnCreateRtPipeline = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
                vkGetDeviceProcAddr(m_device, "vkCreateRayTracingPipelinesKHR"));
            m_pfnGetRtHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
                vkGetDeviceProcAddr(m_device, "vkGetRayTracingShaderGroupHandlesKHR"));
            m_pfnCmdTraceRays = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(
                vkGetDeviceProcAddr(m_device, "vkCmdTraceRaysKHR"));
        }

        vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
        vkGetDeviceQueue(m_device, m_computeQueueFamily, 0, &m_computeQueue);
        vkGetDeviceQueue(m_device, m_presentQueueFamily, 0, &m_presentQueue);
        return true;
    }

    bool VulkanGraphicsDevice::CreateSwapChain(UINT width, UINT height, UINT bufferCount)
    {
        VkSurfaceCapabilitiesKHR surfaceCaps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &surfaceCaps) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateSwapChain: failed to query surface capabilities.\n");
            return false;
        }

        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data());

        VkSurfaceFormatKHR selectedFormat = formats.empty()
            ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR }
            : formats[0];
        for (const VkSurfaceFormatKHR& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM ||
                format.format == VK_FORMAT_R8G8B8A8_UNORM) {
                selectedFormat = format;
                break;
            }
        }

        uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, nullptr);
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, presentModes.data());
        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
        for (VkPresentModeKHR mode : presentModes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                presentMode = mode;
                break;
            }
        }

        VkExtent2D extent{};
        if (surfaceCaps.currentExtent.width != UINT32_MAX) {
            extent = surfaceCaps.currentExtent;
        } else {
            extent.width = std::clamp<uint32_t>(width, surfaceCaps.minImageExtent.width, surfaceCaps.maxImageExtent.width);
            extent.height = std::clamp<uint32_t>(height, surfaceCaps.minImageExtent.height, surfaceCaps.maxImageExtent.height);
        }
        if (extent.width == 0 || extent.height == 0) {
            return false;
        }

        uint32_t imageCount = std::max<uint32_t>(bufferCount, surfaceCaps.minImageCount);
        if (surfaceCaps.maxImageCount > 0) {
            imageCount = std::min<uint32_t>(imageCount, surfaceCaps.maxImageCount);
        }

        uint32_t queueFamilyIndices[] = { m_graphicsQueueFamily, m_presentQueueFamily };
        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = m_surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = selectedFormat.format;
        createInfo.imageColorSpace = selectedFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        createInfo.imageSharingMode = (m_graphicsQueueFamily != m_presentQueueFamily)
            ? VK_SHARING_MODE_CONCURRENT
            : VK_SHARING_MODE_EXCLUSIVE;
        createInfo.queueFamilyIndexCount = (createInfo.imageSharingMode == VK_SHARING_MODE_CONCURRENT) ? 2u : 0u;
        createInfo.pQueueFamilyIndices = (createInfo.imageSharingMode == VK_SHARING_MODE_CONCURRENT) ? queueFamilyIndices : nullptr;
        createInfo.preTransform = surfaceCaps.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = m_swapchain;

        VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
        const VkResult result = vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &newSwapchain);
        if (result != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateSwapChain: vkCreateSwapchainKHR failed.\n");
            return false;
        }

        uint32_t actualImageCount = 0;
        if (vkGetSwapchainImagesKHR(m_device, newSwapchain, &actualImageCount, nullptr) != VK_SUCCESS ||
            actualImageCount == 0) {
            vkDestroySwapchainKHR(m_device, newSwapchain, nullptr);
            return false;
        }
        std::vector<VkImage> newImages(actualImageCount);
        if (vkGetSwapchainImagesKHR(m_device, newSwapchain, &actualImageCount, newImages.data()) != VK_SUCCESS) {
            vkDestroySwapchainKHR(m_device, newSwapchain, nullptr);
            return false;
        }

        std::vector<VkImageView> newImageViews(actualImageCount, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < actualImageCount; ++i) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = newImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = selectedFormat.format;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;
            if (vkCreateImageView(m_device, &viewInfo, nullptr, &newImageViews[i]) != VK_SUCCESS) {
                for (VkImageView imageView : newImageViews) {
                    if (imageView != VK_NULL_HANDLE) {
                        vkDestroyImageView(m_device, imageView, nullptr);
                    }
                }
                vkDestroySwapchainKHR(m_device, newSwapchain, nullptr);
                return false;
            }
        }

        DestroyNativeMeshResources();
        DestroyRayMarchResources();
        for (VkImageView imageView : m_swapchainImageViews) {
            if (imageView != VK_NULL_HANDLE) {
                vkDestroyImageView(m_device, imageView, nullptr);
            }
        }
        if (m_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        }

        m_swapchain = newSwapchain;
        m_swapchainFormat = selectedFormat.format;
        m_swapchainExtent = extent;
        m_swapchainImages = std::move(newImages);
        m_swapchainImageViews = std::move(newImageViews);
        m_swapchainImageLayouts.assign(actualImageCount, VK_IMAGE_LAYOUT_UNDEFINED);
        return true;
    }

    bool VulkanGraphicsDevice::CreateFrameResources(UINT bufferCount)
    {
        const uint32_t frameCount = std::max<uint32_t>(1u, bufferCount);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
        if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateFrameResources: vkCreateCommandPool failed.\n");
            return false;
        }

        m_commandBuffers.resize(frameCount);
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = frameCount;
        if (vkAllocateCommandBuffers(m_device, &allocInfo, m_commandBuffers.data()) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateFrameResources: vkAllocateCommandBuffers failed.\n");
            return false;
        }

        m_imageAvailableSemaphores.resize(frameCount);
        m_renderFinishedSemaphores.resize(frameCount);
        m_frameFences.resize(frameCount);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (uint32_t i = 0; i < frameCount; ++i) {
            if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_imageAvailableSemaphores[i]) != VK_SUCCESS ||
                vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinishedSemaphores[i]) != VK_SUCCESS ||
                vkCreateFence(m_device, &fenceInfo, nullptr, &m_frameFences[i]) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::CreateFrameResources: sync object creation failed.\n");
                return false;
            }
        }

        m_currentFrame = 0;
        return true;
    }

    void VulkanGraphicsDevice::QueryCapabilities()
    {
        m_capabilities = {};
        m_capabilities.api = RhiBackendApi::Vulkan;
        m_capabilities.supportsGraphicsQueue = true;
        m_capabilities.supportsComputeQueue = (m_computeQueue != VK_NULL_HANDLE);
        m_capabilities.supportsSwapChain = (m_swapchain != VK_NULL_HANDLE);
        m_capabilities.supportsNativeFrame = true;
        m_capabilities.supportsFeatureRenderPasses = false;
        m_capabilities.supportsD3D12CompatibilitySurface = false;
        m_capabilities.supportsRhiResourceCreation = true;
        m_capabilities.supportsRhiDescriptorCreation = true;
        m_capabilities.supportsRhiPipelineCreation = true;
        m_capabilities.supportsRhiCommandEncoding = true;

        // Optional Vulkan features — set from detected extensions
        m_capabilities.supportsVulkanDynamicRendering = m_hasVkKhrDynamicRendering;
        m_capabilities.supportsDynamicRenderPass      = m_hasVkKhrDynamicRendering;
        m_capabilities.supportsDescriptorIndexing     = m_hasVkExtDescriptorIndexing;
        m_capabilities.supportsTimelineSemaphore      = m_hasVkKhrTimelineSemaphore;
        m_capabilities.supportsHardwareRayTracing     = m_hasVkKhrAccelerationStructure;
        m_capabilities.supportsRayTracingPipeline     = m_hasVkKhrRayTracingPipeline;
        m_capabilities.supportsRayQuery               = m_hasVkKhrRayQuery;
        m_capabilities.supportsMeshShaders = false;
    }

    uint32_t VulkanGraphicsDevice::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const
    {
        if (m_physicalDevice == VK_NULL_HANDLE) {
            return UINT32_MAX;
        }

        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            const bool typeMatches = (typeBits & (1u << i)) != 0;
            const bool propertiesMatch =
                (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
            if (typeMatches && propertiesMatch) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    void VulkanGraphicsDevice::DestroySwapChain()
    {
        DestroyNativeMeshResources();
        DestroyRayMarchResources();
        if (m_device != VK_NULL_HANDLE) {
            for (VkImageView imageView : m_swapchainImageViews) {
                if (imageView != VK_NULL_HANDLE) {
                    vkDestroyImageView(m_device, imageView, nullptr);
                }
            }
        }
        m_swapchainImageViews.clear();
        m_swapchainImages.clear();
        m_swapchainImageLayouts.clear();
        m_swapchainFormat = VK_FORMAT_UNDEFINED;
        m_swapchainExtent = {};
        if (m_device != VK_NULL_HANDLE && m_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
            m_swapchain = VK_NULL_HANDLE;
        }
    }

    void VulkanGraphicsDevice::DestroyFrameResources()
    {
        if (m_device == VK_NULL_HANDLE) {
            m_commandBuffers.clear();
            m_imageAvailableSemaphores.clear();
            m_renderFinishedSemaphores.clear();
            m_frameFences.clear();
            m_commandPool = VK_NULL_HANDLE;
            return;
        }

        for (VkFence fence : m_frameFences) {
            if (fence != VK_NULL_HANDLE) {
                vkDestroyFence(m_device, fence, nullptr);
            }
        }
        for (VkSemaphore semaphore : m_renderFinishedSemaphores) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(m_device, semaphore, nullptr);
            }
        }
        for (VkSemaphore semaphore : m_imageAvailableSemaphores) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(m_device, semaphore, nullptr);
            }
        }
        if (m_commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
            m_commandPool = VK_NULL_HANDLE;
        }

        m_commandBuffers.clear();
        m_imageAvailableSemaphores.clear();
        m_renderFinishedSemaphores.clear();
        m_frameFences.clear();
        m_currentFrame = 0;
    }

    void VulkanGraphicsDevice::Cleanup()
    {
        if (m_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device);
            for (auto& entry : m_rhiPipelines) {
                VulkanRhiPipeline& pipeline = entry.second;
                if (pipeline.pipeline != VK_NULL_HANDLE) {
                    vkDestroyPipeline(m_device, pipeline.pipeline, nullptr);
                }
                if (pipeline.renderPass != VK_NULL_HANDLE) {
                    vkDestroyRenderPass(m_device, pipeline.renderPass, nullptr);
                }
                if (pipeline.ownedPipelineLayout != VK_NULL_HANDLE) {
                    vkDestroyPipelineLayout(m_device, pipeline.ownedPipelineLayout, nullptr);
                }
            }
            if (m_rhiDescriptorPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(m_device, m_rhiDescriptorPool, nullptr);
                m_rhiDescriptorPool = VK_NULL_HANDLE;
            }
            for (auto& entry : m_rhiPipelineLayouts) {
                VulkanRhiPipelineLayout& layout = entry.second;
                if (layout.pipelineLayout != VK_NULL_HANDLE) {
                    vkDestroyPipelineLayout(m_device, layout.pipelineLayout, nullptr);
                }
                if (layout.descriptorSetLayout != VK_NULL_HANDLE) {
                    vkDestroyDescriptorSetLayout(m_device, layout.descriptorSetLayout, nullptr);
                }
                for (VkSampler sampler : layout.immutableSamplers) {
                    if (sampler != VK_NULL_HANDLE) {
                        vkDestroySampler(m_device, sampler, nullptr);
                    }
                }
            }
            for (auto& entry : m_rhiShaders) {
                if (entry.second.module != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(m_device, entry.second.module, nullptr);
                }
            }
            for (auto& entry : m_rhiImageViews) {
                if (entry.second != VK_NULL_HANDLE) {
                    vkDestroyImageView(m_device, entry.second, nullptr);
                }
            }
            for (auto& entry : m_rhiResources) {
                VulkanRhiResource& resource = entry.second;
                if (resource.image != VK_NULL_HANDLE) {
                    vkDestroyImage(m_device, resource.image, nullptr);
                }
                if (resource.buffer != VK_NULL_HANDLE) {
                    vkDestroyBuffer(m_device, resource.buffer, nullptr);
                }
                if (resource.memory != VK_NULL_HANDLE) {
                    vkFreeMemory(m_device, resource.memory, nullptr);
                }
            }
        }
        m_rhiResources.clear();
        m_rhiShaders.clear();
        m_rhiPipelineLayouts.clear();
        m_rhiPipelines.clear();
        m_rhiImageViews.clear();
        m_rhiImageViewResources.clear();
        m_rhiDescriptors.clear();
        m_nextRhiResourceHandle = 1;
        m_nextRhiDescriptorHandle = 1;
        m_nextRhiShaderHandle = 1;
        m_nextRhiPipelineLayoutHandle = 1;
        m_nextRhiPipelineHandle = 1;
        DestroyFrameResources();
        DestroySwapChain();
        if (m_device != VK_NULL_HANDLE) {
            vkDestroyDevice(m_device, nullptr);
            m_device = VK_NULL_HANDLE;
        }
        if (m_surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
            m_surface = VK_NULL_HANDLE;
        }
        if (m_instance != VK_NULL_HANDLE) {
            vkDestroyInstance(m_instance, nullptr);
            m_instance = VK_NULL_HANDLE;
        }
        m_physicalDevice = VK_NULL_HANDLE;
        m_graphicsQueue = VK_NULL_HANDLE;
        m_computeQueue = VK_NULL_HANDLE;
        m_presentQueue = VK_NULL_HANDLE;
        m_capabilities = {};
        m_hasDedicatedComputeQueue = false;
    }
} // namespace SasamiRenderer
#endif
