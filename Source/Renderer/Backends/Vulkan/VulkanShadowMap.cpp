// VulkanShadowMap.cpp
#include "Renderer/Backends/Vulkan/VulkanShadowMap.h"

#if RHI_VULKAN

#include "Renderer/Backends/Vulkan/VulkanGraphicsDeviceUtils.h"
#include "Renderer/Resources/ShaderCompilationService.h"
#include "Renderer/Structures/Skeleton.h"
#include "Foundation/Tools/DebugOutput.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <wrl/client.h>

namespace SasamiEngine::Render
{
    namespace
    {
        // Mirrors the main skinned mesh path's per-frame slot budget (VulkanGraphicsDevice.cpp's
        // kMaxSimultaneousSkinnedDraws) so a scene that fits within the main path's skinned draw
        // budget also fits within the shadow pass's bone UBO.
        constexpr uint32_t kMaxShadowSkinnedCasters = 16;

        // Push constants for both caster pipelines: lightViewProjection + model = exactly 128
        // bytes, matching Vulkan's guaranteed minimum (same cap as VulkanNativeMeshPushConstants).
        struct VulkanShadowPushConstants
        {
            float lightViewProjection[16];
            float model[16];
        };
        static_assert(sizeof(VulkanShadowPushConstants) == 128,
                      "Shadow caster push constants must stay within the guaranteed 128-byte minimum.");

        const float kIdentity4x4[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };
    } // namespace

    uint32_t VulkanShadowMap::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const
    {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            if ((typeBits & (1u << i)) &&
                (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    bool VulkanShadowMap::TransitionDepthImageToShaderReadOnly(VkImage image)
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(m_device, &allocInfo, &cmd) != VK_SUCCESS) {
            return false;
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        bool ok = vkBeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS;
        if (ok) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);
            ok = vkEndCommandBuffer(cmd) == VK_SUCCESS;
        }
        if (ok) {
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            ok = vkQueueSubmit(m_graphicsQueue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS &&
                 vkQueueWaitIdle(m_graphicsQueue) == VK_SUCCESS;
        }
        vkFreeCommandBuffers(m_device, m_commandPool, 1, &cmd);
        return ok;
    }

    bool VulkanShadowMap::CreateDummyDepthResources()
    {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_D32_SFLOAT;
        imageInfo.extent = { 1u, 1u, 1u };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_dummyDepthImage) != VK_SUCCESS) {
            return false;
        }

