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
            case RhiFormat::R32G32Float: return VK_FORMAT_R32G32_SFLOAT;
            case RhiFormat::R32Float: return VK_FORMAT_R32_SFLOAT;
            case RhiFormat::R32UInt: return VK_FORMAT_R32_UINT;
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

        VkDescriptorType ToVkDescriptorType(RhiBindingType type)
        {
            switch (type) {
            case RhiBindingType::ConstantBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            case RhiBindingType::UnorderedAccess: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            case RhiBindingType::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
            case RhiBindingType::ShaderResource:
            default: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            }
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
            m_recording = false;
            return vkEndCommandBuffer(m_commandBuffer) == VK_SUCCESS;
        }

        void TransitionResources(const RhiResourceTransitionDesc* transitions, uint32_t count) override
        {
            if (!IsValid() || !transitions || count == 0) {
                return;
            }

            std::vector<VkImageMemoryBarrier> imageBarriers;
            imageBarriers.reserve(count);
            VkPipelineStageFlags srcStages = 0;
            VkPipelineStageFlags dstStages = 0;
            for (uint32_t i = 0; i < count; ++i) {
                const auto resourceIt = m_device.m_rhiResources.find(transitions[i].resource.id);
                if (resourceIt == m_device.m_rhiResources.end() || resourceIt->second.image == VK_NULL_HANDLE) {
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
                barrier.image = resourceIt->second.image;
                barrier.subresourceRange.aspectMask =
                    transitions[i].after == RhiResourceState::DepthWrite || transitions[i].after == RhiResourceState::DepthRead
                        ? VK_IMAGE_ASPECT_DEPTH_BIT
                        : VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.baseMipLevel = 0;
                barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
                barrier.subresourceRange.baseArrayLayer = 0;
                barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
                imageBarriers.push_back(barrier);
                srcStages |= ToVkPipelineStage(transitions[i].before);
                dstStages |= ToVkPipelineStage(transitions[i].after);
            }

            if (!imageBarriers.empty()) {
                vkCmdPipelineBarrier(m_commandBuffer,
                                     srcStages ? srcStages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     dstStages ? dstStages : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                     0,
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     static_cast<uint32_t>(imageBarriers.size()),
                                     imageBarriers.data());
            }
        }

        void SetGraphicsPipeline(RhiPipelineHandle pipelineHandle) override
        {
            const auto it = m_device.m_rhiPipelines.find(pipelineHandle.id);
            if (IsValid() && it != m_device.m_rhiPipelines.end() && it->second.pipeline != VK_NULL_HANDLE) {
                vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, it->second.pipeline);
            }
        }

        void SetComputePipeline(RhiPipelineHandle pipelineHandle) override
        {
            const auto it = m_device.m_rhiPipelines.find(pipelineHandle.id);
            if (IsValid() && it != m_device.m_rhiPipelines.end() && it->second.pipeline != VK_NULL_HANDLE) {
                vkCmdBindPipeline(m_commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, it->second.pipeline);
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
            if (IsValid()) {
                vkCmdDraw(m_commandBuffer,
                          draw.vertexCount,
                          draw.instanceCount,
                          draw.startVertex,
                          draw.startInstance);
            }
        }

        void DrawIndexed(const RhiDrawIndexedDesc& draw) override
        {
            if (IsValid()) {
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
            if (IsValid()) {
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

    private:
        VulkanGraphicsDevice& m_device;
        RhiQueueType m_queueType = RhiQueueType::Graphics;
        VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
        bool m_recording = false;
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


} // namespace SasamiRenderer
#endif
