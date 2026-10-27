#pragma once

// VulkanAmbientOcclusionInternal.h
// Shared internals of the two VulkanAmbientOcclusion translation units (the resource/recording
// half in VulkanAmbientOcclusion.cpp and the pipeline half in VulkanAmbientOcclusionPipelines.cpp):
// the two GPU-facing struct layouts, the skinned-occluder budget, and the viewport convention all
// of its render passes share.
//
// Everything here is at namespace scope rather than in an anonymous namespace on purpose: an
// anonymous namespace in a header gives each including translation unit its own copy of every
// entity, which has produced C2084/C2011 redefinition errors in this codebase before.

#include "Renderer/Backends/Vulkan/VulkanAmbientOcclusion.h"

#if RHI_VULKAN

namespace SasamiEngine::Render
{
    // Mirrors the main skinned mesh path's per-frame slot budget (VulkanGraphicsDevice.cpp's
    // kMaxSimultaneousSkinnedDraws) and VulkanShadowMap's kMaxShadowSkinnedCasters, so a scene
    // that fits the forward pass's skinned budget also fits the AO prepass's bone UBO. An occluder
    // past the budget is skipped rather than allowed to overwrite another slot's bones.
    inline constexpr uint32_t kMaxAoSkinnedOccluders = 16u;

    inline constexpr float kAoIdentity4x4[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };

    // Matches NativeAoPrepass.hlsl / NativeAoPrepassSkinned.hlsl's AoPrepassPushConstants.
    struct VulkanAoPrepassPushConstants
    {
        float viewProjection[16];
        float model[16];
    };
    static_assert(sizeof(VulkanAoPrepassPushConstants) == 128,
                  "AO prepass push constants must stay within the guaranteed 128-byte minimum.");

    // Matches SSAO_PS.hlsl's SSAOCB exactly, which is also what Renderer.cpp pushes for the
    // DirectX 12 pass (u_cameraPV, u_cameraInvPV, u_ssaoParams, u_screenSize, u_cameraPos).
    // SSAO_Blur_PS.hlsl's BlurCB is this struct's leading 160 bytes, so both passes read the one
    // buffer rather than each owning a copy that could drift.
    struct VulkanSsaoConstants
    {
        float cameraPV[16];
        float cameraInvPV[16];
        float ssaoParams[4];  // x: radius, y: bias, z: intensity, w: thickness
        float screenSize[4];  // x: width, y: height, z: 1/width, w: 1/height
        float cameraPos[4];   // xyz: world camera position, w: quality index
    };
    static_assert(sizeof(VulkanSsaoConstants) == 176,
                  "SSAO constants must stay byte-compatible with SSAO_PS.hlsl's SSAOCB.");

    // Negative viewport height (VK_KHR_maintenance1, core since Vulkan 1.1) puts the framebuffer
    // origin at the top-left, i.e. Direct3D's convention: framebuffer_y = (1 - ndc_y) / 2 * height.
    //
    // Every pass in this module uses it, and that is load-bearing, not cosmetic:
    //   * the main native mesh pass already flips this way, so the prepass must too or its depth
    //     would be upside-down relative to the frame it describes;
    //   * SSAO_VS.hlsl emits Direct3D-convention clip positions (uv (0,0) -> ndc (-1, +1)), and
    //     ReconstructWorldPos / the kernel's ndc -> uv remap in SSAO_PS.hlsl both assume uv.y = 0
    //     is the top row. Flipping here is what makes those shared shaders correct unmodified.
    inline void SetFlippedViewportAndScissor(VkCommandBuffer cmd, VkExtent2D extent)
    {
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = static_cast<float>(extent.height);
        viewport.width = static_cast<float>(extent.width);
        viewport.height = -static_cast<float>(extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.offset = { 0, 0 };
        scissor.extent = extent;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }
} // namespace SasamiEngine::Render

#endif
