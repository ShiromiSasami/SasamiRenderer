#pragma once

// VulkanAmbientOcclusion.h
// Runtime (screen-space) ambient occlusion for the Vulkan native mesh path: the `runtimeAo` factor
// DirectX 12 reads out of RuntimeAOTex in Raster/Lighting/Deferred/DeferredLighting_PS.hlsl.
//
// Why this exists as its own module rather than as more of VulkanGraphicsDevice: DirectX 12 gets
// SSAO for free because it is deferred -- the G-Buffer already holds depth and world normals when
// the SSAO pass runs. The native Vulkan path is forward-shaded and has neither, so the generator
// has to produce its own inputs. That makes this three chained render passes with their own
// targets, which is a responsibility of its own:
//
//   1. depth + world-normal prepass  (NativeAoPrepass.hlsl / NativeAoPrepassSkinned.hlsl)
//   2. SSAO                          (Effects/AmbientOcclusion/SSAO/SSAO_VS.hlsl + SSAO_PS.hlsl)
//   3. cross-bilateral blur          (the same VS + SSAO_Blur_PS.hlsl)
//
// Passes 2 and 3 compile the *same HLSL DirectX 12 uses*, to SPIR-V instead of DXIL -- no port, no
// second copy of the kernel to keep in step. Only the prepass is Vulkan-specific, because it is
// standing in for a G-Buffer that does not exist here.
//
// Deliberately modelled on VulkanShadowMap: same Initialize/Destroy/IsValid/Render shape, same
// "a view is always bindable even when the real thing failed to build" contract (a 1x1 white image
// stands in for AO = 1.0, i.e. no occlusion), and the same non-fatal failure policy -- a frame that
// cannot build AO renders without it instead of not rendering.
//
// Cost note: the prepass re-submits the frame's opaque geometry. That is the price of generating
// depth+normals in a forward path, and it buys same-frame AO -- the alternative (adding a second
// colour attachment to the main mesh pass) would only produce AO *after* shading, usable no
// earlier than the next frame.

#include "Renderer/RHI/GraphicsDevice.h"

#if RHI_VULKAN
#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>
#include <cstdint>

#include "Renderer/Backends/Vulkan/VulkanShadowMap.h"

namespace SasamiEngine::Render
{
    // The prepass needs exactly what the shadow caster pass needs -- vertex/index buffers, counts,
    // index width, a model matrix and an optional bone palette -- so it consumes the device's
    // existing per-frame caster list verbatim instead of declaring an identical struct of its own.
    // The alias exists only so call sites read in AO terms.
    using VulkanAoPrepassGeometry = VulkanShadowCaster;

    class VulkanAmbientOcclusion
    {
    public:
        // Per-frame tuning + camera state. Mirrors the SSAOCB that Renderer.cpp pushes for the
        // DirectX 12 pass (u_cameraPV / u_cameraInvPV / u_ssaoParams / u_screenSize / u_cameraPos),
        // minus the screen size, which this module already knows from its own extent.
        struct Params
        {
            const float* viewProjection = nullptr;     // row-major, row-vector, reverse-Z; 16 floats
            const float* invViewProjection = nullptr;  // inverse of the above; 16 floats
            const float* cameraPos = nullptr;          // world-space camera position; 3 floats
            float radius = 0.5f;
            float bias = 0.025f;
            float intensity = 1.0f;
            float thickness = 0.15f;
            uint32_t quality = 1u;  // clamped to 0..2 by SSAO_PS.hlsl's ResolveSampleCount
        };

        bool Initialize(VkDevice device,
                        VkPhysicalDevice physicalDevice,
                        VkCommandPool commandPool,
                        VkQueue graphicsQueue,
                        VkExtent2D extent);
        void Destroy();

        // Records all three passes into `cmd`, each beginning and ending its own render pass.
        // Must be called -- and finish recording -- before any render pass that reads the result
        // begins; Vulkan render passes cannot nest. Returns false (recording nothing) when the
        // module is unavailable or there is no geometry, in which case AoImageView() keeps
        // returning the white no-occlusion fallback.
        bool Render(VkCommandBuffer cmd,
                    const VulkanAoPrepassGeometry* geometry,
                    uint32_t geometryCount,
                    const Params& params);

