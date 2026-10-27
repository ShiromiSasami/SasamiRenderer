// VulkanAmbientOcclusionPipelines.cpp
// The four graphics pipelines behind VulkanAmbientOcclusion: the depth + world-normal prepass
// (static and skinned) and the two fullscreen passes (SSAO and its cross-bilateral blur).
//
// Split out from VulkanAmbientOcclusion.cpp purely so neither file carries both the resource
// lifetime and the pipeline state -- they are two different jobs and the combined file would be
// well past the point where it stops being readable.
#include "Renderer/Backends/Vulkan/VulkanAmbientOcclusionInternal.h"

#if RHI_VULKAN

#include "Renderer/Backends/Vulkan/VulkanGraphicsDeviceUtils.h"  // MakeNativeMeshAttribute
#include "Renderer/Resources/ShaderCompilationService.h"
#include "Renderer/Structures/Skeleton.h"

#include <filesystem>
#include <wrl/client.h>

// <wrl/client.h> pulls in windows.h, whose min/max macros break any later use of the std::
// templates. Same guard the DirectX 12 backend applies for the same reason.
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
        // Interleaved vertex strides of the buffers the prepass reads, shared with the forward
        // path: 48 bytes for static vertices (position/normal/colour/uv) and 68 for skinned ones
        // (the same plus packed bone indices and weights). The prepass consumes only a subset of
        // the attributes but must declare the full stride so the vertex puller steps correctly.
        constexpr uint32_t kStaticVertexStride = 48u;
        constexpr uint32_t kSkinnedVertexStride = 68u;

        VkPipelineInputAssemblyStateCreateInfo MakeTriangleListInputAssembly()
        {
            VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            return inputAssembly;
        }

        VkPipelineViewportStateCreateInfo MakeDynamicViewportState()
        {
            VkPipelineViewportStateCreateInfo viewportState{};
            viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportState.viewportCount = 1;
            viewportState.scissorCount = 1;
            return viewportState;
        }

        VkPipelineRasterizationStateCreateInfo MakeNoCullRasterization()
        {
            // Cull NONE / front-face CLOCKWISE, matching the main native mesh pipeline exactly: the
            // prepass depth has to agree with the forward pass's depth, and single-sided geometry
            // in this renderer's content is not reliably wound.
            VkPipelineRasterizationStateCreateInfo raster{};
            raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = VK_CULL_MODE_NONE;
            raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
            raster.lineWidth = 1.0f;
            return raster;
        }

        VkPipelineMultisampleStateCreateInfo MakeSingleSampleState()
        {
            VkPipelineMultisampleStateCreateInfo multisample{};
            multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            return multisample;
        }

        VkPipelineColorBlendAttachmentState MakeOpaqueBlendAttachment()
        {
            VkPipelineColorBlendAttachmentState attachment{};
            attachment.blendEnable = VK_FALSE;
            attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            return attachment;
        }
    } // namespace

    VkShaderModule VulkanAmbientOcclusion::CreateShaderModule(const wchar_t* relativeShaderPath,
                                                              const char* entryPoint,
                                                              const char* target) const
    {
        Microsoft::WRL::ComPtr<ID3DBlob> blob;
        const std::filesystem::path shaderPath =
            ShaderCompilationService::GetShaderSourceRoot() / relativeShaderPath;
        if (!ShaderCompilationService::CompileShader(shaderPath, entryPoint, target, blob, true)) {
            DebugLog("VulkanAmbientOcclusion: SPIR-V shader compile failed.\n");
            return VK_NULL_HANDLE;
        }

        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = blob->GetBufferSize();
        moduleInfo.pCode = static_cast<const uint32_t*>(blob->GetBufferPointer());
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: vkCreateShaderModule failed.\n");
            return VK_NULL_HANDLE;
        }
        return module;
    }

    bool VulkanAmbientOcclusion::CreatePrepassStaticPipeline()
    {
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.size = sizeof(VulkanAoPrepassPushConstants);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_prepassStaticPipelineLayout) != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: static prepass pipeline layout failed.\n");
            return false;
        }

        VkShaderModule vsModule = CreateShaderModule(L"Backend/Native/NativeAoPrepass.hlsl", "VSMain", "vs_6_0");
        VkShaderModule psModule = CreateShaderModule(L"Backend/Native/NativeAoPrepass.hlsl", "PSMain", "ps_6_0");
        if (vsModule == VK_NULL_HANDLE || psModule == VK_NULL_HANDLE) {
            if (vsModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, vsModule, nullptr);
            if (psModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, psModule, nullptr);
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
        binding.stride = kStaticVertexStride;
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        const VkVertexInputAttributeDescription attributes[] = {
            MakeNativeMeshAttribute(0, VK_FORMAT_R32G32B32_SFLOAT, 0),   // POSITION
            MakeNativeMeshAttribute(1, VK_FORMAT_R32G32B32_SFLOAT, 12),  // NORMAL
        };

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 2;
        vertexInput.pVertexAttributeDescriptions = attributes;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly = MakeTriangleListInputAssembly();
        VkPipelineViewportStateCreateInfo viewportState = MakeDynamicViewportState();
        VkPipelineRasterizationStateCreateInfo raster = MakeNoCullRasterization();
        VkPipelineMultisampleStateCreateInfo multisample = MakeSingleSampleState();

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        // Reverse-Z: cleared to 0.0 (far), so the nearest fragment (closest to 1.0) wins with >=.
        // The same compare op the forward mesh pipeline uses, which is what keeps the prepass depth
        // identical to the depth the forward pass would have produced.
        depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        VkPipelineColorBlendAttachmentState blendAttachment = MakeOpaqueBlendAttachment();
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
        pipelineInfo.layout = m_prepassStaticPipelineLayout;
        pipelineInfo.renderPass = m_prepassRenderPass;

        const VkResult result =
            vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_prepassStaticPipeline);
        vkDestroyShaderModule(m_device, psModule, nullptr);
        vkDestroyShaderModule(m_device, vsModule, nullptr);
        if (result != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: static prepass pipeline creation failed.\n");
            return false;
        }
        return true;
    }

    bool VulkanAmbientOcclusion::CreatePrepassSkinnedPipeline()
    {
        // --- bone palette: one dynamic-UBO slot per skinned occluder, written at record time.
        // Lifted wholesale from VulkanShadowMap::CreateSkinnedPipeline, which solves the same
        // problem (push constants are already full at 128 bytes, so the palette needs its own set).
        const VkDeviceSize boneBufferBytes = static_cast<VkDeviceSize>(Skeleton::kMaxBones) * 16 * sizeof(float);
        VkPhysicalDeviceProperties physProps{};
        vkGetPhysicalDeviceProperties(m_physicalDevice, &physProps);
        const VkDeviceSize minAlign = physProps.limits.minUniformBufferOffsetAlignment;
        m_boneSlotStride = minAlign > 0
            ? (boneBufferBytes + minAlign - 1) / minAlign * minAlign
            : boneBufferBytes;

        VkBufferCreateInfo boneBufferInfo{};
        boneBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        boneBufferInfo.size = m_boneSlotStride * kMaxAoSkinnedOccluders;
        boneBufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        boneBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(m_device, &boneBufferInfo, nullptr, &m_boneUbo) != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: bone UBO creation failed.\n");
            return false;
        }

        VkMemoryRequirements boneRequirements{};
        vkGetBufferMemoryRequirements(m_device, m_boneUbo, &boneRequirements);
        VkMemoryAllocateInfo boneAllocateInfo{};
        boneAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        boneAllocateInfo.allocationSize = boneRequirements.size;
        boneAllocateInfo.memoryTypeIndex = FindMemoryType(boneRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (boneAllocateInfo.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(m_device, &boneAllocateInfo, nullptr, &m_boneUboMemory) != VK_SUCCESS ||
            vkBindBufferMemory(m_device, m_boneUbo, m_boneUboMemory, 0) != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: bone UBO memory failed.\n");
            return false;
        }
        if (vkMapMemory(m_device, m_boneUboMemory, 0, boneBufferInfo.size, 0, &m_boneUboMapped) != VK_SUCCESS ||
            m_boneUboMapped == nullptr) {
            DebugLog("VulkanAmbientOcclusion: bone UBO map failed.\n");
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
            DebugLog("VulkanAmbientOcclusion: bone descriptor set layout failed.\n");
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
            DebugLog("VulkanAmbientOcclusion: bone descriptor pool failed.\n");
            return false;
        }

        VkDescriptorSetAllocateInfo boneSetAllocate{};
        boneSetAllocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        boneSetAllocate.descriptorPool = m_boneDescPool;
        boneSetAllocate.descriptorSetCount = 1;
        boneSetAllocate.pSetLayouts = &m_boneDescSetLayout;
        if (vkAllocateDescriptorSets(m_device, &boneSetAllocate, &m_boneDescSet) != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: bone descriptor set allocation failed.\n");
            return false;
        }

        VkDescriptorBufferInfo boneDescriptorInfo{};
        boneDescriptorInfo.buffer = m_boneUbo;
        boneDescriptorInfo.range = m_boneSlotStride;
        VkWriteDescriptorSet boneWrite{};
        boneWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        boneWrite.dstSet = m_boneDescSet;
        boneWrite.dstBinding = 0;
        boneWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        boneWrite.descriptorCount = 1;
        boneWrite.pBufferInfo = &boneDescriptorInfo;
        vkUpdateDescriptorSets(m_device, 1, &boneWrite, 0, nullptr);

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.size = sizeof(VulkanAoPrepassPushConstants);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &m_boneDescSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_prepassSkinnedPipelineLayout) != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: skinned prepass pipeline layout failed.\n");
            return false;
        }

        // The pixel shader is the static prepass's, reused verbatim: both vertex shaders emit the
        // same PSInput, and the normal encode must be identical on both paths anyway.
        VkShaderModule vsModule =
            CreateShaderModule(L"Backend/Native/NativeAoPrepassSkinned.hlsl", "VSMain", "vs_6_0");
        VkShaderModule psModule =
            CreateShaderModule(L"Backend/Native/NativeAoPrepass.hlsl", "PSMain", "ps_6_0");
        if (vsModule == VK_NULL_HANDLE || psModule == VK_NULL_HANDLE) {
            if (vsModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, vsModule, nullptr);
            if (psModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, psModule, nullptr);
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
        binding.stride = kSkinnedVertexStride;
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        // Locations/offsets match the main skinned mesh pipeline
        // (VulkanGraphicsDeviceSkinnedMesh.cpp); colour (24) and UV (40) are left unconsumed.
        const VkVertexInputAttributeDescription attributes[] = {
            MakeNativeMeshAttribute(0, VK_FORMAT_R32G32B32_SFLOAT, 0),      // POSITION
            MakeNativeMeshAttribute(1, VK_FORMAT_R32G32B32_SFLOAT, 12),     // NORMAL
            MakeNativeMeshAttribute(4, VK_FORMAT_R8G8B8A8_UINT, 48),        // JOINTS_0
            MakeNativeMeshAttribute(5, VK_FORMAT_R32G32B32A32_SFLOAT, 52),  // WEIGHTS_0
        };

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 4;
        vertexInput.pVertexAttributeDescriptions = attributes;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly = MakeTriangleListInputAssembly();
        VkPipelineViewportStateCreateInfo viewportState = MakeDynamicViewportState();
        VkPipelineRasterizationStateCreateInfo raster = MakeNoCullRasterization();
        VkPipelineMultisampleStateCreateInfo multisample = MakeSingleSampleState();

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        VkPipelineColorBlendAttachmentState blendAttachment = MakeOpaqueBlendAttachment();
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
        pipelineInfo.layout = m_prepassSkinnedPipelineLayout;
        pipelineInfo.renderPass = m_prepassRenderPass;

        const VkResult result =
            vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_prepassSkinnedPipeline);
        vkDestroyShaderModule(m_device, psModule, nullptr);
        vkDestroyShaderModule(m_device, vsModule, nullptr);
        if (result != VK_SUCCESS) {
            DebugLog("VulkanAmbientOcclusion: skinned prepass pipeline creation failed.\n");
            return false;
        }
        return true;
    }

    // Shared body of the two fullscreen passes: identical state except for the pixel shader, the
    // descriptor set layout and the render pass. Both draw SSAO_VS.hlsl's SV_VertexID-generated
    // triangle with no vertex buffer and no depth test, mirroring the DirectX 12 SSAO/SSAOBlur PSOs
    // (RenderPipelineStateCacheSsao.cpp: DepthEnable FALSE, CullMode NONE, empty input layout).
    namespace
    {
        bool CreateFullscreenAoPipeline(VkDevice device,
                                        VkShaderModule vsModule,
                                        VkShaderModule psModule,
                                        VkDescriptorSetLayout descSetLayout,
                                        VkRenderPass renderPass,
                                        const char* label,
                                        VkPipelineLayout& outPipelineLayout,
                                        VkPipeline& outPipeline)
        {
            VkPipelineLayoutCreateInfo layoutInfo{};
            layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layoutInfo.setLayoutCount = 1;
            layoutInfo.pSetLayouts = &descSetLayout;
            if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &outPipelineLayout) != VK_SUCCESS) {
                DebugLog("VulkanAmbientOcclusion: fullscreen pipeline layout failed.\n");
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

            VkPipelineVertexInputStateCreateInfo vertexInput{};
            vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            VkPipelineInputAssemblyStateCreateInfo inputAssembly = MakeTriangleListInputAssembly();
            VkPipelineViewportStateCreateInfo viewportState = MakeDynamicViewportState();
            VkPipelineRasterizationStateCreateInfo raster = MakeNoCullRasterization();
            VkPipelineMultisampleStateCreateInfo multisample = MakeSingleSampleState();

            VkPipelineDepthStencilStateCreateInfo depthStencil{};
            depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depthStencil.depthTestEnable = VK_FALSE;
            depthStencil.depthWriteEnable = VK_FALSE;
            depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;

            VkPipelineColorBlendAttachmentState blendAttachment = MakeOpaqueBlendAttachment();
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
            pipelineInfo.layout = outPipelineLayout;
            pipelineInfo.renderPass = renderPass;

            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &outPipeline) !=
                VK_SUCCESS) {
                DebugLog("VulkanAmbientOcclusion: fullscreen pipeline creation failed: ");
                DebugLog(label);
                DebugLog("\n");
                return false;
            }
            return true;
        }
    } // namespace

    bool VulkanAmbientOcclusion::CreateSsaoPipeline()
    {
        VkShaderModule vsModule =
            CreateShaderModule(L"Effects/AmbientOcclusion/SSAO/SSAO_VS.hlsl", "VSMain", "vs_6_0");
        VkShaderModule psModule =
            CreateShaderModule(L"Effects/AmbientOcclusion/SSAO/SSAO_PS.hlsl", "PSMain", "ps_6_0");
        bool created = false;
        if (vsModule != VK_NULL_HANDLE && psModule != VK_NULL_HANDLE) {
            created = CreateFullscreenAoPipeline(m_device, vsModule, psModule, m_ssaoDescSetLayout,
                                                 m_ssaoRenderPass, "SSAO",
                                                 m_ssaoPipelineLayout, m_ssaoPipeline);
        }
        if (vsModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, vsModule, nullptr);
        if (psModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, psModule, nullptr);
        return created;
    }

    bool VulkanAmbientOcclusion::CreateBlurPipeline()
    {
        VkShaderModule vsModule =
            CreateShaderModule(L"Effects/AmbientOcclusion/SSAO/SSAO_VS.hlsl", "VSMain", "vs_6_0");
        VkShaderModule psModule =
            CreateShaderModule(L"Effects/AmbientOcclusion/SSAO/SSAO_Blur_PS.hlsl", "PSMain", "ps_6_0");
        bool created = false;
        if (vsModule != VK_NULL_HANDLE && psModule != VK_NULL_HANDLE) {
            created = CreateFullscreenAoPipeline(m_device, vsModule, psModule, m_blurDescSetLayout,
                                                 m_blurRenderPass, "SSAOBlur",
                                                 m_blurPipelineLayout, m_blurPipeline);
        }
        if (vsModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, vsModule, nullptr);
        if (psModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, psModule, nullptr);
        return created;
    }
} // namespace SasamiEngine::Render
#endif
