// VulkanGraphicsDevice_Pipeline.cpp
// RHI resource creation and graphics pipeline state for Vulkan.
#include "Renderer/Core/VulkanGraphicsDevice.h"

#if RHI_VULKAN

#include "Foundation/Tools/DebugOutput.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>


namespace SasamiRenderer
{
    RhiTextureHandle VulkanGraphicsDevice::CreateRhiTexture(const RhiTextureDesc& desc)
    {
        if (m_device == VK_NULL_HANDLE || desc.extent.width == 0 || desc.extent.height == 0) {
            return {};
        }
        if (desc.dimension != RhiResourceDimension::Texture2D) {
            return {};
        }

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
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
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &resource.buffer) != VK_SUCCESS) {
            return {};
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(m_device, resource.buffer, &requirements);

        VkMemoryAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
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

        if (initialData && desc.memoryUsage != RhiMemoryUsage::GpuOnly) {
            void* mapped = nullptr;
            if (vkMapMemory(m_device, resource.memory, 0, desc.sizeInBytes, 0, &mapped) == VK_SUCCESS && mapped) {
                std::memcpy(mapped, initialData, static_cast<size_t>(desc.sizeInBytes));
                vkUnmapMemory(m_device, resource.memory);
            }
        }

        const uint64_t id = m_nextRhiResourceHandle++;
        m_rhiResources.emplace(id, resource);
        return RhiBufferHandle{ id };
    }

    RhiShaderHandle VulkanGraphicsDevice::CreateRhiShaderModule(const RhiShaderModuleDesc& desc)
    {
        if (m_device == VK_NULL_HANDLE || !desc.bytecode || desc.bytecodeSize == 0 || (desc.bytecodeSize % sizeof(uint32_t)) != 0) {
            return {};
        }

        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = static_cast<size_t>(desc.bytecodeSize);
        moduleInfo.pCode = static_cast<const uint32_t*>(desc.bytecode);

        VulkanRhiShader shader{};
        shader.stage = desc.stage;
        shader.entryPoint = desc.entryPoint ? desc.entryPoint : "main";
        if (vkCreateShaderModule(m_device, &moduleInfo, nullptr, &shader.module) != VK_SUCCESS) {
            return {};
        }

        const uint64_t id = m_nextRhiShaderHandle++;
        m_rhiShaders.emplace(id, shader);
        return RhiShaderHandle{ id };
    }

    RhiPipelineLayoutHandle VulkanGraphicsDevice::CreateRhiPipelineLayout(const RhiPipelineLayoutDesc& desc)
    {
        if (m_device == VK_NULL_HANDLE) {
            return {};
        }

        std::vector<VkDescriptorSetLayoutBinding> setBindings;
        std::vector<VkPushConstantRange> pushConstants;
        setBindings.reserve(desc.bindingCount);
        pushConstants.reserve(desc.bindingCount);
        for (uint32_t i = 0; i < desc.bindingCount; ++i) {
            const RhiBindingRangeDesc& binding = desc.bindings[i];
            if (binding.type == RhiBindingType::RootConstants) {
                VkPushConstantRange range{};
                range.stageFlags = ToVkShaderStages(binding.visibility);
                range.offset = 0;
                range.size = binding.rootConstantCount * sizeof(uint32_t);
                pushConstants.push_back(range);
                continue;
            }

            VkDescriptorSetLayoutBinding setBinding{};
            setBinding.binding = binding.baseRegister;
            setBinding.descriptorType = ToVkDescriptorType(binding.type);
            setBinding.descriptorCount = binding.descriptorCount;
            setBinding.stageFlags = ToVkShaderStages(binding.visibility);
            setBindings.push_back(setBinding);
        }

        VulkanRhiPipelineLayout layout{};
        if (!setBindings.empty()) {
            VkDescriptorSetLayoutCreateInfo setInfo{};
            setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            setInfo.bindingCount = static_cast<uint32_t>(setBindings.size());
            setInfo.pBindings = setBindings.data();
            if (vkCreateDescriptorSetLayout(m_device, &setInfo, nullptr, &layout.descriptorSetLayout) != VK_SUCCESS) {
                return {};
            }
        }

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = layout.descriptorSetLayout != VK_NULL_HANDLE ? 1u : 0u;
        layoutInfo.pSetLayouts = layout.descriptorSetLayout != VK_NULL_HANDLE ? &layout.descriptorSetLayout : nullptr;
        layoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size());
        layoutInfo.pPushConstantRanges = pushConstants.empty() ? nullptr : pushConstants.data();
        if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &layout.pipelineLayout) != VK_SUCCESS) {
            if (layout.descriptorSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(m_device, layout.descriptorSetLayout, nullptr);
            }
            return {};
        }

        const uint64_t id = m_nextRhiPipelineLayoutHandle++;
        m_rhiPipelineLayouts.emplace(id, layout);
        return RhiPipelineLayoutHandle{ id };
    }

    RhiPipelineHandle VulkanGraphicsDevice::CreateRhiGraphicsPipeline(const RhiGraphicsPipelineDesc& desc)
    {
        if (m_device == VK_NULL_HANDLE || !desc.layout.IsValid()) {
            return {};
        }
        const auto layoutIt = m_rhiPipelineLayouts.find(desc.layout.id);
        if (layoutIt == m_rhiPipelineLayouts.end() || layoutIt->second.pipelineLayout == VK_NULL_HANDLE) {
            return {};
        }

        std::vector<VkShaderModule> temporaryModules;
        std::vector<VkPipelineShaderStageCreateInfo> shaderStages;
        auto addShader = [&](RhiShaderStageFlags stage, VkShaderModule module, const char* entryPoint) {
            if (module == VK_NULL_HANDLE) {
                return;
            }
            const VkShaderStageFlagBits vkStage = ToVkShaderStage(stage);
            if (vkStage == VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM) {
                return;
            }
            VkPipelineShaderStageCreateInfo stageInfo{};
            stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stageInfo.stage = vkStage;
            stageInfo.module = module;
            stageInfo.pName = entryPoint ? entryPoint : "main";
            shaderStages.push_back(stageInfo);
        };

        if (desc.shaderHandles) {
            for (uint32_t i = 0; i < desc.shaderHandleCount; ++i) {
                const auto shaderIt = m_rhiShaders.find(desc.shaderHandles[i].id);
                if (shaderIt != m_rhiShaders.end()) {
                    addShader(shaderIt->second.stage, shaderIt->second.module, shaderIt->second.entryPoint.c_str());
                }
            }
        }
        if (desc.shaders) {
            for (uint32_t i = 0; i < desc.shaderCount; ++i) {
                const RhiShaderModuleDesc& shader = desc.shaders[i];
                if (!shader.bytecode || shader.bytecodeSize == 0 || (shader.bytecodeSize % sizeof(uint32_t)) != 0) {
                    continue;
                }
                VkShaderModuleCreateInfo moduleInfo{};
                moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
                moduleInfo.codeSize = static_cast<size_t>(shader.bytecodeSize);
                moduleInfo.pCode = static_cast<const uint32_t*>(shader.bytecode);
                VkShaderModule module = VK_NULL_HANDLE;
                if (vkCreateShaderModule(m_device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
                    continue;
                }
                temporaryModules.push_back(module);
                addShader(shader.stage, module, shader.entryPoint);
            }
        }
        if (shaderStages.empty()) {
            for (VkShaderModule module : temporaryModules) {
                vkDestroyShaderModule(m_device, module, nullptr);
            }
            return {};
        }

        std::vector<VkVertexInputBindingDescription> bindings(desc.vertexBindingCount);
        for (uint32_t i = 0; i < desc.vertexBindingCount; ++i) {
            bindings[i].binding = desc.vertexBindings[i].binding;
            bindings[i].stride = desc.vertexBindings[i].stride;
            bindings[i].inputRate = desc.vertexBindings[i].inputRate == RhiInputRate::PerInstance
                ? VK_VERTEX_INPUT_RATE_INSTANCE
                : VK_VERTEX_INPUT_RATE_VERTEX;
        }

        std::vector<VkVertexInputAttributeDescription> attributes(desc.vertexAttributeCount);
        for (uint32_t i = 0; i < desc.vertexAttributeCount; ++i) {
            attributes[i].location = i;
            attributes[i].binding = desc.vertexAttributes[i].binding;
            attributes[i].format = ToVkFormat(desc.vertexAttributes[i].format);
            attributes[i].offset = desc.vertexAttributes[i].offset;
        }

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
        vertexInput.pVertexBindingDescriptions = bindings.empty() ? nullptr : bindings.data();
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.empty() ? nullptr : attributes.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = ToVkTopology(desc.topology);

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = ToVkPolygonMode(desc.raster.fillMode);
        raster.cullMode = ToVkCullMode(desc.raster.cullMode);
        raster.frontFace = desc.raster.frontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = desc.depthStencil.depthTestEnabled;
        depthStencil.depthWriteEnable = desc.depthStencil.depthWriteEnabled;
        depthStencil.depthCompareOp = ToVkCompareOp(desc.depthStencil.depthCompare);
        depthStencil.stencilTestEnable = desc.depthStencil.stencilEnabled;

        std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(desc.colorFormatCount);
        for (auto& attachment : blendAttachments) {
            attachment.colorWriteMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            if (desc.blend.alphaBlendEnabled) {
                attachment.blendEnable = VK_TRUE;
                attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                attachment.colorBlendOp = VK_BLEND_OP_ADD;
                attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                attachment.alphaBlendOp = VK_BLEND_OP_ADD;
            }
        }
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
        blend.pAttachments = blendAttachments.empty() ? nullptr : blendAttachments.data();

        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;

        std::vector<VkAttachmentDescription> attachments;
        std::vector<VkAttachmentReference> colorRefs;
        attachments.reserve(desc.colorFormatCount + 1);
        colorRefs.reserve(desc.colorFormatCount);
        for (uint32_t i = 0; i < desc.colorFormatCount; ++i) {
            VkAttachmentDescription attachment{};
            attachment.format = ToVkFormat(desc.colorFormats[i]);
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachments.push_back(attachment);

            VkAttachmentReference ref{};
            ref.attachment = i;
            ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorRefs.push_back(ref);
        }

        VkAttachmentReference depthRef{};
        const bool hasDepth = desc.depthStencilFormat != RhiFormat::Unknown;
        if (hasDepth) {
            VkAttachmentDescription attachment{};
            attachment.format = ToVkFormat(desc.depthStencilFormat);
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthRef.attachment = static_cast<uint32_t>(attachments.size());
            depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachments.push_back(attachment);
        }

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
        subpass.pColorAttachments = colorRefs.empty() ? nullptr : colorRefs.data();
        subpass.pDepthStencilAttachment = hasDepth ? &depthRef : nullptr;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        renderPassInfo.pAttachments = attachments.empty() ? nullptr : attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;

        VulkanRhiPipeline pipeline{};
        if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &pipeline.renderPass) != VK_SUCCESS) {
            for (VkShaderModule module : temporaryModules) {
                vkDestroyShaderModule(m_device, module, nullptr);
            }
            return {};
        }

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
        pipelineInfo.pStages = shaderStages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &raster;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = layoutIt->second.pipelineLayout;
        pipelineInfo.renderPass = pipeline.renderPass;

        const VkResult result = vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline.pipeline);
        for (VkShaderModule module : temporaryModules) {
            vkDestroyShaderModule(m_device, module, nullptr);
        }
        if (result != VK_SUCCESS) {
            vkDestroyRenderPass(m_device, pipeline.renderPass, nullptr);
            return {};
        }

        const uint64_t id = m_nextRhiPipelineHandle++;
        m_rhiPipelines.emplace(id, pipeline);
        return RhiPipelineHandle{ id };
    }

    RhiPipelineHandle VulkanGraphicsDevice::CreateRhiComputePipeline(const RhiComputePipelineDesc& desc)
    {
        if (m_device == VK_NULL_HANDLE || !desc.shader.bytecode || desc.shader.bytecodeSize == 0 ||
            (desc.shader.bytecodeSize % sizeof(uint32_t)) != 0) {
            return {};
        }

        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = static_cast<size_t>(desc.shader.bytecodeSize);
        moduleInfo.pCode = static_cast<const uint32_t*>(desc.shader.bytecode);

        VkShaderModule shader = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &moduleInfo, nullptr, &shader) != VK_SUCCESS) {
            return {};
        }

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

        VulkanRhiPipeline pipeline{};
        if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &pipeline.ownedPipelineLayout) != VK_SUCCESS) {
            vkDestroyShaderModule(m_device, shader, nullptr);
            return {};
        }

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shader;
        pipelineInfo.stage.pName = desc.shader.entryPoint ? desc.shader.entryPoint : "main";
        pipelineInfo.layout = pipeline.ownedPipelineLayout;

        const VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline.pipeline);
        vkDestroyShaderModule(m_device, shader, nullptr);
        if (result != VK_SUCCESS) {
            vkDestroyPipelineLayout(m_device, pipeline.ownedPipelineLayout, nullptr);
            return {};
        }

        const uint64_t id = m_nextRhiPipelineHandle++;
        m_rhiPipelines.emplace(id, pipeline);
        return RhiPipelineHandle{ id };
    }

    RhiDescriptorAllocation VulkanGraphicsDevice::AllocateRhiDescriptors(RhiDescriptorHeapType type,
                                                                        uint32_t count,
                                                                        bool shaderVisible)
    {
        (void)shaderVisible;
        if (count == 0) {
            return {};
        }
        const uint64_t base = m_nextRhiDescriptorHandle;
        m_nextRhiDescriptorHandle += count;

        RhiDescriptorAllocation allocation{};
        allocation.type = type;
        allocation.cpu.ptr = base;
        allocation.gpu.ptr = base;
        allocation.count = count;
        allocation.increment = 1;
        return allocation;
    }

    bool VulkanGraphicsDevice::CreateRhiShaderResourceView(RhiResourceHandle resourceHandle,
                                                           const RhiTextureViewDesc& desc,
                                                           RhiCpuDescriptorHandle destination)
    {
        const auto resourceIt = m_rhiResources.find(resourceHandle.id);
        if (resourceIt == m_rhiResources.end() || resourceIt->second.image == VK_NULL_HANDLE || !destination.IsValid()) {
            return false;
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = resourceIt->second.image;
        viewInfo.viewType = ToVkImageViewType(desc.dimension);
        viewInfo.format = ToVkFormat(desc.format);
        viewInfo.subresourceRange.aspectMask = ToVkAspectMask(desc.format);
        viewInfo.subresourceRange.baseMipLevel = desc.baseMipLevel;
        viewInfo.subresourceRange.levelCount = desc.mipLevelCount;
        viewInfo.subresourceRange.baseArrayLayer = desc.baseArrayLayer;
        viewInfo.subresourceRange.layerCount = desc.arrayLayerCount;

        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(m_device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
            return false;
        }
        m_rhiImageViews[destination.ptr] = view;
        return true;
    }

    bool VulkanGraphicsDevice::CreateRhiRenderTargetView(RhiTextureHandle texture,
                                                         const RhiRenderTargetViewDesc& desc,
                                                         RhiCpuDescriptorHandle destination)
    {
        RhiTextureViewDesc viewDesc{};
        viewDesc.format = desc.format;
        viewDesc.dimension = desc.dimension;
        viewDesc.baseMipLevel = desc.mipLevel;
        viewDesc.mipLevelCount = 1;
        viewDesc.baseArrayLayer = desc.baseArrayLayer;
        viewDesc.arrayLayerCount = desc.arrayLayerCount;
        return CreateRhiShaderResourceView(texture, viewDesc, destination);
    }

    bool VulkanGraphicsDevice::CreateRhiDepthStencilView(RhiTextureHandle texture,
                                                         const RhiDepthStencilViewDesc& desc,
                                                         RhiCpuDescriptorHandle destination)
    {
        RhiTextureViewDesc viewDesc{};
        viewDesc.format = desc.format;
        viewDesc.dimension = desc.dimension;
        viewDesc.baseMipLevel = desc.mipLevel;
        viewDesc.mipLevelCount = 1;
        viewDesc.baseArrayLayer = desc.baseArrayLayer;
        viewDesc.arrayLayerCount = desc.arrayLayerCount;
        return CreateRhiShaderResourceView(texture, viewDesc, destination);
    }

    std::unique_ptr<IRhiCommandEncoder> VulkanGraphicsDevice::CreateCommandEncoder(RhiQueueType queueType)
    {
        auto encoder = std::make_unique<VulkanRhiCommandEncoder>(*this, queueType);
        if (!encoder->IsValid()) {
            return std::make_unique<NullRhiCommandEncoder>();
        }
        return encoder;
    }

    bool VulkanGraphicsDevice::SubmitCommandEncoder(IRhiCommandEncoder& encoder, RhiQueueType queueType)
    {
        auto* vkEncoder = dynamic_cast<VulkanRhiCommandEncoder*>(&encoder);
        if (!vkEncoder || vkEncoder->QueueType() != queueType || m_device == VK_NULL_HANDLE) {
            return false;
        }
        if (!vkEncoder->Finish()) {
            return false;
        }

        VkQueue queue = m_graphicsQueue;
        if (queueType == RhiQueueType::Compute && m_computeQueue != VK_NULL_HANDLE) {
            queue = m_computeQueue;
        }
        if (queue == VK_NULL_HANDLE) {
            return false;
        }

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkCommandBuffer commandBuffer = vkEncoder->CommandBuffer();
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        if (vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
            return false;
        }
        return vkQueueWaitIdle(queue) == VK_SUCCESS;
    }

} // namespace SasamiRenderer
#endif