        bool IsValid() const { return m_valid; }
        VkExtent2D Extent() const { return m_extent; }

        // Always a valid SAMPLED_IMAGE-compatible view in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        // so callers can bind it unconditionally: the blurred AO target once initialized, otherwise
        // a 1x1 white image that reads as runtimeAo = 1.0 (no occlusion).
        VkImageView AoImageView() const { return m_valid ? m_blurView : m_whiteView; }
        VkSampler AoSampler() const { return m_linearClampSampler; }

        // Debug: blits the AO result over `dstImage`, which must already be in
        // VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL. Leaves the AO target back in
        // SHADER_READ_ONLY_OPTIMAL so any descriptor referencing it stays valid. No-op when the
        // module is unavailable.
        //
        // This is the "display AO on its own before trusting it" step TODO.md's 2026-09-17
        // retrospective asks for: with it, a wrong AO buffer is visible directly instead of only
        // as a wrong-looking lit frame.
        void RecordDebugBlit(VkCommandBuffer cmd, VkImage dstImage, VkExtent2D dstExtent) const;

        // True when SASAMI_VK_AO_DEBUG is set to something other than "0". Opt-in via the
        // environment for the same reason SASAMI_GPU_VALIDATION is: the backend only ever sees
        // RhiBackendMeshFrameDesc, which carries no debug-view field.
        static bool IsDebugViewEnabled();

    private:
        // -- VulkanAmbientOcclusion.cpp --
        uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;
        bool CreateImage(VkFormat format,
                         VkImageUsageFlags usage,
                         VkImageAspectFlags aspect,
                         VkExtent2D extent,
                         VkImage& outImage,
                         VkDeviceMemory& outMemory,
                         VkImageView& outView) const;
        bool TransitionImageToShaderReadOnly(VkImage image, VkImageAspectFlags aspect) const;
        bool CreateWhiteFallback();
        bool CreateSamplers();
        bool CreateTargets();
        bool CreateRenderPasses();
        bool CreateFramebuffers();
        bool CreateConstantBuffer();
        bool CreateDescriptorSets();
        void RecordPrepass(VkCommandBuffer cmd,
                           const VulkanAoPrepassGeometry* geometry,
                           uint32_t geometryCount,
                           const Params& params);
        void RecordFullscreenPass(VkCommandBuffer cmd,
                                  VkRenderPass renderPass,
                                  VkFramebuffer framebuffer,
                                  VkPipeline pipeline,
                                  VkPipelineLayout pipelineLayout,
                                  VkDescriptorSet descriptorSet) const;

        // -- VulkanAmbientOcclusionPipelines.cpp --
        bool CreatePrepassStaticPipeline();
        bool CreatePrepassSkinnedPipeline();
        bool CreateSsaoPipeline();
        bool CreateBlurPipeline();
        // Shared by the four pipeline builders above: compiles one HLSL entry point to SPIR-V and
        // wraps it in a shader module. Returns VK_NULL_HANDLE on failure (already logged).
        VkShaderModule CreateShaderModule(const wchar_t* relativeShaderPath,
                                          const char* entryPoint,
                                          const char* target) const;

        VkDevice m_device = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkCommandPool m_commandPool = VK_NULL_HANDLE;
        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        VkExtent2D m_extent{ 0u, 0u };

        // 1x1 white stand-in bound whenever the real AO target is unavailable.
        VkImage m_whiteImage = VK_NULL_HANDLE;
        VkImageView m_whiteView = VK_NULL_HANDLE;
        VkDeviceMemory m_whiteMemory = VK_NULL_HANDLE;

        // Point-clamp is what SSAO_PS.hlsl/SSAO_Blur_PS.hlsl expect at s0 (DirectX 12 declares it
        // as a static sampler); linear-clamp is what consumers of the finished AO texture want.
        VkSampler m_pointClampSampler = VK_NULL_HANDLE;
        VkSampler m_linearClampSampler = VK_NULL_HANDLE;

