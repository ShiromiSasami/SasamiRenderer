// VulkanGraphicsDevice_Backend.cpp
// Vulkan backend helpers, Initialize, Getters.
#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"

#if RHI_VULKAN

#include "Foundation/Tools/DebugOutput.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>


namespace SasamiRenderer
{
    namespace
    {
        bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
        {
            return std::any_of(extensions.begin(), extensions.end(),
                               [name](const VkExtensionProperties& extension) {
                                   return std::strcmp(extension.extensionName, name) == 0;
                               });
        }

        std::vector<VkExtensionProperties> EnumerateDeviceExtensions(VkPhysicalDevice physicalDevice)
        {
            uint32_t extensionCount = 0;
            vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
            std::vector<VkExtensionProperties> extensions(extensionCount);
            if (extensionCount > 0) {
                vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, extensions.data());
            }
            return extensions;
        }

        VkFormat ToVkFormat(RhiFormat format)
        {
            switch (format) {
            case RhiFormat::R8UNorm: return VK_FORMAT_R8_UNORM;
            case RhiFormat::R8G8B8A8UNorm: return VK_FORMAT_R8G8B8A8_UNORM;
            case RhiFormat::B8G8R8A8UNorm: return VK_FORMAT_B8G8R8A8_UNORM;
            case RhiFormat::R16G16B16A16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case RhiFormat::R32G32B32Float: return VK_FORMAT_R32G32B32_SFLOAT;
            case RhiFormat::R32G32Float: return VK_FORMAT_R32G32_SFLOAT;
            case RhiFormat::R16Float: return VK_FORMAT_R16_SFLOAT;
            case RhiFormat::R16Typeless: return VK_FORMAT_D16_UNORM;
            case RhiFormat::R16UNorm: return VK_FORMAT_R16_UNORM;
            case RhiFormat::R32Float: return VK_FORMAT_R32_SFLOAT;
            case RhiFormat::R32UInt: return VK_FORMAT_R32_UINT;
            case RhiFormat::R32Typeless: return VK_FORMAT_D32_SFLOAT;
            case RhiFormat::D16UNorm: return VK_FORMAT_D16_UNORM;
            case RhiFormat::D32Float: return VK_FORMAT_D32_SFLOAT;
            case RhiFormat::D24UNormS8UInt: return VK_FORMAT_D24_UNORM_S8_UINT;
            default: return VK_FORMAT_UNDEFINED;
            }
        }

        VkImageUsageFlags ToVkImageUsage(RhiTextureUsageFlags usage)
        {
            VkImageUsageFlags flags = 0;
            if (HasFlag(usage, RhiTextureUsageFlags::ShaderResource)) {
                flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
            }
            if (HasFlag(usage, RhiTextureUsageFlags::RenderTarget)) {
                flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            }
            if (HasFlag(usage, RhiTextureUsageFlags::DepthStencil)) {
                flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            }
            if (HasFlag(usage, RhiTextureUsageFlags::UnorderedAccess)) {
                flags |= VK_IMAGE_USAGE_STORAGE_BIT;
            }
            if (HasFlag(usage, RhiTextureUsageFlags::CopySource)) {
                flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            }
            if (HasFlag(usage, RhiTextureUsageFlags::CopyDest)) {
                flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            }
            return flags;
        }

        VkBufferUsageFlags ToVkBufferUsage(RhiBufferUsageFlags usage)
        {
            VkBufferUsageFlags flags = 0;
            if (HasFlag(usage, RhiBufferUsageFlags::Vertex)) {
                flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            }
            if (HasFlag(usage, RhiBufferUsageFlags::Index)) {
                flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            }
            if (HasFlag(usage, RhiBufferUsageFlags::Constant)) {
                flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            }
            if (HasFlag(usage, RhiBufferUsageFlags::ShaderResource) ||
                HasFlag(usage, RhiBufferUsageFlags::Structured)) {
                flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            }
            if (HasFlag(usage, RhiBufferUsageFlags::UnorderedAccess)) {
                flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            }
            if (HasFlag(usage, RhiBufferUsageFlags::CopySource)) {
                flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            }
            if (HasFlag(usage, RhiBufferUsageFlags::CopyDest)) {
                flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            }
            if (HasFlag(usage, RhiBufferUsageFlags::AccelerationStructure)) {
#ifdef VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
                flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
#endif
            }
            return flags;
        }

        VkMemoryPropertyFlags ToVkMemoryProperties(RhiMemoryUsage usage)
        {
            switch (usage) {
            case RhiMemoryUsage::CpuToGpu:
                return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            case RhiMemoryUsage::GpuToCpu:
                return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            case RhiMemoryUsage::GpuOnly:
            default:
                return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            }
        }

        VkImageViewType ToVkImageViewType(RhiTextureViewDimension dimension)
        {
            switch (dimension) {
            case RhiTextureViewDimension::Texture1D: return VK_IMAGE_VIEW_TYPE_1D;
            case RhiTextureViewDimension::Texture1DArray: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
            case RhiTextureViewDimension::Texture2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            case RhiTextureViewDimension::Texture3D: return VK_IMAGE_VIEW_TYPE_3D;
            case RhiTextureViewDimension::TextureCube: return VK_IMAGE_VIEW_TYPE_CUBE;
            case RhiTextureViewDimension::TextureCubeArray: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
            case RhiTextureViewDimension::Texture2D:
            default: return VK_IMAGE_VIEW_TYPE_2D;
            }
        }

