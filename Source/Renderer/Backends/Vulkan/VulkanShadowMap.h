#pragma once

// VulkanShadowMap.h
// Self-contained directional shadow-map resource for the Vulkan native mesh path: an offscreen
// depth-only render target plus the two depth-only caster pipelines (static and skinned) that
// render into it, and the comparison sampler the main mesh pixel shaders sample it with. Ported
// from the DirectX 11 reference (Dx11ShadowMap.h/.cpp) -- same resolution rule (0 = disabled),
// same reverse-Z convention (near=1.0, far=0.0) and the same GREATER_EQUAL comparison direction,
// just expressed with Vulkan handles instead of D3D11 COM objects.
//
// Deliberately owns its own pipeline layouts/descriptor sets rather than sharing anything with
// VulkanGraphicsDevice's main mesh pipeline: the caster pass only ever needs a light-space
// transform and (for skinned casters) a bone palette, so it has no reason to touch the main
// path's texture/frame descriptor sets at all.

#include "Renderer/RHI/GraphicsDevice.h"

#if RHI_VULKAN
#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>
#include <cstdint>

namespace SasamiEngine::Render
{
    // Mirrors Dx11ShadowCaster: one draw call's worth of geometry to render into the shadow map.
    // Pointers are caller-owned for the duration of the synchronous Render() call, same contract
    // as RhiBackendSkinnedMeshDrawDesc::boneMatrices.
    struct VulkanShadowCaster
    {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        bool index32Bit = false;
        const float* model = nullptr;        // row-major, row-vector convention; nullptr = identity
        const float* boneMatrices = nullptr;  // non-null => skinned caster (Skeleton::kMaxBones * 16 floats)
    };

    class VulkanShadowMap
    {
    public:
        // resolution == 0 disables shadows entirely (matches Dx11ShadowMap::Initialize): the
        // comparison sampler and a 1x1 dummy depth view are still created so callers always have
        // a valid descriptor to bind, but IsValid() is false and no caster pipelines are built.
        bool Initialize(VkDevice device,
                        VkPhysicalDevice physicalDevice,
                        VkCommandPool commandPool,
                        VkQueue graphicsQueue,
                        uint32_t resolution);
        void Destroy();

        // Records the shadow depth pre-pass (its own render pass, begun and ended within this
        // call) into `cmd`. Must be called -- and finish recording -- before any render pass that
        // reads the shadow map begins; Vulkan render passes cannot nest. No-op when !IsValid() or
        // casterCount == 0.
        void Render(VkCommandBuffer cmd,
                    const VulkanShadowCaster* casters,
                    uint32_t casterCount,
                    const float lightViewProjection[16]);

        bool IsValid() const { return m_valid; }
        uint32_t Resolution() const { return m_valid ? m_resolution : 0; }
        // Returns the real depth view once initialized with a non-zero resolution, otherwise a
        // 1x1 dummy depth view -- always a valid SAMPLED_IMAGE-compatible view in
        // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL so callers can unconditionally bind it.
        VkImageView ImageView() const { return m_valid ? m_depthView : m_dummyDepthView; }
        VkSampler ComparisonSampler() const { return m_comparisonSampler; }

    private:
        uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;
        bool CreateDummyDepthResources();
        bool CreateComparisonSampler();
        bool CreateRealDepthResources(uint32_t resolution);
        bool CreateStaticPipeline();
        bool CreateSkinnedPipeline();
        // Transitions `image` (a single-mip, single-layer depth image, aspect DEPTH_BIT) from
        // VK_IMAGE_LAYOUT_UNDEFINED to VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL via a one-time
        // command buffer, so a freshly created depth image is already valid to bind as a
        // descriptor before the first Render() call ever runs (see the header comment on
        // ImageView()).
        bool TransitionDepthImageToShaderReadOnly(VkImage image);

        VkDevice m_device = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkCommandPool m_commandPool = VK_NULL_HANDLE;
        VkQueue m_graphicsQueue = VK_NULL_HANDLE;

        VkSampler m_comparisonSampler = VK_NULL_HANDLE;

        // 1x1 fallback bound whenever the real shadow map is unavailable/disabled.
        VkImage m_dummyDepthImage = VK_NULL_HANDLE;
        VkImageView m_dummyDepthView = VK_NULL_HANDLE;
        VkDeviceMemory m_dummyDepthMemory = VK_NULL_HANDLE;

        // Real resolution x resolution shadow map.
        VkImage m_depthImage = VK_NULL_HANDLE;
        VkImageView m_depthView = VK_NULL_HANDLE;
        VkDeviceMemory m_depthMemory = VK_NULL_HANDLE;
        VkRenderPass m_renderPass = VK_NULL_HANDLE;
        VkFramebuffer m_framebuffer = VK_NULL_HANDLE;

        // Static caster pipeline: push constants only (lightViewProjection + model = 128 bytes).
        VkPipelineLayout m_staticPipelineLayout = VK_NULL_HANDLE;
        VkPipeline m_staticPipeline = VK_NULL_HANDLE;

        // Skinned caster pipeline: same 128-byte push constants plus a dedicated bone-palette
        // dynamic UBO (set 0, binding 0, vertex stage only -- mirrors Dx11ShadowMap's ShadowBoneCB
        // at b1, just split into its own descriptor set here instead of a second constant buffer
        // slot since Vulkan push constants are already full).
        VkDescriptorSetLayout m_boneDescSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool m_boneDescPool = VK_NULL_HANDLE;
        VkDescriptorSet m_boneDescSet = VK_NULL_HANDLE;
        VkBuffer m_boneUbo = VK_NULL_HANDLE;
        VkDeviceMemory m_boneUboMemory = VK_NULL_HANDLE;
        void* m_boneUboMapped = nullptr;
        VkDeviceSize m_boneSlotStride = 0;
        VkPipelineLayout m_skinnedPipelineLayout = VK_NULL_HANDLE;
        VkPipeline m_skinnedPipeline = VK_NULL_HANDLE;

        uint32_t m_resolution = 0;
        bool m_valid = false;
    };
} // namespace SasamiEngine::Render
#endif