        // Pass 1 targets. Depth carries SAMPLED_BIT so the SSAO pass can read it back.
        VkImage m_depthImage = VK_NULL_HANDLE;
        VkImageView m_depthView = VK_NULL_HANDLE;
        VkDeviceMemory m_depthMemory = VK_NULL_HANDLE;
        VkImage m_normalImage = VK_NULL_HANDLE;
        VkImageView m_normalView = VK_NULL_HANDLE;
        VkDeviceMemory m_normalMemory = VK_NULL_HANDLE;

        // Pass 2 and pass 3 targets, both R8G8B8A8_UNORM to match the DirectX 12 SSAO/SSAOBlur
        // render-target formats (RenderPipelineStateCacheSsao.cpp) so the shared pixel shaders see
        // identical precision on both backends.
        VkImage m_ssaoImage = VK_NULL_HANDLE;
        VkImageView m_ssaoView = VK_NULL_HANDLE;
        VkDeviceMemory m_ssaoMemory = VK_NULL_HANDLE;
        VkImage m_blurImage = VK_NULL_HANDLE;
        VkImageView m_blurView = VK_NULL_HANDLE;
        VkDeviceMemory m_blurMemory = VK_NULL_HANDLE;

        VkRenderPass m_prepassRenderPass = VK_NULL_HANDLE;
        VkRenderPass m_ssaoRenderPass = VK_NULL_HANDLE;
        VkRenderPass m_blurRenderPass = VK_NULL_HANDLE;
        VkFramebuffer m_prepassFramebuffer = VK_NULL_HANDLE;
        VkFramebuffer m_ssaoFramebuffer = VK_NULL_HANDLE;
        VkFramebuffer m_blurFramebuffer = VK_NULL_HANDLE;

        // Prepass pipelines. Static takes push constants only; skinned adds a bone-palette dynamic
        // UBO at set 0 binding 0, exactly as VulkanShadowMap's skinned caster pipeline does.
        VkPipelineLayout m_prepassStaticPipelineLayout = VK_NULL_HANDLE;
        VkPipeline m_prepassStaticPipeline = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_boneDescSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool m_boneDescPool = VK_NULL_HANDLE;
        VkDescriptorSet m_boneDescSet = VK_NULL_HANDLE;
        VkBuffer m_boneUbo = VK_NULL_HANDLE;
        VkDeviceMemory m_boneUboMemory = VK_NULL_HANDLE;
        void* m_boneUboMapped = nullptr;
        VkDeviceSize m_boneSlotStride = 0;
        VkPipelineLayout m_prepassSkinnedPipelineLayout = VK_NULL_HANDLE;
        VkPipeline m_prepassSkinnedPipeline = VK_NULL_HANDLE;

        // SSAO constants, written once per Render(). Both fullscreen passes read it: SSAO_PS.hlsl's
        // SSAOCB is the whole struct, SSAO_Blur_PS.hlsl's BlurCB is its leading 160 bytes.
        VkBuffer m_constantBuffer = VK_NULL_HANDLE;
        VkDeviceMemory m_constantMemory = VK_NULL_HANDLE;
        void* m_constantMapped = nullptr;

        VkDescriptorSetLayout m_ssaoDescSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_blurDescSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool m_fullscreenDescPool = VK_NULL_HANDLE;
        VkDescriptorSet m_ssaoDescSet = VK_NULL_HANDLE;
        VkDescriptorSet m_blurDescSet = VK_NULL_HANDLE;
        VkPipelineLayout m_ssaoPipelineLayout = VK_NULL_HANDLE;
        VkPipeline m_ssaoPipeline = VK_NULL_HANDLE;
        VkPipelineLayout m_blurPipelineLayout = VK_NULL_HANDLE;
        VkPipeline m_blurPipeline = VK_NULL_HANDLE;

        bool m_valid = false;
    };
} // namespace SasamiEngine::Render
#endif