        VkImageAspectFlags ToVkAspectMask(RhiFormat format)
        {
            switch (format) {
            case RhiFormat::R16Typeless:
            case RhiFormat::R32Typeless:
            case RhiFormat::D16UNorm:
            case RhiFormat::D32Float:
                return VK_IMAGE_ASPECT_DEPTH_BIT;
            case RhiFormat::D24UNormS8UInt:
                return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            default:
                return VK_IMAGE_ASPECT_COLOR_BIT;
            }
        }

        VkShaderStageFlagBits ToVkShaderStage(RhiShaderStageFlags stage)
        {
            switch (stage) {
            case RhiShaderStageFlags::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
            case RhiShaderStageFlags::Hull: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
            case RhiShaderStageFlags::Domain: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
            case RhiShaderStageFlags::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
            case RhiShaderStageFlags::Pixel: return VK_SHADER_STAGE_FRAGMENT_BIT;
            case RhiShaderStageFlags::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
            default: return VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
            }
        }

        VkShaderStageFlags ToVkShaderStages(RhiShaderStageFlags stages)
        {
            const uint32_t flags = static_cast<uint32_t>(stages);
            VkShaderStageFlags result = 0;
            if (flags & static_cast<uint32_t>(RhiShaderStageFlags::Vertex)) result |= VK_SHADER_STAGE_VERTEX_BIT;
            if (flags & static_cast<uint32_t>(RhiShaderStageFlags::Hull)) result |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
            if (flags & static_cast<uint32_t>(RhiShaderStageFlags::Domain)) result |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
            if (flags & static_cast<uint32_t>(RhiShaderStageFlags::Geometry)) result |= VK_SHADER_STAGE_GEOMETRY_BIT;
            if (flags & static_cast<uint32_t>(RhiShaderStageFlags::Pixel)) result |= VK_SHADER_STAGE_FRAGMENT_BIT;
            if (flags & static_cast<uint32_t>(RhiShaderStageFlags::Compute)) result |= VK_SHADER_STAGE_COMPUTE_BIT;
            return result != 0 ? result : VK_SHADER_STAGE_ALL;
        }

        VkPrimitiveTopology ToVkTopology(RhiPrimitiveTopology topology)
        {
            switch (topology) {
            case RhiPrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            case RhiPrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            case RhiPrimitiveTopology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            case RhiPrimitiveTopology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            case RhiPrimitiveTopology::PatchList: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
            case RhiPrimitiveTopology::TriangleList:
            default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            }
        }

        VkCullModeFlags ToVkCullMode(RhiCullMode mode)
        {
            switch (mode) {
            case RhiCullMode::None: return VK_CULL_MODE_NONE;
            case RhiCullMode::Front: return VK_CULL_MODE_FRONT_BIT;
            case RhiCullMode::Back:
            default: return VK_CULL_MODE_BACK_BIT;
            }
        }

        VkPolygonMode ToVkPolygonMode(RhiFillMode mode)
        {
            return mode == RhiFillMode::Wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        }

        VkCompareOp ToVkCompareOp(RhiCompareOp op)
        {
            switch (op) {
            case RhiCompareOp::Never: return VK_COMPARE_OP_NEVER;
            case RhiCompareOp::Less: return VK_COMPARE_OP_LESS;
            case RhiCompareOp::Equal: return VK_COMPARE_OP_EQUAL;
            case RhiCompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
            case RhiCompareOp::Greater: return VK_COMPARE_OP_GREATER;
            case RhiCompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
            case RhiCompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
            case RhiCompareOp::Always:
            default: return VK_COMPARE_OP_ALWAYS;
            }
        }

        VkImageLayout ToVkImageLayout(RhiResourceState state)
        {
            switch (state) {
            case RhiResourceState::RenderTarget: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            case RhiResourceState::DepthWrite: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            case RhiResourceState::DepthRead: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            case RhiResourceState::ShaderResource: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            case RhiResourceState::UnorderedAccess: return VK_IMAGE_LAYOUT_GENERAL;
            case RhiResourceState::CopySource: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            case RhiResourceState::CopyDest: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            case RhiResourceState::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            case RhiResourceState::Common:
            default: return VK_IMAGE_LAYOUT_GENERAL;
            }
        }

        VkAccessFlags ToVkAccessFlags(RhiResourceState state)
        {
            switch (state) {
            case RhiResourceState::RenderTarget: return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            case RhiResourceState::DepthWrite: return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            case RhiResourceState::DepthRead: return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            case RhiResourceState::ShaderResource: return VK_ACCESS_SHADER_READ_BIT;
            case RhiResourceState::UnorderedAccess: return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            case RhiResourceState::CopySource: return VK_ACCESS_TRANSFER_READ_BIT;
            case RhiResourceState::CopyDest: return VK_ACCESS_TRANSFER_WRITE_BIT;
            case RhiResourceState::Present: return VK_ACCESS_MEMORY_READ_BIT;
            case RhiResourceState::Common:
            default: return 0;
            }
        }

