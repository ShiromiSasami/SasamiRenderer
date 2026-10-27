// VulkanGraphicsDeviceSky.cpp
// The analytic sky for the Vulkan native path.
//
// Its own file rather than more weight in VulkanGraphicsDevice.cpp, which is already past 2200 real
// lines and on the split list. Shares the native mesh render pass -- the sky is just the first draw
// into the same attachment -- so only the pipeline and its layout live here.

#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"

#if RHI_VULKAN

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Resources/ShaderCompilationService.h"

#include <cstring>
#include <filesystem>

namespace SasamiEngine::Render
{
    namespace
    {
        struct SkyPushConstants
        {
            float invViewProjection[16];
            float cameraPos[4];
            float sunDirection[4];
            float sunColor[4];
            float hasEnvironmentCube;
        };
        static_assert(sizeof(SkyPushConstants) == 116u,
                      "Must stay within the 128 bytes of push constants Vulkan guarantees, and must "
                      "match NativeSky.hlsl's SkyPushConstants byte for byte.");
    }

    void VulkanGraphicsDevice::DestroyNativeSkyResources()
    {
        if (m_device != VK_NULL_HANDLE) {
            if (m_nativeSkyPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(m_device, m_nativeSkyPipeline, nullptr);
            }
            if (m_nativeSkyPipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(m_device, m_nativeSkyPipelineLayout, nullptr);
            }
            // Destroy the descriptor pool first (implicitly frees every set allocated from it).
            if (m_nativeSkyDescPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(m_device, m_nativeSkyDescPool, nullptr);
            }
            if (m_nativeSkyDescSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(m_device, m_nativeSkyDescSetLayout, nullptr);
            }
            if (m_nativeSkyCubeSampler != VK_NULL_HANDLE) {
                vkDestroySampler(m_device, m_nativeSkyCubeSampler, nullptr);
            }
            if (m_nativeSkyDummyCubeImageView != VK_NULL_HANDLE) {
                vkDestroyImageView(m_device, m_nativeSkyDummyCubeImageView, nullptr);
            }
            if (m_nativeSkyDummyCubeImage != VK_NULL_HANDLE) {
                vkDestroyImage(m_device, m_nativeSkyDummyCubeImage, nullptr);
            }
            if (m_nativeSkyDummyCubeMemory != VK_NULL_HANDLE) {
                vkFreeMemory(m_device, m_nativeSkyDummyCubeMemory, nullptr);
            }
        }

        m_nativeSkyPipeline           = VK_NULL_HANDLE;
        m_nativeSkyPipelineLayout     = VK_NULL_HANDLE;
        m_nativeSkyDescPool           = VK_NULL_HANDLE;
        m_nativeSkyDescSetLayout      = VK_NULL_HANDLE;
        m_nativeSkyCubeSampler        = VK_NULL_HANDLE;
        m_nativeSkyDummyCubeImageView = VK_NULL_HANDLE;
        m_nativeSkyDummyCubeImage     = VK_NULL_HANDLE;
        m_nativeSkyDummyCubeMemory    = VK_NULL_HANDLE;
        m_nativeSkyDescSets.clear();
    }

