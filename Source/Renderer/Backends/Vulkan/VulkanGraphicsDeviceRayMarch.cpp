// VulkanGraphicsDeviceRayMarch.cpp
// The ray-march background path: its own render pass, uniform buffer, descriptor set, pipeline
// and per-image framebuffers, none of which the mesh paths touch.
#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"
#include "Renderer/Backends/Vulkan/VulkanGraphicsDeviceUtils.h"
#include "Renderer/Resources/ShaderCompilationService.h"

#if RHI_VULKAN

#include "Foundation/Tools/DebugOutput.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

namespace SasamiEngine::Render
{
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
                        0, &m_nativeRayMarchUboMapped) != VK_SUCCESS || !m_nativeRayMarchUboMapped) {
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
        // Tells RayMarch_PS.hlsl to tone map its sky: native backends have no post-process pass,
        // while the D3D12 path leaves this at 0 and tone maps downstream.
        constants.extra1[3]   = 1.0f;
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

} // namespace SasamiEngine::Render
#endif