        VkPipelineStageFlags ToVkPipelineStage(RhiResourceState state)
        {
            switch (state) {
            case RhiResourceState::RenderTarget: return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            case RhiResourceState::DepthWrite:
            case RhiResourceState::DepthRead:
                return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            case RhiResourceState::ShaderResource:
            case RhiResourceState::UnorderedAccess:
                return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            case RhiResourceState::CopySource:
            case RhiResourceState::CopyDest:
                return VK_PIPELINE_STAGE_TRANSFER_BIT;
            case RhiResourceState::Present:
                return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            case RhiResourceState::Common:
            default:
                return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            }
        }
    }

    class VulkanRhiCommandEncoder final : public IRhiCommandEncoder
    {
    public:
        VulkanRhiCommandEncoder(VulkanGraphicsDevice& device, RhiQueueType queueType)
            : m_device(device)
            , m_queueType(queueType)
        {
            if (m_device.m_device == VK_NULL_HANDLE || m_device.m_commandPool == VK_NULL_HANDLE) {
                return;
            }

            VkCommandBufferAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocateInfo.commandPool = m_device.m_commandPool;
            allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocateInfo.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(m_device.m_device, &allocateInfo, &m_commandBuffer) != VK_SUCCESS) {
                m_commandBuffer = VK_NULL_HANDLE;
                return;
            }

            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            m_recording = vkBeginCommandBuffer(m_commandBuffer, &beginInfo) == VK_SUCCESS;
        }

        ~VulkanRhiCommandEncoder() override
        {
            if (m_device.m_device != VK_NULL_HANDLE && m_device.m_rhiDescriptorPool != VK_NULL_HANDLE && !m_descriptorSets.empty()) {
                vkFreeDescriptorSets(m_device.m_device,
                                     m_device.m_rhiDescriptorPool,
                                     static_cast<uint32_t>(m_descriptorSets.size()),
                                     m_descriptorSets.data());
            }
            for (VkFramebuffer framebuffer : m_temporaryFramebuffers) {
                vkDestroyFramebuffer(m_device.m_device, framebuffer, nullptr);
            }
            for (VkRenderPass renderPass : m_temporaryRenderPasses) {
                vkDestroyRenderPass(m_device.m_device, renderPass, nullptr);
            }
            for (VkImageView imageView : m_temporaryImageViews) {
                vkDestroyImageView(m_device.m_device, imageView, nullptr);
            }
            if (m_device.m_device != VK_NULL_HANDLE && m_device.m_commandPool != VK_NULL_HANDLE && m_commandBuffer != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(m_device.m_device, m_device.m_commandPool, 1, &m_commandBuffer);
            }
        }

        bool IsValid() const { return m_commandBuffer != VK_NULL_HANDLE && m_recording; }
        RhiQueueType QueueType() const { return m_queueType; }
        VkCommandBuffer CommandBuffer() const { return m_commandBuffer; }

        bool Finish()
        {
            if (!m_recording || m_commandBuffer == VK_NULL_HANDLE) {
                return false;
            }
            if (m_renderPassActive) {
                vkCmdEndRenderPass(m_commandBuffer);
                m_renderPassActive = false;
            }
            m_recording = false;
            return vkEndCommandBuffer(m_commandBuffer) == VK_SUCCESS;
        }

        void TransitionResources(const RhiResourceTransitionDesc* transitions, uint32_t count) override
        {
            if (!IsValid() || !transitions || count == 0) {
                return;
            }

            std::vector<VkImageMemoryBarrier> imageBarriers;
            std::vector<VkBufferMemoryBarrier> bufferBarriers;
            imageBarriers.reserve(count);
            bufferBarriers.reserve(count);
            VkPipelineStageFlags srcStages = 0;
            VkPipelineStageFlags dstStages = 0;
            for (uint32_t i = 0; i < count; ++i) {
                const auto resourceIt = m_device.m_rhiResources.find(transitions[i].resource.id);
                if (resourceIt == m_device.m_rhiResources.end()) {
                    continue;
                }

                const auto& resource = resourceIt->second;
                srcStages |= ToVkPipelineStage(transitions[i].before);
                dstStages |= ToVkPipelineStage(transitions[i].after);
                if (resource.buffer != VK_NULL_HANDLE) {
                    VkBufferMemoryBarrier barrier{};
                    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                    barrier.srcAccessMask = ToVkAccessFlags(transitions[i].before);
                    barrier.dstAccessMask = ToVkAccessFlags(transitions[i].after);
                    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.buffer = resource.buffer;
                    barrier.offset = 0;
                    barrier.size = resource.sizeInBytes;
                    bufferBarriers.push_back(barrier);
                    continue;
                }
                if (resource.image == VK_NULL_HANDLE) {
                    continue;
                }

                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.srcAccessMask = ToVkAccessFlags(transitions[i].before);
                barrier.dstAccessMask = ToVkAccessFlags(transitions[i].after);
                barrier.oldLayout = ToVkImageLayout(transitions[i].before);
                barrier.newLayout = ToVkImageLayout(transitions[i].after);
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = resource.image;
                barrier.subresourceRange.aspectMask = ToVkAspectMask(resource.format);
                barrier.subresourceRange.baseMipLevel = 0;
                barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
                imageBarriers.push_back(barrier);
            }

            if (!imageBarriers.empty() || !bufferBarriers.empty()) {
                vkCmdPipelineBarrier(m_commandBuffer,
                                     srcStages ? srcStages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     dstStages ? dstStages : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                     0,
                                     0,
                                     nullptr,
                                     static_cast<uint32_t>(bufferBarriers.size()),
                                     bufferBarriers.empty() ? nullptr : bufferBarriers.data(),
                                     static_cast<uint32_t>(imageBarriers.size()),
                                     imageBarriers.empty() ? nullptr : imageBarriers.data());
            }
        }

        void BeginRenderPass(const RhiRenderPassDesc& desc) override
        {
            if (!IsValid() || m_renderPassActive || desc.colorAttachmentCount > 8 ||
                (desc.colorAttachmentCount > 0 && !desc.colorAttachments)) {
                return;
            }

            std::vector<VkAttachmentDescription> attachments;
            std::vector<VkAttachmentReference> colorReferences;
            std::vector<VkImageView> imageViews;
            std::vector<VkClearValue> clearValues;
            attachments.reserve(desc.colorAttachmentCount + 1);
            colorReferences.reserve(desc.colorAttachmentCount);
            imageViews.reserve(desc.colorAttachmentCount + 1);
            clearValues.reserve(desc.colorAttachmentCount + 1);
            uint32_t width = UINT32_MAX;
            uint32_t height = UINT32_MAX;

            auto addAttachment = [&](const RhiAttachmentDesc& source, bool depth) -> bool {
                const auto resourceIt = m_device.m_rhiResources.find(source.texture.id);
                if (resourceIt == m_device.m_rhiResources.end() || resourceIt->second.image == VK_NULL_HANDLE) {
                    return false;
                }
                const auto& resource = resourceIt->second;
                const RhiFormat format = source.format != RhiFormat::Unknown ? source.format : resource.format;
                const VkFormat vkFormat = ToVkFormat(format);
                if (vkFormat == VK_FORMAT_UNDEFINED) {
                    return false;
                }

                VkImageViewCreateInfo viewInfo{};
                viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                viewInfo.image = resource.image;
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                viewInfo.format = vkFormat;
                viewInfo.subresourceRange.aspectMask = depth ? ToVkAspectMask(format) : VK_IMAGE_ASPECT_COLOR_BIT;
                viewInfo.subresourceRange.baseMipLevel = 0;
                viewInfo.subresourceRange.levelCount = 1;
                viewInfo.subresourceRange.baseArrayLayer = 0;
                viewInfo.subresourceRange.layerCount = 1;
                VkImageView imageView = VK_NULL_HANDLE;
                if (vkCreateImageView(m_device.m_device, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
                    return false;
                }
                imageViews.push_back(imageView);

                auto loadOp = [](RhiLoadOp op) {
                    switch (op) {
                    case RhiLoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
                    case RhiLoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                    case RhiLoadOp::Load:
                    default: return VK_ATTACHMENT_LOAD_OP_LOAD;
                    }
                };
                VkAttachmentDescription attachment{};
                attachment.format = vkFormat;
                attachment.samples = VK_SAMPLE_COUNT_1_BIT;
                attachment.loadOp = loadOp(source.loadOp);
                attachment.storeOp = source.storeOp == RhiStoreOp::Store
                    ? VK_ATTACHMENT_STORE_OP_STORE
                    : VK_ATTACHMENT_STORE_OP_DONT_CARE;
                attachment.stencilLoadOp = depth ? loadOp(source.loadOp) : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                attachment.stencilStoreOp = depth && source.storeOp == RhiStoreOp::Store
                    ? VK_ATTACHMENT_STORE_OP_STORE
                    : VK_ATTACHMENT_STORE_OP_DONT_CARE;
                attachment.initialLayout = ToVkImageLayout(source.initialState);
                attachment.finalLayout = ToVkImageLayout(source.finalState);
                attachments.push_back(attachment);

                VkClearValue clear{};
                if (depth) {
                    clear.depthStencil.depth = source.clearDepthStencil.depth;
                    clear.depthStencil.stencil = source.clearDepthStencil.stencil;
                } else {
                    clear.color.float32[0] = source.clearColor.r;
                    clear.color.float32[1] = source.clearColor.g;
                    clear.color.float32[2] = source.clearColor.b;
                    clear.color.float32[3] = source.clearColor.a;
                }
                clearValues.push_back(clear);
                width = (std::min)(width, resource.extent.width);
                height = (std::min)(height, resource.extent.height);
                return true;
            };

            bool valid = true;
            for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i) {
                valid = addAttachment(desc.colorAttachments[i], false) && valid;
                VkAttachmentReference reference{};
                reference.attachment = i;
                reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colorReferences.push_back(reference);
            }

            VkAttachmentReference depthReference{};
            const bool hasDepth = desc.depthStencilAttachment != nullptr;
            if (hasDepth) {
                depthReference.attachment = static_cast<uint32_t>(attachments.size());
                depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                valid = addAttachment(*desc.depthStencilAttachment, true) && valid;
            }
            if (!valid || attachments.empty() || width == 0 || height == 0 || width == UINT32_MAX || height == UINT32_MAX) {
                for (VkImageView imageView : imageViews) {
                    vkDestroyImageView(m_device.m_device, imageView, nullptr);
                }
                return;
            }

            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = static_cast<uint32_t>(colorReferences.size());
            subpass.pColorAttachments = colorReferences.empty() ? nullptr : colorReferences.data();
            subpass.pDepthStencilAttachment = hasDepth ? &depthReference : nullptr;

            VkRenderPassCreateInfo renderPassInfo{};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
            renderPassInfo.pAttachments = attachments.data();
            renderPassInfo.subpassCount = 1;
            renderPassInfo.pSubpasses = &subpass;
            VkRenderPass renderPass = VK_NULL_HANDLE;
            if (vkCreateRenderPass(m_device.m_device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
                for (VkImageView imageView : imageViews) {
                    vkDestroyImageView(m_device.m_device, imageView, nullptr);
                }
                return;
            }

            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass;
            framebufferInfo.attachmentCount = static_cast<uint32_t>(imageViews.size());
            framebufferInfo.pAttachments = imageViews.data();
            framebufferInfo.width = width;
            framebufferInfo.height = height;
            framebufferInfo.layers = 1;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            if (vkCreateFramebuffer(m_device.m_device, &framebufferInfo, nullptr, &framebuffer) != VK_SUCCESS) {
                vkDestroyRenderPass(m_device.m_device, renderPass, nullptr);
                for (VkImageView imageView : imageViews) {
                    vkDestroyImageView(m_device.m_device, imageView, nullptr);
                }
                return;
            }

            VkRenderPassBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            beginInfo.renderPass = renderPass;
            beginInfo.framebuffer = framebuffer;
            beginInfo.renderArea.extent = { width, height };
            beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
            beginInfo.pClearValues = clearValues.data();
            vkCmdBeginRenderPass(m_commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
            m_temporaryImageViews.insert(m_temporaryImageViews.end(), imageViews.begin(), imageViews.end());
            m_temporaryRenderPasses.push_back(renderPass);
            m_temporaryFramebuffers.push_back(framebuffer);
            m_renderPassActive = true;
        }

        void EndRenderPass() override
        {
            if (IsValid() && m_renderPassActive) {
                vkCmdEndRenderPass(m_commandBuffer);
                m_renderPassActive = false;
            }
        }

        void SetGraphicsPipeline(RhiPipelineHandle pipelineHandle) override
        {
            const auto it = m_device.m_rhiPipelines.find(pipelineHandle.id);
            if (IsValid() && it != m_device.m_rhiPipelines.end() && it->second.pipeline != VK_NULL_HANDLE) {
                vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, it->second.pipeline);
                if (it->second.layout.IsValid()) {
                    SetGraphicsPipelineLayout(it->second.layout);
                }
            }
        }

        void SetComputePipeline(RhiPipelineHandle pipelineHandle) override
        {
            const auto it = m_device.m_rhiPipelines.find(pipelineHandle.id);
            if (IsValid() && it != m_device.m_rhiPipelines.end() && it->second.pipeline != VK_NULL_HANDLE) {
                vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, it->second.pipeline);
                if (it->second.layout.IsValid()) {
                    SetComputePipelineLayout(it->second.layout);
                }
            }
        }

        void SetGraphicsPipelineLayout(RhiPipelineLayoutHandle layout) override
        {
            if (m_graphicsDescriptors.layout.id != layout.id) {
                m_graphicsDescriptors.layout = layout;
                m_graphicsDescriptors.tables.clear();
                m_graphicsDescriptors.dirty = true;
            }
        }

        void SetComputePipelineLayout(RhiPipelineLayoutHandle layout) override
        {
            if (m_computeDescriptors.layout.id != layout.id) {
                m_computeDescriptors.layout = layout;
                m_computeDescriptors.tables.clear();
                m_computeDescriptors.dirty = true;
            }
        }

        void SetGraphicsDescriptorTable(uint32_t slot, RhiGpuDescriptorHandle table) override
        {
            if (table.IsValid()) {
                m_graphicsDescriptors.tables[slot] = table.ptr;
                m_graphicsDescriptors.dirty = true;
            }
        }

        void SetComputeDescriptorTable(uint32_t slot, RhiGpuDescriptorHandle table) override
        {
            if (table.IsValid()) {
                m_computeDescriptors.tables[slot] = table.ptr;
                m_computeDescriptors.dirty = true;
            }
        }

        void SetPrimitiveTopology(RhiPrimitiveTopology) override
        {
            // Vulkan pipeline topology is baked into VkPipeline in this backend.
        }

        void SetViewports(const RhiViewport* viewports, uint32_t count) override
        {
            if (!IsValid() || !viewports || count == 0) {
                return;
            }
            std::vector<VkViewport> vkViewports(count);
            for (uint32_t i = 0; i < count; ++i) {
                vkViewports[i].x = viewports[i].x;
                vkViewports[i].y = viewports[i].y;
                vkViewports[i].width = viewports[i].width;
                vkViewports[i].height = viewports[i].height;
                vkViewports[i].minDepth = viewports[i].minDepth;
                vkViewports[i].maxDepth = viewports[i].maxDepth;
            }
            vkCmdSetViewport(m_commandBuffer, 0, count, vkViewports.data());
        }

        void SetScissors(const RhiRect* scissors, uint32_t count) override
        {
            if (!IsValid() || !scissors || count == 0) {
                return;
            }
            std::vector<VkRect2D> vkScissors(count);
            for (uint32_t i = 0; i < count; ++i) {
                vkScissors[i].offset = { scissors[i].left, scissors[i].top };
                vkScissors[i].extent = {
                    static_cast<uint32_t>(scissors[i].right - scissors[i].left),
                    static_cast<uint32_t>(scissors[i].bottom - scissors[i].top),
                };
            }
            vkCmdSetScissor(m_commandBuffer, 0, count, vkScissors.data());
        }

        void Draw(const RhiDrawDesc& draw) override
        {
            if (IsValid() && BindDescriptors(m_graphicsDescriptors, VK_PIPELINE_BIND_POINT_GRAPHICS)) {
                vkCmdDraw(m_commandBuffer,
                          draw.vertexCount,
                          draw.instanceCount,
                          draw.startVertex,
                          draw.startInstance);
            }
        }

        void DrawIndexed(const RhiDrawIndexedDesc& draw) override
        {
            if (IsValid() && BindDescriptors(m_graphicsDescriptors, VK_PIPELINE_BIND_POINT_GRAPHICS)) {
                vkCmdDrawIndexed(m_commandBuffer,
                                 draw.indexCount,
                                 draw.instanceCount,
                                 draw.startIndex,
                                 draw.baseVertex,
                                 draw.startInstance);
            }
        }

        void Dispatch(const RhiDispatchDesc& dispatch) override
        {
            if (IsValid() && BindDescriptors(m_computeDescriptors, VK_PIPELINE_BIND_POINT_COMPUTE)) {
                vkCmdDispatch(m_commandBuffer, dispatch.groupCountX, dispatch.groupCountY, dispatch.groupCountZ);
            }
        }

        void SetVertexBuffers(uint32_t startSlot, uint32_t count, const RhiVertexBufferView* views) override
        {
            if (!IsValid() || !views || count == 0) {
                return;
            }

            std::vector<VkBuffer> buffers(count, VK_NULL_HANDLE);
            std::vector<VkDeviceSize> offsets(count, 0);
            for (uint32_t i = 0; i < count; ++i) {
                const auto it = m_device.m_rhiResources.find(views[i].gpuAddress);
                if (it != m_device.m_rhiResources.end()) {
                    buffers[i] = it->second.buffer;
                }
            }
            vkCmdBindVertexBuffers(m_commandBuffer, startSlot, count, buffers.data(), offsets.data());
        }

        void SetIndexBuffer(const RhiIndexBufferView& view) override
        {
            if (!IsValid() || view.gpuAddress == 0) {
                return;
            }
            const auto it = m_device.m_rhiResources.find(view.gpuAddress);
            if (it != m_device.m_rhiResources.end() && it->second.buffer != VK_NULL_HANDLE) {
                vkCmdBindIndexBuffer(m_commandBuffer,
                                     it->second.buffer,
                                     0,
                                     view.is32Bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
            }
        }

        void SetVertexBufferBindings(uint32_t startSlot, uint32_t count, const RhiVertexBufferBinding* bindings) override
        {
            if (!IsValid() || !bindings || count == 0) {
                return;
            }

            std::vector<VkBuffer> buffers(count, VK_NULL_HANDLE);
            std::vector<VkDeviceSize> offsets(count, 0);
            for (uint32_t i = 0; i < count; ++i) {
                const auto it = m_device.m_rhiResources.find(bindings[i].buffer.id);
                if (it != m_device.m_rhiResources.end()) {
                    buffers[i] = it->second.buffer;
                    offsets[i] = bindings[i].offsetInBytes;
                }
            }
            vkCmdBindVertexBuffers(m_commandBuffer, startSlot, count, buffers.data(), offsets.data());
        }

        void SetIndexBufferBinding(const RhiIndexBufferBinding& binding) override
        {
            if (!IsValid() || !binding.buffer.IsValid()) {
                return;
            }
            const auto it = m_device.m_rhiResources.find(binding.buffer.id);
            if (it != m_device.m_rhiResources.end() && it->second.buffer != VK_NULL_HANDLE) {
                vkCmdBindIndexBuffer(m_commandBuffer,
                                     it->second.buffer,
                                     binding.offsetInBytes,
                                     binding.is32Bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
            }
        }

        void CopyBuffer(const RhiBufferCopyDesc& copy) override
        {
            if (!IsValid() || copy.sizeInBytes == 0) {
                return;
            }
            const auto sourceIt = m_device.m_rhiResources.find(copy.source.id);
            const auto destinationIt = m_device.m_rhiResources.find(copy.destination.id);
            if (sourceIt == m_device.m_rhiResources.end() || destinationIt == m_device.m_rhiResources.end() ||
                sourceIt->second.buffer == VK_NULL_HANDLE || destinationIt->second.buffer == VK_NULL_HANDLE ||
                copy.sourceOffsetInBytes >= sourceIt->second.sizeInBytes ||
                copy.sizeInBytes > sourceIt->second.sizeInBytes - copy.sourceOffsetInBytes ||
                copy.destinationOffsetInBytes >= destinationIt->second.sizeInBytes ||
                copy.sizeInBytes > destinationIt->second.sizeInBytes - copy.destinationOffsetInBytes) {
                return;
            }
            VkBufferCopy region{};
            region.srcOffset = copy.sourceOffsetInBytes;
            region.dstOffset = copy.destinationOffsetInBytes;
            region.size = copy.sizeInBytes;
            vkCmdCopyBuffer(m_commandBuffer,
                            sourceIt->second.buffer,
                            destinationIt->second.buffer,
                            1,
                            &region);
        }

    private:
        struct DescriptorState
        {
            RhiPipelineLayoutHandle layout{};
            std::unordered_map<uint32_t, uint64_t> tables;
            bool dirty = true;
        };

        bool BindDescriptors(DescriptorState& state, VkPipelineBindPoint bindPoint)
        {
            if (!state.layout.IsValid()) {
                return true;
            }
            const auto layoutIt = m_device.m_rhiPipelineLayouts.find(state.layout.id);
            if (layoutIt == m_device.m_rhiPipelineLayouts.end()) {
                return false;
            }
            const VulkanGraphicsDevice::VulkanRhiPipelineLayout& layout = layoutIt->second;
            if (layout.descriptorSetLayout == VK_NULL_HANDLE || !state.dirty) {
                return true;
            }
            if (!m_device.EnsureRhiDescriptorPool()) {
                return false;
            }

            VkDescriptorSetAllocateInfo allocateInfo{};
            allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocateInfo.descriptorPool = m_device.m_rhiDescriptorPool;
            allocateInfo.descriptorSetCount = 1;
            allocateInfo.pSetLayouts = &layout.descriptorSetLayout;
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(m_device.m_device, &allocateInfo, &descriptorSet) != VK_SUCCESS) {
                return false;
            }

            size_t descriptorCount = 0;
            for (const auto& table : state.tables) {
                if (table.first < layout.bindings.size()) {
                    descriptorCount += layout.bindings[table.first].descriptorCount;
                }
            }
            std::vector<VkWriteDescriptorSet> writes;
            std::vector<VkDescriptorImageInfo> imageInfos;
            std::vector<VkDescriptorBufferInfo> bufferInfos;
            // AS write infos and handles must stay alive until vkUpdateDescriptorSets.
            // Reserve up front to prevent reallocation (which would invalidate pNext pointers).
            std::vector<VkWriteDescriptorSetAccelerationStructureKHR> asWriteInfos;
            std::vector<VkAccelerationStructureKHR> asHandles;
            writes.reserve(state.tables.size());
            imageInfos.reserve(descriptorCount);
            bufferInfos.reserve(descriptorCount);
            asWriteInfos.reserve(descriptorCount);
            asHandles.reserve(descriptorCount);

            for (const auto& table : state.tables) {
                if (table.first >= layout.bindings.size()) {
                    continue;
                }
                const auto& binding = layout.bindings[table.first];
                if (!binding.valid || binding.descriptorCount == 0) {
                    continue;
                }

                const size_t imageStart = imageInfos.size();
                const size_t bufferStart = bufferInfos.size();
                uint32_t validCount = 0;
                for (; validCount < binding.descriptorCount; ++validCount) {
                    const auto descriptorIt = m_device.m_rhiDescriptors.find(table.second + validCount);
                    if (descriptorIt == m_device.m_rhiDescriptors.end() ||
                        descriptorIt->second.type != binding.descriptorType) {
                        break;
                    }
                    const auto& descriptor = descriptorIt->second;
                    if (descriptor.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                        descriptor.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                        VkDescriptorImageInfo imageInfo{};
                        imageInfo.imageView = descriptor.imageView;
                        imageInfo.imageLayout = descriptor.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                            ? VK_IMAGE_LAYOUT_GENERAL
                            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        imageInfos.push_back(imageInfo);
                    } else if (descriptor.type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
                        // AS descriptors are handled with a pNext write; break after one.
                        asHandles.push_back(descriptor.accelerationStructure);
                        validCount = 1; // force single-descriptor write
                        break;
                    } else {
                        VkDescriptorBufferInfo bufferInfo{};
                        bufferInfo.buffer = descriptor.buffer;
                        bufferInfo.offset = descriptor.offset;
                        bufferInfo.range = descriptor.range;
                        bufferInfos.push_back(bufferInfo);
                    }
                }
                if (validCount == 0) {
                    continue;
                }

                VkWriteDescriptorSet write{};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = descriptorSet;
                write.dstBinding = binding.binding;
                write.descriptorCount = validCount;
                write.descriptorType = binding.descriptorType;
                if (binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                    binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                    write.pImageInfo = imageInfos.data() + imageStart;
                } else if (binding.descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
                    // pNext carries the AS write; pImageInfo/pBufferInfo are null.
                    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
                    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
                    asWrite.accelerationStructureCount = 1;
                    asWrite.pAccelerationStructures = &asHandles.back();
                    asWriteInfos.push_back(asWrite);
                    write.pNext = &asWriteInfos.back();
                } else {
                    write.pBufferInfo = bufferInfos.data() + bufferStart;
                }
                writes.push_back(write);
            }

            if (!writes.empty()) {
                vkUpdateDescriptorSets(m_device.m_device,
                                       static_cast<uint32_t>(writes.size()),
                                       writes.data(),
                                       0,
                                       nullptr);
            }
            vkCmdBindDescriptorSets(m_commandBuffer,
                                    bindPoint,
                                    layout.pipelineLayout,
                                    0,
                                    1,
                                    &descriptorSet,
                                    0,
                                    nullptr);
            m_descriptorSets.push_back(descriptorSet);
            state.dirty = false;
            return true;
        }

        VulkanGraphicsDevice& m_device;
        RhiQueueType m_queueType = RhiQueueType::Graphics;
        VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
        bool m_recording = false;
        bool m_renderPassActive = false;
        DescriptorState m_graphicsDescriptors;
        DescriptorState m_computeDescriptors;
        std::vector<VkDescriptorSet> m_descriptorSets;
        std::vector<VkImageView> m_temporaryImageViews;
        std::vector<VkRenderPass> m_temporaryRenderPasses;
        std::vector<VkFramebuffer> m_temporaryFramebuffers;
    };

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

    VulkanGraphicsDevice::~VulkanGraphicsDevice()
    {
        Cleanup();
    }

    bool VulkanGraphicsDevice::Initialize(HWND hWnd, UINT width, UINT height, UINT bufferCount)
    {
        Cleanup();
        if (!CreateInstance() ||
            !CreateSurface(hWnd) ||
            !PickPhysicalDevice() ||
            !CreateDevice() ||
            !CreateSwapChain(width, height, bufferCount) ||
            !CreateFrameResources(bufferCount)) {
            Cleanup();
            return false;
        }

        QueryCapabilities();
        DebugLog("VulkanGraphicsDevice::Initialize: Vulkan backend initialized. Full render-pass parity still requires RHI resource/pipeline migration.\n");
        return true;
    }

    GraphicsRuntime VulkanGraphicsDevice::GetBackend() const
    {
        return GraphicsRuntime::Vulkan;
    }

    void* VulkanGraphicsDevice::GetNativeDeviceHandle() const
    {
        return m_device;
    }

    void* VulkanGraphicsDevice::GetNativeGraphicsQueueHandle() const
    {
        return m_graphicsQueue;
    }

    ID3D12Device* VulkanGraphicsDevice::GetDevice() const
    {
        return nullptr;
    }

    ID3D12Device5* VulkanGraphicsDevice::GetRayTracingDevice() const
    {
        return nullptr;
    }

    const RhiBackendCapabilities& VulkanGraphicsDevice::GetCapabilities() const
    {
        return m_capabilities;
    }

    bool VulkanGraphicsDevice::SupportsHardwareRayTracing() const
    {
        return m_capabilities.supportsRayTracingPipeline || m_capabilities.supportsRayQuery;
    }

    CommandQueue& VulkanGraphicsDevice::GetCommandQueue()
    {
        return m_emptyGraphicsQueue;
    }

    CommandQueue& VulkanGraphicsDevice::GetComputeQueue()
    {
        return m_emptyComputeQueue;
    }

    SwapChain& VulkanGraphicsDevice::GetSwapChain()
    {
        return m_emptySwapChain;
    }

    UINT VulkanGraphicsDevice::GetDescriptorHandleIncrementSize(DescriptorHeapType) const
    {
        return 0;
    }

    void VulkanGraphicsDevice::WaitForGPU()
    {
        if (m_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device);
        }
    }

    bool VulkanGraphicsDevice::RenderBackendClearFrame(const float clearColor[4])
    {
        RhiBackendFrameDesc frameDesc{};
        if (clearColor) {
            frameDesc.clearColor = { clearColor[0], clearColor[1], clearColor[2], clearColor[3] };
        }
        frameDesc.present = true;
        return ExecuteBackendFrame(frameDesc);
    }

    bool VulkanGraphicsDevice::ResizeBackendSwapChain(UINT width, UINT height)
    {
        if (m_device == VK_NULL_HANDLE || width == 0 || height == 0) {
            return false;
        }
        if (vkDeviceWaitIdle(m_device) != VK_SUCCESS) {
            return false;
        }

        const UINT bufferCount = std::max<UINT>(
            1u,
            static_cast<UINT>(m_commandBuffers.size()));
        if (!CreateSwapChain(width, height, bufferCount)) {
            return false;
        }

        m_capabilities.supportsSwapChain = true;
        return true;
    }


} // namespace SasamiRenderer
#endif
