// VulkanGraphicsDevicePipeline.cpp
// RHI resource creation and graphics pipeline state for Vulkan.
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
        VulkanRhiPipelineLayout layout{};
        setBindings.reserve(desc.bindingCount + desc.staticSamplerCount);
        pushConstants.reserve(desc.bindingCount);
        for (uint32_t i = 0; i < desc.bindingCount; ++i) {
            const RhiBindingRangeDesc& binding = desc.bindings[i];
            if (binding.registerSpace != 0 || binding.baseRegister >= 100 ||
                binding.descriptorCount == 0 || binding.descriptorCount > 100 - binding.baseRegister) {
                return {};
            }
            if (binding.type == RhiBindingType::RootConstants) {
                VkPushConstantRange range{};
                range.stageFlags = ToVkShaderStages(binding.visibility);
                range.offset = 0;
                range.size = binding.rootConstantCount * sizeof(uint32_t);
                pushConstants.push_back(range);
                continue;
            }

            VkDescriptorSetLayoutBinding setBinding{};
            setBinding.binding = ToVkBindingNumber(binding.type, binding.baseRegister);
            setBinding.descriptorType = ToVkDescriptorType(binding);
            setBinding.descriptorCount = binding.descriptorCount;
            setBinding.stageFlags = ToVkShaderStages(binding.visibility);
            setBindings.push_back(setBinding);
        }

        layout.bindings.resize(desc.bindingCount);
        for (uint32_t i = 0; i < desc.bindingCount; ++i) {
            const RhiBindingRangeDesc& binding = desc.bindings[i];
            if (binding.type == RhiBindingType::RootConstants) {
                continue;
            }
            layout.bindings[i].binding = ToVkBindingNumber(binding.type, binding.baseRegister);
            layout.bindings[i].descriptorCount = binding.descriptorCount;
            layout.bindings[i].descriptorType = ToVkDescriptorType(binding);
            layout.bindings[i].valid = true;
        }

        layout.immutableSamplers.reserve(desc.staticSamplerCount);
        for (uint32_t i = 0; i < desc.staticSamplerCount; ++i) {
            const RhiStaticSamplerDesc& source = desc.staticSamplers[i];
            const bool bindingCollision = std::any_of(
                setBindings.begin(), setBindings.end(),
                [&](const VkDescriptorSetLayoutBinding& binding) {
                    return binding.binding == ToVkBindingNumber(RhiBindingType::Sampler, source.shaderRegister);
                });
            if (bindingCollision || source.registerSpace != 0 || source.shaderRegister >= 100) {
                for (VkSampler sampler : layout.immutableSamplers) {
                    vkDestroySampler(m_device, sampler, nullptr);
                }
                return {};
            }

            VkSamplerCreateInfo samplerInfo{};
            samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerInfo.magFilter = source.linearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            samplerInfo.minFilter = source.linearFilter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            samplerInfo.mipmapMode = source.linearFilter
                ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            samplerInfo.addressModeU = source.clamp
                ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                : VK_SAMPLER_ADDRESS_MODE_REPEAT;
            samplerInfo.addressModeV = samplerInfo.addressModeU;
            samplerInfo.addressModeW = samplerInfo.addressModeU;
            samplerInfo.minLod = 0.0f;
            samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
            VkSampler sampler = VK_NULL_HANDLE;
            if (vkCreateSampler(m_device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
                for (VkSampler created : layout.immutableSamplers) {
                    vkDestroySampler(m_device, created, nullptr);
                }
                return {};
            }
            layout.immutableSamplers.push_back(sampler);

            VkDescriptorSetLayoutBinding samplerBinding{};
            samplerBinding.binding = ToVkBindingNumber(RhiBindingType::Sampler, source.shaderRegister);
            samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            samplerBinding.descriptorCount = 1;
            samplerBinding.stageFlags = ToVkShaderStages(source.visibility);
            samplerBinding.pImmutableSamplers = &layout.immutableSamplers.back();
            setBindings.push_back(samplerBinding);
        }
        if (!setBindings.empty()) {
            VkDescriptorSetLayoutCreateInfo setInfo{};
            setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            setInfo.bindingCount = static_cast<uint32_t>(setBindings.size());
            setInfo.pBindings = setBindings.data();
            if (vkCreateDescriptorSetLayout(m_device, &setInfo, nullptr, &layout.descriptorSetLayout) != VK_SUCCESS) {
                for (VkSampler sampler : layout.immutableSamplers) {
                    vkDestroySampler(m_device, sampler, nullptr);
                }
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
            for (VkSampler sampler : layout.immutableSamplers) {
                vkDestroySampler(m_device, sampler, nullptr);
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
        raster.depthBiasEnable = (desc.raster.depthBias != 0 || desc.raster.slopeScaledDepthBias != 0.0f) ? VK_TRUE : VK_FALSE;
        raster.depthBiasConstantFactor = static_cast<float>(desc.raster.depthBias);
        raster.depthBiasClamp = desc.raster.depthBiasClamp;
        raster.depthBiasSlopeFactor = desc.raster.slopeScaledDepthBias;

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
        pipeline.layout = desc.layout;
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

        VulkanRhiPipeline pipeline{};
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        if (desc.layout.IsValid()) {
            const auto layoutIt = m_rhiPipelineLayouts.find(desc.layout.id);
            if (layoutIt == m_rhiPipelineLayouts.end() || layoutIt->second.pipelineLayout == VK_NULL_HANDLE) {
                vkDestroyShaderModule(m_device, shader, nullptr);
                return {};
            }
            pipelineLayout = layoutIt->second.pipelineLayout;
            pipeline.layout = desc.layout;
        } else {
            VkPipelineLayoutCreateInfo layoutInfo{};
            layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &pipeline.ownedPipelineLayout) != VK_SUCCESS) {
                vkDestroyShaderModule(m_device, shader, nullptr);
                return {};
            }
            pipelineLayout = pipeline.ownedPipelineLayout;
        }

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shader;
        pipelineInfo.stage.pName = desc.shader.entryPoint ? desc.shader.entryPoint : "main";
        pipelineInfo.layout = pipelineLayout;

        const VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline.pipeline);
        vkDestroyShaderModule(m_device, shader, nullptr);
        if (result != VK_SUCCESS) {
            if (pipeline.ownedPipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(m_device, pipeline.ownedPipelineLayout, nullptr);
            }
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

    bool VulkanGraphicsDevice::EnsureRhiDescriptorPool()
    {
        if (m_rhiDescriptorPool != VK_NULL_HANDLE) {
            return true;
        }
        if (m_device == VK_NULL_HANDLE) {
            return false;
        }

        const std::array<VkDescriptorPoolSize, 5> poolSizes = {{
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192 },
            { VK_DESCRIPTOR_TYPE_SAMPLER, 2048 },
        }};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 8192;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        return vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_rhiDescriptorPool) == VK_SUCCESS;
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
        const auto previousView = m_rhiImageViews.find(destination.ptr);
        if (previousView != m_rhiImageViews.end() && previousView->second != VK_NULL_HANDLE) {
            vkDestroyImageView(m_device, previousView->second, nullptr);
        }
        m_rhiImageViews[destination.ptr] = view;
        m_rhiImageViewResources[destination.ptr] = resourceHandle.id;
        VulkanRhiDescriptor descriptor{};
        descriptor.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        descriptor.imageView = view;
        descriptor.resourceId = resourceHandle.id;
        m_rhiDescriptors[destination.ptr] = descriptor;
        return true;
    }

    bool VulkanGraphicsDevice::CreateRhiBufferShaderResourceView(RhiBufferHandle bufferHandle,
                                                                 const RhiBufferViewDesc& desc,
                                                                 RhiCpuDescriptorHandle destination)
    {
        const auto resourceIt = m_rhiResources.find(bufferHandle.id);
        if (resourceIt == m_rhiResources.end() || resourceIt->second.buffer == VK_NULL_HANDLE ||
            !destination.IsValid() || desc.offset >= resourceIt->second.sizeInBytes) {
            return false;
        }
        const uint64_t available = resourceIt->second.sizeInBytes - desc.offset;
        const uint64_t range = desc.sizeInBytes == 0 ? available : desc.sizeInBytes;
        if (range == 0 || range > available) {
            return false;
        }

        VulkanRhiDescriptor descriptor{};
        descriptor.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        descriptor.buffer = resourceIt->second.buffer;
        descriptor.offset = desc.offset;
        descriptor.range = range;
        descriptor.resourceId = bufferHandle.id;
        m_rhiDescriptors[destination.ptr] = descriptor;
        return true;
    }

    bool VulkanGraphicsDevice::CreateRhiUnorderedAccessView(RhiTextureHandle texture,
                                                            const RhiTextureViewDesc& desc,
                                                            RhiCpuDescriptorHandle destination)
    {
        const auto resourceIt = m_rhiResources.find(texture.id);
        if (resourceIt == m_rhiResources.end() || resourceIt->second.image == VK_NULL_HANDLE || !destination.IsValid()) return false;
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = resourceIt->second.image;
        info.viewType = ToVkImageViewType(desc.dimension);
        info.format = ToVkFormat(desc.format);
        info.subresourceRange.aspectMask = ToVkAspectMask(desc.format);
        info.subresourceRange.baseMipLevel = desc.baseMipLevel;
        info.subresourceRange.levelCount = desc.mipLevelCount;
        info.subresourceRange.baseArrayLayer = desc.baseArrayLayer;
        info.subresourceRange.layerCount = desc.arrayLayerCount;
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(m_device, &info, nullptr, &view) != VK_SUCCESS) return false;
        const auto old = m_rhiImageViews.find(destination.ptr);
        if (old != m_rhiImageViews.end() && old->second != VK_NULL_HANDLE) vkDestroyImageView(m_device, old->second, nullptr);
        m_rhiImageViews[destination.ptr] = view;
        m_rhiImageViewResources[destination.ptr] = texture.id;
        VulkanRhiDescriptor descriptor{};
        descriptor.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        descriptor.imageView = view;
        descriptor.resourceId = texture.id;
        m_rhiDescriptors[destination.ptr] = descriptor;
        return true;
    }

    bool VulkanGraphicsDevice::CreateRhiBufferUnorderedAccessView(RhiBufferHandle buffer,
                                                                  const RhiBufferViewDesc& desc,
                                                                  RhiCpuDescriptorHandle destination)
    {
        return CreateRhiBufferShaderResourceView(buffer, desc, destination);
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

} // namespace SasamiEngine::Render
#endif
