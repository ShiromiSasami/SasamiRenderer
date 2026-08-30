#pragma once

#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"

#if RHI_VULKAN

#include <algorithm>
#include <cstring>
#include <vector>

namespace SasamiRenderer
{
    inline bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
    {
        return std::any_of(extensions.begin(), extensions.end(),
                           [name](const VkExtensionProperties& extension) {
                               return std::strcmp(extension.extensionName, name) == 0;
                           });
    }

    inline std::vector<VkExtensionProperties> EnumerateDeviceExtensions(VkPhysicalDevice physicalDevice)
    {
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        if (extensionCount > 0) {
            vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, extensions.data());
        }
        return extensions;
    }

    inline VkFormat ToVkFormat(RhiFormat format)
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

    inline VkImageUsageFlags ToVkImageUsage(RhiTextureUsageFlags usage)
    {
        VkImageUsageFlags flags = 0;
        if (HasFlag(usage, RhiTextureUsageFlags::ShaderResource)) flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (HasFlag(usage, RhiTextureUsageFlags::RenderTarget)) flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (HasFlag(usage, RhiTextureUsageFlags::DepthStencil)) flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (HasFlag(usage, RhiTextureUsageFlags::UnorderedAccess)) flags |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (HasFlag(usage, RhiTextureUsageFlags::CopySource)) flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (HasFlag(usage, RhiTextureUsageFlags::CopyDest)) flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        return flags;
    }

    inline VkBufferUsageFlags ToVkBufferUsage(RhiBufferUsageFlags usage)
    {
        VkBufferUsageFlags flags = 0;
        if (HasFlag(usage, RhiBufferUsageFlags::Vertex)) flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (HasFlag(usage, RhiBufferUsageFlags::Index)) flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (HasFlag(usage, RhiBufferUsageFlags::Constant)) flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (HasFlag(usage, RhiBufferUsageFlags::ShaderResource) ||
            HasFlag(usage, RhiBufferUsageFlags::Structured) ||
            HasFlag(usage, RhiBufferUsageFlags::UnorderedAccess)) {
            flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        }
        if (HasFlag(usage, RhiBufferUsageFlags::CopySource)) flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (HasFlag(usage, RhiBufferUsageFlags::CopyDest)) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (HasFlag(usage, RhiBufferUsageFlags::AccelerationStructure)) {
#ifdef VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
            flags |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
#endif
        }
        return flags;
    }

    inline VkMemoryPropertyFlags ToVkMemoryProperties(RhiMemoryUsage usage)
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

    inline VkImageViewType ToVkImageViewType(RhiTextureViewDimension dimension)
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

    inline VkImageAspectFlags ToVkAspectMask(RhiFormat format)
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

    inline VkShaderStageFlagBits ToVkShaderStage(RhiShaderStageFlags stage)
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

    inline VkShaderStageFlags ToVkShaderStages(RhiShaderStageFlags stages)
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

    inline VkDescriptorType ToVkDescriptorType(const RhiBindingRangeDesc& binding)
    {
        switch (binding.type) {
        case RhiBindingType::ConstantBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case RhiBindingType::UnorderedAccess:
            return binding.bufferResource ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case RhiBindingType::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
        case RhiBindingType::ShaderResource:
        default:
            return binding.bufferResource ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
    }

    inline uint32_t ToVkBindingNumber(RhiBindingType type, uint32_t shaderRegister)
    {
        constexpr uint32_t kRegisterClassSpan = 100;
        switch (type) {
        case RhiBindingType::ShaderResource: return kRegisterClassSpan + shaderRegister;
        case RhiBindingType::Sampler: return kRegisterClassSpan * 2u + shaderRegister;
        case RhiBindingType::UnorderedAccess: return kRegisterClassSpan * 3u + shaderRegister;
        case RhiBindingType::ConstantBuffer:
        default: return shaderRegister;
        }
    }

    inline VkAccessFlags ToVkAccessFlags(RhiResourceState state)
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
        default: return VK_ACCESS_MEMORY_READ_BIT;
        }
    }

    inline VkPipelineStageFlags ToVkPipelineStage(RhiResourceState state)
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
        case RhiResourceState::Present: return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        case RhiResourceState::Common:
        default: return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
    }

    inline VkPrimitiveTopology ToVkTopology(RhiPrimitiveTopology topology)
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

    inline VkCullModeFlags ToVkCullMode(RhiCullMode mode)
    {
        switch (mode) {
        case RhiCullMode::None: return VK_CULL_MODE_NONE;
        case RhiCullMode::Front: return VK_CULL_MODE_FRONT_BIT;
        case RhiCullMode::Back:
        default: return VK_CULL_MODE_BACK_BIT;
        }
    }

    inline VkPolygonMode ToVkPolygonMode(RhiFillMode mode)
    {
        return mode == RhiFillMode::Wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    }

    inline VkCompareOp ToVkCompareOp(RhiCompareOp op)
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
}

#endif
