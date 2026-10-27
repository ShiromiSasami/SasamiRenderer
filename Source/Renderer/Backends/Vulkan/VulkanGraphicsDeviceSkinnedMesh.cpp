// VulkanGraphicsDeviceSkinnedMesh.cpp
// The skinned variant of the native mesh path: it needs a bone-matrix buffer and its own
// pipeline, but shares the push-constant layout with the static path (VulkanGraphicsDeviceUtils.h).
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
    void VulkanGraphicsDevice::DestroyNativeSkinnedMeshResources()
    {
        if (m_device != VK_NULL_HANDLE) {
            if (m_nativeSkinnedMeshPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(m_device, m_nativeSkinnedMeshPipeline, nullptr);
            }
            if (m_nativeSkinnedMeshPipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(m_device, m_nativeSkinnedMeshPipelineLayout, nullptr);
            }
            // Destroy descriptor pool first (implicitly frees the bone descriptor set)
            if (m_nativeSkinnedMeshBoneDescPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(m_device, m_nativeSkinnedMeshBoneDescPool, nullptr);
            }
            if (m_nativeSkinnedMeshBoneDescSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(m_device, m_nativeSkinnedMeshBoneDescSetLayout, nullptr);
            }
            if (m_nativeSkinnedMeshBoneUboMapped != nullptr && m_nativeSkinnedMeshBoneUboMemory != VK_NULL_HANDLE) {
                vkUnmapMemory(m_device, m_nativeSkinnedMeshBoneUboMemory);
            }
            if (m_nativeSkinnedMeshBoneUbo != VK_NULL_HANDLE) {
                vkDestroyBuffer(m_device, m_nativeSkinnedMeshBoneUbo, nullptr);
            }
            if (m_nativeSkinnedMeshBoneUboMemory != VK_NULL_HANDLE) {
                vkFreeMemory(m_device, m_nativeSkinnedMeshBoneUboMemory, nullptr);
            }
        }

        m_nativeSkinnedMeshPipeline          = VK_NULL_HANDLE;
        m_nativeSkinnedMeshPipelineLayout    = VK_NULL_HANDLE;
        m_nativeSkinnedMeshBoneDescPool      = VK_NULL_HANDLE;
        m_nativeSkinnedMeshBoneDescSetLayout = VK_NULL_HANDLE;
        m_nativeSkinnedMeshBoneDescSet       = VK_NULL_HANDLE;
        m_nativeSkinnedMeshBoneUbo           = VK_NULL_HANDLE;
        m_nativeSkinnedMeshBoneUboMemory     = VK_NULL_HANDLE;
        m_nativeSkinnedMeshBoneUboMapped     = nullptr;
        m_nativeSkinnedMeshBoneSlotStride    = 0;
    }

    bool VulkanGraphicsDevice::EnsureNativeSkinnedMeshResources()
    {
        // Depends on the static mesh render pass, set-0 texture descriptor layout, sampler,
        // and dummy texture — skinned draws are submitted inside the same render pass scope
        // as static draws (see RenderMeshFrame), so no dedicated render pass/framebuffers here.
        if (!EnsureNativeMeshResources()) {
            return false;
        }
        if (m_nativeSkinnedMeshPipeline != VK_NULL_HANDLE) {
            return true;
        }

        DestroyNativeSkinnedMeshResources();

        constexpr uint32_t kMaxSimultaneousSkinnedDraws = 16;
        const VkDeviceSize boneBufferBytes = static_cast<VkDeviceSize>(Skeleton::kMaxBones) * 16 * sizeof(float);

        VkPhysicalDeviceProperties physProps{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &physProps);
        const VkDeviceSize minAlign = physProps.limits.minUniformBufferOffsetAlignment;
        VkDeviceSize slotStride = boneBufferBytes;
        if (minAlign > 0) {
            slotStride = (boneBufferBytes + minAlign - 1) / minAlign * minAlign;
        }
        m_nativeSkinnedMeshBoneSlotStride = slotStride;

        // --- bone palette UBO: persistently-mapped, one aligned slot per simultaneous skinned draw ---
        {
            VkBufferCreateInfo boneBufInfo{};
            boneBufInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            boneBufInfo.size        = slotStride * kMaxSimultaneousSkinnedDraws;
            boneBufInfo.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            boneBufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(m_device, &boneBufInfo, nullptr, &m_nativeSkinnedMeshBoneUbo) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeSkinnedMeshResources: bone UBO create failed.\n");
                DestroyNativeSkinnedMeshResources();
                return false;
            }

            VkMemoryRequirements boneReqs{};
            vkGetBufferMemoryRequirements(m_device, m_nativeSkinnedMeshBoneUbo, &boneReqs);
            VkMemoryAllocateInfo boneAllocInfo{};
            boneAllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            boneAllocInfo.allocationSize  = boneReqs.size;
            boneAllocInfo.memoryTypeIndex = FindMemoryType(boneReqs.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (boneAllocInfo.memoryTypeIndex == UINT32_MAX ||
                vkAllocateMemory(m_device, &boneAllocInfo, nullptr, &m_nativeSkinnedMeshBoneUboMemory) != VK_SUCCESS ||
                vkBindBufferMemory(m_device, m_nativeSkinnedMeshBoneUbo, m_nativeSkinnedMeshBoneUboMemory, 0) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeSkinnedMeshResources: bone UBO memory failed.\n");
                DestroyNativeSkinnedMeshResources();
                return false;
            }
            if (vkMapMemory(m_device, m_nativeSkinnedMeshBoneUboMemory, 0, boneBufInfo.size, 0,
                    &m_nativeSkinnedMeshBoneUboMapped) != VK_SUCCESS || !m_nativeSkinnedMeshBoneUboMapped) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeSkinnedMeshResources: bone UBO map failed.\n");
                DestroyNativeSkinnedMeshResources();
                return false;
            }
        }

        // --- bone descriptor set layout/pool/set (set 1): binding 0 = dynamic bone UBO
        // (vertex stage), binding 1 = the same per-frame constants UBO the static mesh pipeline
        // binds at set1 binding0 (see VulkanGraphicsDeviceNativeMesh.cpp / m_nativeMeshFrameUbo).
        // Reusing one VkDescriptorSet for both keeps the skinned draw loop's existing
        // vkCmdBindDescriptorSets call (RenderMeshFrame) untouched. EnsureNativeMeshResources()
        // above guarantees m_nativeMeshFrameUbo already exists by the time we get here.
        {
            VkDescriptorSetLayoutBinding boneBindings[2]{};
            boneBindings[0].binding         = 0;
            boneBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            boneBindings[0].descriptorCount = 1;
            boneBindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;
            boneBindings[1].binding         = 1;
            boneBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            boneBindings[1].descriptorCount = 1;
            boneBindings[1].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo boneLayoutInfo{};
            boneLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            boneLayoutInfo.bindingCount = static_cast<uint32_t>(std::size(boneBindings));
            boneLayoutInfo.pBindings    = boneBindings;
            if (vkCreateDescriptorSetLayout(m_device, &boneLayoutInfo, nullptr, &m_nativeSkinnedMeshBoneDescSetLayout) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeSkinnedMeshResources: bone descriptor set layout failed.\n");
                DestroyNativeSkinnedMeshResources();
                return false;
            }

            VkDescriptorPoolSize bonePoolSizes[2]{};
            bonePoolSizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            bonePoolSizes[0].descriptorCount = 1;
            bonePoolSizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bonePoolSizes[1].descriptorCount = 1;
            VkDescriptorPoolCreateInfo bonePoolInfo{};
            bonePoolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            bonePoolInfo.maxSets       = 1;
            bonePoolInfo.poolSizeCount = static_cast<uint32_t>(std::size(bonePoolSizes));
            bonePoolInfo.pPoolSizes    = bonePoolSizes;
            if (vkCreateDescriptorPool(m_device, &bonePoolInfo, nullptr, &m_nativeSkinnedMeshBoneDescPool) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeSkinnedMeshResources: bone descriptor pool failed.\n");
                DestroyNativeSkinnedMeshResources();
                return false;
            }

            VkDescriptorSetAllocateInfo boneSetAlloc{};
            boneSetAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            boneSetAlloc.descriptorPool     = m_nativeSkinnedMeshBoneDescPool;
            boneSetAlloc.descriptorSetCount = 1;
            boneSetAlloc.pSetLayouts        = &m_nativeSkinnedMeshBoneDescSetLayout;
            if (vkAllocateDescriptorSets(m_device, &boneSetAlloc, &m_nativeSkinnedMeshBoneDescSet) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeSkinnedMeshResources: bone descriptor set alloc failed.\n");
                DestroyNativeSkinnedMeshResources();
                return false;
            }

            VkDescriptorBufferInfo boneBufferInfo{};
            boneBufferInfo.buffer = m_nativeSkinnedMeshBoneUbo;
            boneBufferInfo.offset = 0;
            boneBufferInfo.range  = slotStride;
            VkDescriptorBufferInfo frameBufferInfo{};
            frameBufferInfo.buffer = m_nativeMeshFrameUbo;
            frameBufferInfo.offset = 0;
            frameBufferInfo.range  = sizeof(VulkanNativeMeshFrameConstants);

            VkWriteDescriptorSet boneWrites[2]{};
            boneWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            boneWrites[0].dstSet          = m_nativeSkinnedMeshBoneDescSet;
            boneWrites[0].dstBinding      = 0;
            boneWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            boneWrites[0].descriptorCount = 1;
            boneWrites[0].pBufferInfo     = &boneBufferInfo;
            boneWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            boneWrites[1].dstSet          = m_nativeSkinnedMeshBoneDescSet;
            boneWrites[1].dstBinding      = 1;
            boneWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            boneWrites[1].descriptorCount = 1;
            boneWrites[1].pBufferInfo     = &frameBufferInfo;
            vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(std::size(boneWrites)), boneWrites, 0, nullptr);
        }

        // --- pipeline layout: set0 = texture (shared with static mesh), set1 = bone UBO ---
        VkPushConstantRange skinnedPushRange{};
        skinnedPushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        skinnedPushRange.offset = 0;
        skinnedPushRange.size = sizeof(VulkanNativeMeshPushConstants);

        const VkDescriptorSetLayout skinnedSetLayouts[] = {
            m_nativeMeshDescSetLayout,
            m_nativeSkinnedMeshBoneDescSetLayout,
        };
        VkPipelineLayoutCreateInfo skinnedLayoutInfo{};
        skinnedLayoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        skinnedLayoutInfo.setLayoutCount         = static_cast<uint32_t>(std::size(skinnedSetLayouts));
        skinnedLayoutInfo.pSetLayouts            = skinnedSetLayouts;
        skinnedLayoutInfo.pushConstantRangeCount = 1;
        skinnedLayoutInfo.pPushConstantRanges    = &skinnedPushRange;
        if (vkCreatePipelineLayout(m_device, &skinnedLayoutInfo, nullptr, &m_nativeSkinnedMeshPipelineLayout) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureNativeSkinnedMeshResources: vkCreatePipelineLayout failed.\n");
            DestroyNativeSkinnedMeshResources();
            return false;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
        const std::filesystem::path skinnedShaderPath =
            ShaderCompilationService::GetShaderSourceRoot() / L"Backend" / L"Native" / L"NativeMeshSkinned.hlsl";
        if (!ShaderCompilationService::CompileShader(skinnedShaderPath, "VSMain", "vs_6_0", vsBlob, true) ||
            !ShaderCompilationService::CompileShader(skinnedShaderPath, "PSMain", "ps_6_0", psBlob, true)) {
            DebugLog("VulkanGraphicsDevice::EnsureNativeSkinnedMeshResources: SPIR-V shader compile failed.\n");
            DestroyNativeSkinnedMeshResources();
            return false;
        }

        VkShaderModuleCreateInfo vsInfo{};
        vsInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vsInfo.codeSize = vsBlob->GetBufferSize();
        vsInfo.pCode = static_cast<const uint32_t*>(vsBlob->GetBufferPointer());
        VkShaderModule vsModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &vsInfo, nullptr, &vsModule) != VK_SUCCESS) {
            DestroyNativeSkinnedMeshResources();
            return false;
        }

        VkShaderModuleCreateInfo psInfo{};
        psInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        psInfo.codeSize = psBlob->GetBufferSize();
        psInfo.pCode = static_cast<const uint32_t*>(psBlob->GetBufferPointer());
        VkShaderModule psModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &psInfo, nullptr, &psModule) != VK_SUCCESS) {
            vkDestroyShaderModule(m_device, vsModule, nullptr);
            DestroyNativeSkinnedMeshResources();
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

        VkVertexInputBindingDescription skinnedBinding{};
        skinnedBinding.binding = 0;
        skinnedBinding.stride = 68;
        skinnedBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 6> skinnedAttributes = {
            MakeNativeMeshAttribute(0, VK_FORMAT_R32G32B32_SFLOAT, 0),
            MakeNativeMeshAttribute(1, VK_FORMAT_R32G32B32_SFLOAT, 12),
            MakeNativeMeshAttribute(2, VK_FORMAT_R32G32B32A32_SFLOAT, 24),
            MakeNativeMeshAttribute(3, VK_FORMAT_R32G32_SFLOAT, 40),
            MakeNativeMeshAttribute(4, VK_FORMAT_R8G8B8A8_UINT, 48),
            MakeNativeMeshAttribute(5, VK_FORMAT_R32G32B32A32_SFLOAT, 52),
        };

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &skinnedBinding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(skinnedAttributes.size());
        vertexInput.pVertexAttributeDescriptions = skinnedAttributes.data();

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
        pipelineInfo.layout = m_nativeSkinnedMeshPipelineLayout;
        pipelineInfo.renderPass = m_nativeMeshRenderPass;

        const VkResult skinnedPipelineResult =
            vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_nativeSkinnedMeshPipeline);
        vkDestroyShaderModule(m_device, psModule, nullptr);
        vkDestroyShaderModule(m_device, vsModule, nullptr);
        if (skinnedPipelineResult != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureNativeSkinnedMeshResources: vkCreateGraphicsPipelines failed.\n");
            DestroyNativeSkinnedMeshResources();
            return false;
        }

        return true;
    }
} // namespace SasamiEngine::Render
#endif