    bool VulkanGraphicsDevice::EnsureNativeSkyResources()
    {
        if (m_nativeSkyPipeline != VK_NULL_HANDLE) {
            return true;
        }
        if (m_nativeSkyFailed) {
            return false;   // one failed compile is enough; do not retry every frame
        }
        if (m_nativeMeshRenderPass == VK_NULL_HANDLE) {
            return false;   // the mesh resources own the render pass this pipeline needs
        }

        // --- environment-cube sampler (single mip, clamped like a skybox should be) ---
        {
            VkSamplerCreateInfo samplerInfo{};
            samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            samplerInfo.magFilter    = VK_FILTER_LINEAR;
            samplerInfo.minFilter    = VK_FILTER_LINEAR;
            samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            samplerInfo.maxLod       = 0.0f;   // the cubes bound here are always a single mip
            if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_nativeSkyCubeSampler) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeSkyResources: cube sampler creation failed.\n");
                m_nativeSkyFailed = true;
                return false;
            }
        }

        // --- 1x1x6 dummy cube (bound when desc.environmentCubeSrv is 0 or unresolved, so the
        // shader's g_envCube/g_envSampler declarations are always satisfied; g_sky.hasEnvironmentCube
        // is what actually keeps its content out of the picture, via ComputeSkyColor() instead) ---
        {
            constexpr uint32_t w = 1, h = 1, faceCount = 6;
            const uint8_t whitePixel[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
            const VkDeviceSize uploadSize = 4 * faceCount;

            VkImageCreateInfo dummyImageInfo{};
            dummyImageInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            dummyImageInfo.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            dummyImageInfo.imageType     = VK_IMAGE_TYPE_2D;
            dummyImageInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
            dummyImageInfo.extent        = { w, h, 1u };
            dummyImageInfo.mipLevels     = 1;
            dummyImageInfo.arrayLayers   = faceCount;
            dummyImageInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
            dummyImageInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
            dummyImageInfo.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            dummyImageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
            dummyImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(m_device, &dummyImageInfo, nullptr, &m_nativeSkyDummyCubeImage) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeSkyResources: dummy cube image create failed.\n");
                DestroyNativeSkyResources();
                m_nativeSkyFailed = true;
                return false;
            }

            VkMemoryRequirements dummyReqs{};
            vkGetImageMemoryRequirements(m_device, m_nativeSkyDummyCubeImage, &dummyReqs);
            VkMemoryAllocateInfo dummyAllocInfo{};
            dummyAllocInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            dummyAllocInfo.allocationSize  = dummyReqs.size;
            dummyAllocInfo.memoryTypeIndex = FindMemoryType(dummyReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (dummyAllocInfo.memoryTypeIndex == UINT32_MAX ||
                vkAllocateMemory(m_device, &dummyAllocInfo, nullptr, &m_nativeSkyDummyCubeMemory) != VK_SUCCESS ||
                vkBindImageMemory(m_device, m_nativeSkyDummyCubeImage, m_nativeSkyDummyCubeMemory, 0) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeSkyResources: dummy cube image memory failed.\n");
                DestroyNativeSkyResources();
                m_nativeSkyFailed = true;
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
                DebugLog("VulkanGraphicsDevice::EnsureNativeSkyResources: dummy cube staging buffer failed.\n");
                DestroyNativeSkyResources();
                m_nativeSkyFailed = true;
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
                        toTransfer.image               = m_nativeSkyDummyCubeImage;
                        toTransfer.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, faceCount };
                        vkCmdPipelineBarrier(uploadCmd,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, nullptr, 0, nullptr, 1, &toTransfer);

                        VkBufferImageCopy copyRegion{};
                        copyRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, faceCount };
                        copyRegion.imageExtent      = { w, h, 1u };
                        vkCmdCopyBufferToImage(uploadCmd, stagingBuf, m_nativeSkyDummyCubeImage,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

                        VkImageMemoryBarrier toRead{};
                        toRead.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                        toRead.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
                        toRead.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
                        toRead.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        toRead.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        toRead.image               = m_nativeSkyDummyCubeImage;
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
                DebugLog("VulkanGraphicsDevice::EnsureNativeSkyResources: dummy cube upload failed.\n");
                DestroyNativeSkyResources();
                m_nativeSkyFailed = true;
                return false;
            }

            VkImageViewCreateInfo dummyViewInfo{};
            dummyViewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            dummyViewInfo.image            = m_nativeSkyDummyCubeImage;
            dummyViewInfo.viewType         = VK_IMAGE_VIEW_TYPE_CUBE;
            dummyViewInfo.format           = VK_FORMAT_R8G8B8A8_UNORM;
            dummyViewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, faceCount };
            if (vkCreateImageView(m_device, &dummyViewInfo, nullptr, &m_nativeSkyDummyCubeImageView) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeSkyResources: dummy cube image view failed.\n");
                DestroyNativeSkyResources();
                m_nativeSkyFailed = true;
                return false;
            }
        }

        // --- descriptor set layout (t0=binding100 sampled cube, s0=binding200 sampler) ---
        // Separate SAMPLED_IMAGE/SAMPLER bindings rather than one COMBINED_IMAGE_SAMPLER: DXC always
        // lowers a plain `TextureCube`/`SamplerState` pair to two distinct SPIR-V bindings under the
        // -fvk-t-shift/-fvk-s-shift 100/200 convention ShaderCompilationService compiles every native
        // shader with (see GetOrCreateNativeMeshTextureDescriptorSet's identical binding 100/200
        // split), so the layout here must match what the compiled shader actually declares.
        {
            VkDescriptorSetLayoutBinding bindings[2]{};
            bindings[0].binding         = 100;
            bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings[1].binding         = 200;
            bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo descLayoutInfo{};
            descLayoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            descLayoutInfo.bindingCount = 2;
            descLayoutInfo.pBindings    = bindings;
            if (vkCreateDescriptorSetLayout(m_device, &descLayoutInfo, nullptr, &m_nativeSkyDescSetLayout) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeSkyResources: descriptor set layout failed.\n");
                DestroyNativeSkyResources();
                m_nativeSkyFailed = true;
                return false;
            }
        }

        // --- descriptor pool + pre-allocated key=0 dummy set ---
        // Small pool: unlike per-draw albedo textures, only a handful of distinct environment cubes
        // are ever bound in one run (usually exactly one, or zero).
        {
            VkDescriptorPoolSize poolSizes[2]{};
            poolSizes[0].type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            poolSizes[0].descriptorCount = 64;
            poolSizes[1].type            = VK_DESCRIPTOR_TYPE_SAMPLER;
            poolSizes[1].descriptorCount = 64;
            VkDescriptorPoolCreateInfo poolInfo{};
            poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.maxSets       = 64;
            poolInfo.poolSizeCount = 2;
            poolInfo.pPoolSizes    = poolSizes;
            if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_nativeSkyDescPool) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeSkyResources: descriptor pool failed.\n");
                DestroyNativeSkyResources();
                m_nativeSkyFailed = true;
                return false;
            }

            VkDescriptorSetAllocateInfo dummyAlloc{};
            dummyAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dummyAlloc.descriptorPool     = m_nativeSkyDescPool;
            dummyAlloc.descriptorSetCount = 1;
            dummyAlloc.pSetLayouts        = &m_nativeSkyDescSetLayout;
            VkDescriptorSet dummySet = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(m_device, &dummyAlloc, &dummySet) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureNativeSkyResources: dummy descriptor set alloc failed.\n");
                DestroyNativeSkyResources();
                m_nativeSkyFailed = true;
                return false;
            }
            VkDescriptorImageInfo dummyImageInfo{};
            dummyImageInfo.imageView   = m_nativeSkyDummyCubeImageView;
            dummyImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkDescriptorImageInfo dummySamplerInfo{};
            dummySamplerInfo.sampler = m_nativeSkyCubeSampler;
            VkWriteDescriptorSet writes[2]{};
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
            vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);
            m_nativeSkyDescSets[0] = dummySet;
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(SkyPushConstants);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &m_nativeSkyDescSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_nativeSkyPipelineLayout) != VK_SUCCESS) {
            DestroyNativeSkyResources();
            m_nativeSkyFailed = true;
            return false;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
        const std::filesystem::path shaderPath =
            ShaderCompilationService::GetShaderSourceRoot() / L"Backend" / L"Native" / L"NativeSky.hlsl";
        if (!ShaderCompilationService::CompileShader(shaderPath, "VSMain", "vs_6_0", vsBlob, true) ||
            !ShaderCompilationService::CompileShader(shaderPath, "PSMain", "ps_6_0", psBlob, true)) {
            DebugLog("VulkanGraphicsDevice::EnsureNativeSkyResources: SPIR-V shader compile failed.\n");
            DestroyNativeSkyResources();
            m_nativeSkyFailed = true;
            return false;
        }

        VkShaderModule vsModule = VK_NULL_HANDLE;
        VkShaderModule psModule = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = vsBlob->GetBufferSize();
        moduleInfo.pCode = static_cast<const uint32_t*>(vsBlob->GetBufferPointer());
        if (vkCreateShaderModule(m_device, &moduleInfo, nullptr, &vsModule) != VK_SUCCESS) {
            DestroyNativeSkyResources();
            m_nativeSkyFailed = true;
            return false;
        }
        moduleInfo.codeSize = psBlob->GetBufferSize();
        moduleInfo.pCode = static_cast<const uint32_t*>(psBlob->GetBufferPointer());
        if (vkCreateShaderModule(m_device, &moduleInfo, nullptr, &psModule) != VK_SUCCESS) {
            vkDestroyShaderModule(m_device, vsModule, nullptr);
            DestroyNativeSkyResources();
            m_nativeSkyFailed = true;
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

        // No vertex input at all: the vertex shader builds the triangle from SV_VertexID.
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

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
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Depth off entirely. The sky is the background: it must not be rejected by depth the
        // meshes have not written yet, and must not write depth that would then reject them.
        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blendAttachment.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blendAttachment;

        const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(std::size(dynamicStates));
        dynamicState.pDynamicStates = dynamicStates;

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
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = m_nativeSkyPipelineLayout;
        pipelineInfo.renderPass = m_nativeMeshRenderPass;
        pipelineInfo.subpass = 0;

        const VkResult result =
            vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_nativeSkyPipeline);
        vkDestroyShaderModule(m_device, vsModule, nullptr);
        vkDestroyShaderModule(m_device, psModule, nullptr);
        if (result != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::EnsureNativeSkyResources: vkCreateGraphicsPipelines failed.\n");
            DestroyNativeSkyResources();
            m_nativeSkyFailed = true;
            return false;
        }
        return true;
    }

    VkDescriptorSet VulkanGraphicsDevice::GetOrCreateNativeSkyEnvironmentDescriptorSet(uint64_t environmentCubeSrv)
    {
        auto found = m_nativeSkyDescSets.find(environmentCubeSrv);
        if (found != m_nativeSkyDescSets.end()) {
            return found->second;
        }
        if (environmentCubeSrv == 0) {
            return m_nativeSkyDescSets.at(0);
        }

        auto srvIt = m_rhiImageViews.find(environmentCubeSrv);
        if (srvIt == m_rhiImageViews.end() || srvIt->second == VK_NULL_HANDLE) {
            DebugLog("VulkanGraphicsDevice::GetOrCreateNativeSkyEnvironmentDescriptorSet: unresolved "
                     "environmentCubeSrv handle; falling back to the procedural sky.\n");
            return m_nativeSkyDescSets.at(0);
        }

        VkDescriptorSetAllocateInfo texAlloc{};
        texAlloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        texAlloc.descriptorPool     = m_nativeSkyDescPool;
        texAlloc.descriptorSetCount = 1;
        texAlloc.pSetLayouts        = &m_nativeSkyDescSetLayout;
        VkDescriptorSet newSet = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(m_device, &texAlloc, &newSet) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::GetOrCreateNativeSkyEnvironmentDescriptorSet: descriptor "
                     "set allocation failed; falling back to the procedural sky.\n");
            return m_nativeSkyDescSets.at(0);
        }

        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageView   = srvIt->second;
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo smpInfo{};
        smpInfo.sampler = m_nativeSkyCubeSampler;

        VkWriteDescriptorSet writes[2]{};
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
        vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);

        m_nativeSkyDescSets[environmentCubeSrv] = newSet;
        return newSet;
    }

    void VulkanGraphicsDevice::RecordNativeSky(VkCommandBuffer cmd, const RhiBackendMeshFrameDesc& desc)
    {
        if (!EnsureNativeSkyResources()) {
            return;
        }

        const VkDescriptorSet descSet = GetOrCreateNativeSkyEnvironmentDescriptorSet(desc.environmentCubeSrv);
        // Only claim the environment cube is present when it actually resolved to a real view.
        // Falling back to the dummy set (key 0) must also fall the shader back to ComputeSkyColor(),
        // otherwise it would sample the 1x1 dummy cube's texels instead of the procedural sky.
        const bool hasEnvironmentCube =
            desc.environmentCubeSrv != 0 && descSet != m_nativeSkyDescSets.at(0);

        SkyPushConstants push{};
        std::memcpy(push.invViewProjection, desc.invViewProjection, sizeof(push.invViewProjection));
        push.cameraPos[0] = desc.cameraPos[0];
        push.cameraPos[1] = desc.cameraPos[1];
        push.cameraPos[2] = desc.cameraPos[2];
        push.sunDirection[0] = desc.sunDir[0];
        push.sunDirection[1] = desc.sunDir[1];
        push.sunDirection[2] = desc.sunDir[2];
        push.sunDirection[3] = desc.sunIntensity;
        push.sunColor[0] = desc.sunColor[0];
        push.sunColor[1] = desc.sunColor[1];
        push.sunColor[2] = desc.sunColor[2];
        push.sunColor[3] = desc.exposure; // RenderSettings::exposure
        push.hasEnvironmentCube = hasEnvironmentCube ? 1.0f : 0.0f;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_nativeSkyPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_nativeSkyPipelineLayout,
                                0, 1, &descSet, 0, nullptr);
        vkCmdPushConstants(cmd, m_nativeSkyPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
}

#endif // RHI_VULKAN