        VkMemoryRequirements reqs{};
        vkGetImageMemoryRequirements(m_device, m_dummyDepthImage, &reqs);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = reqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(m_device, &allocInfo, nullptr, &m_dummyDepthMemory) != VK_SUCCESS ||
            vkBindImageMemory(m_device, m_dummyDepthImage, m_dummyDepthMemory, 0) != VK_SUCCESS) {
            return false;
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_dummyDepthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_dummyDepthView) != VK_SUCCESS) {
            return false;
        }

        return TransitionDepthImageToShaderReadOnly(m_dummyDepthImage);
    }

    bool VulkanShadowMap::CreateComparisonSampler()
    {
        // Mirrors Dx11ShadowMap's comparison sampler: linear min/mag, point mip, clamp-to-border
        // with an all-zero border (so any UV that slips just past the edge samples as "occluded
        // enough to reach 0", but SampleShadow already early-outs on out-of-range UVs, so the
        // border colour is a defensive fallback rather than a load-bearing value). GREATER_EQUAL
        // must exactly match Dx11ShadowMap's D3D11_COMPARISON_GREATER_EQUAL: reverse-Z means the
        // sampled (occluder) depth compares correctly against the fragment's own biased depth only
        // in this direction -- flipping it would invert every shadow in the scene.
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        samplerInfo.compareEnable = VK_TRUE;
        samplerInfo.compareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
        samplerInfo.maxLod = 0.0f;
        return vkCreateSampler(m_device, &samplerInfo, nullptr, &m_comparisonSampler) == VK_SUCCESS;
    }

    bool VulkanShadowMap::CreateRealDepthResources(uint32_t resolution)
    {
        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = VK_FORMAT_D32_SFLOAT;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // UNDEFINED regardless of the image's actual current layout is deliberate here: this
        // attachment is always LOAD_OP_CLEAR'd, so its previous contents (and layout) never need
        // to be preserved, and the render pass performs its own transition to depthRef's layout.
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference depthRef{};
        depthRef.attachment = 0;
        depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &depthRef;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &depthAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS) {
            DebugLog("VulkanShadowMap::Initialize: vkCreateRenderPass failed.\n");
            return false;
        }

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_D32_SFLOAT;
        imageInfo.extent = { resolution, resolution, 1u };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_device, &imageInfo, nullptr, &m_depthImage) != VK_SUCCESS) {
            DebugLog("VulkanShadowMap::Initialize: depth image creation failed.\n");
            return false;
        }

        VkMemoryRequirements reqs{};
        vkGetImageMemoryRequirements(m_device, m_depthImage, &reqs);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = reqs.size;
        allocInfo.memoryTypeIndex = FindMemoryType(reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocInfo.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(m_device, &allocInfo, nullptr, &m_depthMemory) != VK_SUCCESS ||
            vkBindImageMemory(m_device, m_depthImage, m_depthMemory, 0) != VK_SUCCESS) {
            DebugLog("VulkanShadowMap::Initialize: depth memory allocation failed.\n");
            return false;
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_depthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_depthView) != VK_SUCCESS) {
            DebugLog("VulkanShadowMap::Initialize: depth view creation failed.\n");
            return false;
        }

        if (!TransitionDepthImageToShaderReadOnly(m_depthImage)) {
            DebugLog("VulkanShadowMap::Initialize: initial depth layout transition failed.\n");
            return false;
        }

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = m_renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &m_depthView;
        framebufferInfo.width = resolution;
        framebufferInfo.height = resolution;
        framebufferInfo.layers = 1;
        if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_framebuffer) != VK_SUCCESS) {
            DebugLog("VulkanShadowMap::Initialize: vkCreateFramebuffer failed.\n");
            return false;
        }

        return true;
    }

    bool VulkanShadowMap::CreateStaticPipeline()
    {
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.size = sizeof(VulkanShadowPushConstants);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_staticPipelineLayout) != VK_SUCCESS) {
            DebugLog("VulkanShadowMap::Initialize: static pipeline layout creation failed.\n");
            return false;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
        const std::filesystem::path shaderPath =
            ShaderCompilationService::GetShaderSourceRoot() / L"Backend" / L"Native" / L"NativeShadowDepth_VS.hlsl";
        if (!ShaderCompilationService::CompileShader(shaderPath, "VSMain", "vs_6_0", vsBlob, true)) {
            DebugLog("VulkanShadowMap::Initialize: static depth VS SPIR-V compile failed.\n");
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

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        stage.module = vsModule;
        stage.pName = "VSMain";

        // Same 48-byte interleaved vertex layout as the main static mesh pipeline
        // (VulkanGraphicsDeviceNativeMesh.cpp): only POSITION is consumed, but the buffer stride
        // must still match so the vertex puller advances correctly through the shared buffer.
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = 48;
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attribute = MakeNativeMeshAttribute(0, VK_FORMAT_R32G32B32_SFLOAT, 0);

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions = &attribute;

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
        raster.cullMode = VK_CULL_MODE_NONE;  // Matches Dx11ShadowMap's D3D11_CULL_NONE.
        raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        // Reverse-Z: cleared to 0.0 (far), so the nearest occluder (closest to 1.0) must win.
        depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 1;
        pipelineInfo.pStages = &stage;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = m_staticPipelineLayout;
        pipelineInfo.renderPass = m_renderPass;

        const VkResult result =
            vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_staticPipeline);
        vkDestroyShaderModule(m_device, vsModule, nullptr);
        if (result != VK_SUCCESS) {
            DebugLog("VulkanShadowMap::Initialize: static pipeline creation failed.\n");
            return false;
        }
        return true;
    }

    bool VulkanShadowMap::CreateSkinnedPipeline()
    {
        const VkDeviceSize boneBufferBytes = static_cast<VkDeviceSize>(Skeleton::kMaxBones) * 16 * sizeof(float);
        VkPhysicalDeviceProperties physProps{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &physProps);
        const VkDeviceSize minAlign = physProps.limits.minUniformBufferOffsetAlignment;
        VkDeviceSize slotStride = boneBufferBytes;
        if (minAlign > 0) {
            slotStride = (boneBufferBytes + minAlign - 1) / minAlign * minAlign;
        }
        m_boneSlotStride = slotStride;

        VkBufferCreateInfo boneBufInfo{};
        boneBufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        boneBufInfo.size = slotStride * kMaxShadowSkinnedCasters;
        boneBufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        boneBufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &boneBufInfo, nullptr, &m_boneUbo) != VK_SUCCESS) {
            DebugLog("VulkanShadowMap::Initialize: bone UBO creation failed.\n");
            return false;
        }

        VkMemoryRequirements boneReqs{};
        vkGetBufferMemoryRequirements(m_device, m_boneUbo, &boneReqs);
        VkMemoryAllocateInfo boneAllocInfo{};
        boneAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        boneAllocInfo.allocationSize = boneReqs.size;
        boneAllocInfo.memoryTypeIndex = FindMemoryType(boneReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (boneAllocInfo.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(m_device, &boneAllocInfo, nullptr, &m_boneUboMemory) != VK_SUCCESS ||
            vkBindBufferMemory(m_device, m_boneUbo, m_boneUboMemory, 0) != VK_SUCCESS) {
            DebugLog("VulkanShadowMap::Initialize: bone UBO memory failed.\n");
            return false;
        }
        if (vkMapMemory(m_device, m_boneUboMemory, 0, boneBufInfo.size, 0, &m_boneUboMapped) != VK_SUCCESS ||
            !m_boneUboMapped) {
            DebugLog("VulkanShadowMap::Initialize: bone UBO map failed.\n");
            return false;
        }

        VkDescriptorSetLayoutBinding boneBinding{};
        boneBinding.binding = 0;
        boneBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        boneBinding.descriptorCount = 1;
        boneBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        VkDescriptorSetLayoutCreateInfo boneLayoutInfo{};
        boneLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        boneLayoutInfo.bindingCount = 1;
        boneLayoutInfo.pBindings = &boneBinding;
        if (vkCreateDescriptorSetLayout(m_device, &boneLayoutInfo, nullptr, &m_boneDescSetLayout) != VK_SUCCESS) {
            DebugLog("VulkanShadowMap::Initialize: bone descriptor set layout failed.\n");
            return false;
        }

        VkDescriptorPoolSize bonePoolSize{};
        bonePoolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        bonePoolSize.descriptorCount = 1;
        VkDescriptorPoolCreateInfo bonePoolInfo{};
        bonePoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        bonePoolInfo.maxSets = 1;
        bonePoolInfo.poolSizeCount = 1;
        bonePoolInfo.pPoolSizes = &bonePoolSize;
        if (vkCreateDescriptorPool(m_device, &bonePoolInfo, nullptr, &m_boneDescPool) != VK_SUCCESS) {
            DebugLog("VulkanShadowMap::Initialize: bone descriptor pool failed.\n");
            return false;
        }

        VkDescriptorSetAllocateInfo boneSetAlloc{};
        boneSetAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        boneSetAlloc.descriptorPool = m_boneDescPool;
        boneSetAlloc.descriptorSetCount = 1;
        boneSetAlloc.pSetLayouts = &m_boneDescSetLayout;
        if (vkAllocateDescriptorSets(m_device, &boneSetAlloc, &m_boneDescSet) != VK_SUCCESS) {
            DebugLog("VulkanShadowMap::Initialize: bone descriptor set alloc failed.\n");
            return false;
        }

        VkDescriptorBufferInfo boneBufferInfo{};
        boneBufferInfo.buffer = m_boneUbo;
        boneBufferInfo.range = slotStride;
        VkWriteDescriptorSet boneWrite{};
        boneWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        boneWrite.dstSet = m_boneDescSet;
        boneWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        boneWrite.descriptorCount = 1;
        boneWrite.pBufferInfo = &boneBufferInfo;
        vkUpdateDescriptorSets(m_device, 1, &boneWrite, 0, nullptr);

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.size = sizeof(VulkanShadowPushConstants);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &m_boneDescSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_skinnedPipelineLayout) != VK_SUCCESS) {
            DebugLog("VulkanShadowMap::Initialize: skinned pipeline layout creation failed.\n");
            return false;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
        const std::filesystem::path shaderPath = ShaderCompilationService::GetShaderSourceRoot() /
            L"Backend" / L"Native" / L"NativeShadowDepthSkinned_VS.hlsl";
        if (!ShaderCompilationService::CompileShader(shaderPath, "VSMain", "vs_6_0", vsBlob, true)) {
            DebugLog("VulkanShadowMap::Initialize: skinned depth VS SPIR-V compile failed.\n");
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

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        stage.module = vsModule;
        stage.pName = "VSMain";

        // Same 68-byte skinned interleaved layout as the main skinned mesh pipeline
        // (VulkanGraphicsDeviceSkinnedMesh.cpp): POSITION, JOINTS_0, WEIGHTS_0.
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = 68;
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 3> attributes = {
            MakeNativeMeshAttribute(0, VK_FORMAT_R32G32B32_SFLOAT, 0),
            MakeNativeMeshAttribute(1, VK_FORMAT_R8G8B8A8_UINT, 48),
            MakeNativeMeshAttribute(2, VK_FORMAT_R32G32B32A32_SFLOAT, 52),
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
        depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 1;
        pipelineInfo.pStages = &stage;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = m_skinnedPipelineLayout;
        pipelineInfo.renderPass = m_renderPass;

        const VkResult result =
            vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_skinnedPipeline);
        vkDestroyShaderModule(m_device, vsModule, nullptr);
        if (result != VK_SUCCESS) {
            DebugLog("VulkanShadowMap::Initialize: skinned pipeline creation failed.\n");
            return false;
        }
        return true;
    }

    bool VulkanShadowMap::Initialize(VkDevice device,
                                     VkPhysicalDevice physicalDevice,
                                     VkCommandPool commandPool,
                                     VkQueue graphicsQueue,
                                     uint32_t resolution)
    {
        Destroy();

        m_device = device;
        m_physicalDevice = physicalDevice;
        m_commandPool = commandPool;
        m_graphicsQueue = graphicsQueue;

        if (m_device == VK_NULL_HANDLE) {
            return false;
        }

        if (!CreateComparisonSampler() || !CreateDummyDepthResources()) {
            DebugLog("VulkanShadowMap::Initialize: fallback resources failed.\n");
            Destroy();
            return false;
        }

        // Matches Dx11ShadowMap::Initialize: resolution == 0 means shadows are disabled this run.
        // The comparison sampler and dummy depth view created above stay valid so callers can
        // still bind something, but no real shadow map or caster pipeline is built.
        if (resolution == 0) {
            return true;
        }

        if (!CreateRealDepthResources(resolution) || !CreateStaticPipeline() || !CreateSkinnedPipeline()) {
            // Non-fatal: same as DirectX 11, a failure here falls back to unshadowed rendering
            // rather than failing the whole mesh frame. Keep the dummy/sampler resources created
            // above so descriptor sets referencing this shadow map remain valid.
            DebugLog("VulkanShadowMap::Initialize: shadow map disabled after resource creation failure.\n");
            if (m_renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_renderPass, nullptr);
            if (m_framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
            if (m_depthView != VK_NULL_HANDLE) vkDestroyImageView(m_device, m_depthView, nullptr);
            if (m_depthImage != VK_NULL_HANDLE) vkDestroyImage(m_device, m_depthImage, nullptr);
            if (m_depthMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_depthMemory, nullptr);
            if (m_staticPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_staticPipeline, nullptr);
            if (m_staticPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_staticPipelineLayout, nullptr);
            if (m_skinnedPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_skinnedPipeline, nullptr);
            if (m_skinnedPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_skinnedPipelineLayout, nullptr);
            if (m_boneDescPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_boneDescPool, nullptr);
            if (m_boneDescSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_boneDescSetLayout, nullptr);
            if (m_boneUboMapped != nullptr && m_boneUboMemory != VK_NULL_HANDLE) vkUnmapMemory(m_device, m_boneUboMemory);
            if (m_boneUbo != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_boneUbo, nullptr);
            if (m_boneUboMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_boneUboMemory, nullptr);
            m_renderPass = VK_NULL_HANDLE;
            m_framebuffer = VK_NULL_HANDLE;
            m_depthView = VK_NULL_HANDLE;
            m_depthImage = VK_NULL_HANDLE;
            m_depthMemory = VK_NULL_HANDLE;
            m_staticPipeline = VK_NULL_HANDLE;
            m_staticPipelineLayout = VK_NULL_HANDLE;
            m_skinnedPipeline = VK_NULL_HANDLE;
            m_skinnedPipelineLayout = VK_NULL_HANDLE;
            m_boneDescPool = VK_NULL_HANDLE;
            m_boneDescSetLayout = VK_NULL_HANDLE;
            m_boneDescSet = VK_NULL_HANDLE;
            m_boneUbo = VK_NULL_HANDLE;
            m_boneUboMemory = VK_NULL_HANDLE;
            m_boneUboMapped = nullptr;
            return true;
        }

        m_resolution = resolution;
        m_valid = true;
        return true;
    }

    void VulkanShadowMap::Destroy()
    {
        if (m_device != VK_NULL_HANDLE) {
            if (m_staticPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_staticPipeline, nullptr);
            if (m_staticPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_staticPipelineLayout, nullptr);
            if (m_skinnedPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_skinnedPipeline, nullptr);
            if (m_skinnedPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_skinnedPipelineLayout, nullptr);
            if (m_boneDescPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_boneDescPool, nullptr);
            if (m_boneDescSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_boneDescSetLayout, nullptr);
            if (m_boneUboMapped != nullptr && m_boneUboMemory != VK_NULL_HANDLE) vkUnmapMemory(m_device, m_boneUboMemory);
            if (m_boneUbo != VK_NULL_HANDLE) vkDestroyBuffer(m_device, m_boneUbo, nullptr);
            if (m_boneUboMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_boneUboMemory, nullptr);
            if (m_framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(m_device, m_framebuffer, nullptr);
            if (m_renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(m_device, m_renderPass, nullptr);
            if (m_depthView != VK_NULL_HANDLE) vkDestroyImageView(m_device, m_depthView, nullptr);
            if (m_depthImage != VK_NULL_HANDLE) vkDestroyImage(m_device, m_depthImage, nullptr);
            if (m_depthMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_depthMemory, nullptr);
            if (m_dummyDepthView != VK_NULL_HANDLE) vkDestroyImageView(m_device, m_dummyDepthView, nullptr);
            if (m_dummyDepthImage != VK_NULL_HANDLE) vkDestroyImage(m_device, m_dummyDepthImage, nullptr);
            if (m_dummyDepthMemory != VK_NULL_HANDLE) vkFreeMemory(m_device, m_dummyDepthMemory, nullptr);
            if (m_comparisonSampler != VK_NULL_HANDLE) vkDestroySampler(m_device, m_comparisonSampler, nullptr);
        }

        m_staticPipeline = VK_NULL_HANDLE;
        m_staticPipelineLayout = VK_NULL_HANDLE;
        m_skinnedPipeline = VK_NULL_HANDLE;
        m_skinnedPipelineLayout = VK_NULL_HANDLE;
        m_boneDescSet = VK_NULL_HANDLE;
        m_boneDescPool = VK_NULL_HANDLE;
        m_boneDescSetLayout = VK_NULL_HANDLE;
        m_boneUbo = VK_NULL_HANDLE;
        m_boneUboMemory = VK_NULL_HANDLE;
        m_boneUboMapped = nullptr;
        m_boneSlotStride = 0;
        m_framebuffer = VK_NULL_HANDLE;
        m_renderPass = VK_NULL_HANDLE;
        m_depthView = VK_NULL_HANDLE;
        m_depthImage = VK_NULL_HANDLE;
        m_depthMemory = VK_NULL_HANDLE;
        m_dummyDepthView = VK_NULL_HANDLE;
        m_dummyDepthImage = VK_NULL_HANDLE;
        m_dummyDepthMemory = VK_NULL_HANDLE;
        m_comparisonSampler = VK_NULL_HANDLE;
        m_resolution = 0;
        m_valid = false;
    }

    void VulkanShadowMap::Render(VkCommandBuffer cmd,
                                 const VulkanShadowCaster* casters,
                                 uint32_t casterCount,
                                 const float lightViewProjection[16])
    {
        if (!m_valid || casters == nullptr || casterCount == 0) {
            return;
        }

        VkClearValue clearValue{};
        clearValue.depthStencil.depth = 0.0f;  // Reverse-Z: the far plane is 0.0.
        clearValue.depthStencil.stencil = 0;

        VkRenderPassBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = m_renderPass;
        beginInfo.framebuffer = m_framebuffer;
        beginInfo.renderArea.offset = { 0, 0 };
        beginInfo.renderArea.extent = { m_resolution, m_resolution };
        beginInfo.clearValueCount = 1;
        beginInfo.pClearValues = &clearValue;
        vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.width = static_cast<float>(m_resolution);
        viewport.height = static_cast<float>(m_resolution);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.extent = { m_resolution, m_resolution };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        VkPipeline boundPipeline = VK_NULL_HANDLE;
        uint32_t skinnedSlot = 0;
        for (uint32_t i = 0; i < casterCount; ++i) {
            const VulkanShadowCaster& caster = casters[i];
            if (caster.vertexBuffer == VK_NULL_HANDLE) {
                continue;
            }
            const bool wantsSkinned = caster.boneMatrices != nullptr && m_skinnedPipeline != VK_NULL_HANDLE;
            // Defensive cap matching the main skinned mesh path's per-frame slot budget; a caster
            // beyond the budget is simply skipped rather than corrupting another slot's bones.
            if (wantsSkinned && skinnedSlot >= kMaxShadowSkinnedCasters) {
                continue;
            }

            VulkanShadowPushConstants push{};
            std::memcpy(push.lightViewProjection, lightViewProjection, sizeof(push.lightViewProjection));
            std::memcpy(push.model, caster.model ? caster.model : kIdentity4x4, sizeof(push.model));

            if (wantsSkinned) {
                if (boundPipeline != m_skinnedPipeline) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedPipeline);
                    boundPipeline = m_skinnedPipeline;
                }
                const VkDeviceSize slotOffset = static_cast<VkDeviceSize>(skinnedSlot) * m_boneSlotStride;
                std::memcpy(static_cast<uint8_t*>(m_boneUboMapped) + slotOffset,
                    caster.boneMatrices, static_cast<size_t>(Skeleton::kMaxBones) * 16 * sizeof(float));
                const uint32_t dynamicOffset = static_cast<uint32_t>(slotOffset);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_skinnedPipelineLayout, 0, 1, &m_boneDescSet, 1, &dynamicOffset);
                vkCmdPushConstants(cmd, m_skinnedPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
                ++skinnedSlot;
            } else {
                if (boundPipeline != m_staticPipeline) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_staticPipeline);
                    boundPipeline = m_staticPipeline;
                }
                vkCmdPushConstants(cmd, m_staticPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
            }

            VkBuffer vertexBuffer = caster.vertexBuffer;
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);

            if (caster.indexCount > 0 && caster.indexBuffer != VK_NULL_HANDLE) {
                vkCmdBindIndexBuffer(cmd, caster.indexBuffer, 0,
                    caster.index32Bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
                vkCmdDrawIndexed(cmd, caster.indexCount, 1, 0, 0, 0);
            } else if (caster.vertexCount > 0) {
                vkCmdDraw(cmd, caster.vertexCount, 1, 0, 0);
            }
        }

        vkCmdEndRenderPass(cmd);
    }
} // namespace SasamiEngine::Render
#endif
