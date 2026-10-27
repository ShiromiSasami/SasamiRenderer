// VulkanGraphicsDevice.cpp
// Vulkan ExecuteBackendFrame, D3D12 compatibility wrappers.
#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"
#include "Renderer/Backends/Vulkan/VulkanGraphicsDeviceUtils.h"
#include "Renderer/Resources/ShaderCompilationService.h"
#include "Renderer/Structures/Skeleton.h"
#include "Foundation/Math/MathUtil.h"

#if RHI_VULKAN

#include "Foundation/Tools/DebugOutput.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>
#include <wrl/client.h>


namespace SasamiEngine::Render
{
    bool VulkanGraphicsDevice::ExecuteBackendFrame(const RhiBackendFrameDesc& frameDesc)
    {
        if (m_device == VK_NULL_HANDLE ||
            m_swapchain == VK_NULL_HANDLE ||
            m_commandBuffers.empty() ||
            m_frameFences.empty() ||
            m_swapchainImages.empty() ||
            !frameDesc.present) {
            return false;
        }

        const uint32_t frame = m_currentFrame % static_cast<uint32_t>(m_commandBuffers.size());
        vkWaitForFences(m_device, 1, &m_frameFences[frame], VK_TRUE, UINT64_MAX);

        uint32_t imageIndex = 0;
        VkResult result = vkAcquireNextImageKHR(m_device,
                                                m_swapchain,
                                                UINT64_MAX,
                                                m_imageAvailableSemaphores[frame],
                                                VK_NULL_HANDLE,
                                                &imageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            ResizeBackendSwapChain(m_swapchainExtent.width, m_swapchainExtent.height);
            return false;
        }
        const bool recreateAfterPresent = result == VK_SUBOPTIMAL_KHR;
        if (result != VK_SUCCESS && !recreateAfterPresent) {
            return false;
        }
        if (imageIndex >= m_swapchainImages.size()) {
            return false;
        }

        VkCommandBuffer cmd = m_commandBuffers[frame];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(cmd, &beginInfo);
        if (result != VK_SUCCESS) {
            return false;
        }

        if (frameDesc.mesh.enabled) {
            // Casters go in first: the shadow pass records its own depth-only render pass, which
            // must be fully begun and ended (Vulkan render passes cannot nest) before
            // RenderMeshFrame's render pass begins. Mirrors Dx11GraphicsDevice::ExecuteBackendFrame's
            // ordering (m_shadowMapReadyThisFrame = RenderNativeShadowMap(...) before the mesh draw).
            //
            // The caster list is built once here and read by both offscreen passes: the shadow map
            // and the ambient-occlusion depth+normal prepass need the identical set of opaque
            // geometry, so neither gathers its own copy.
            BuildNativeCasterList(frameDesc.mesh);
            m_nativeShadowMapReadyThisFrame = RenderNativeShadowMap(cmd, frameDesc.mesh);
            // Same nesting rule, same slot in the frame: runtime AO is generated before the forward
            // pass so it is usable within this frame rather than one frame late.
            m_nativeAmbientOcclusionReadyThisFrame = RenderNativeAmbientOcclusion(cmd, frameDesc.mesh);
        } else {
            m_nativeAmbientOcclusionReadyThisFrame = false;
        }

        VkImageLayout frameOutputLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkAccessFlags frameOutputAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
        VkPipelineStageFlags frameOutputStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

        if (frameDesc.rayMarch.enabled &&
            RenderRayMarchFrame(imageIndex, cmd, frameDesc.rayMarch, frameDesc.clearColor)) {
            frameOutputLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            frameOutputAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            frameOutputStage  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        } else if (frameDesc.mesh.enabled &&
            ((frameDesc.mesh.draws && frameDesc.mesh.drawCount > 0) ||
             (frameDesc.mesh.skinnedDraws && frameDesc.mesh.skinnedDrawCount > 0)) &&
            RenderMeshFrame(frame, imageIndex, cmd, frameDesc.mesh, frameDesc.clearColor)) {
            frameOutputLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            frameOutputAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            frameOutputStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        } else {
            VkImageMemoryBarrier toTransfer{};
            toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toTransfer.srcAccessMask = 0;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toTransfer.oldLayout = m_swapchainImageLayouts[imageIndex];
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.image = m_swapchainImages[imageIndex];
            toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toTransfer.subresourceRange.baseMipLevel = 0;
            toTransfer.subresourceRange.levelCount = 1;
            toTransfer.subresourceRange.baseArrayLayer = 0;
            toTransfer.subresourceRange.layerCount = 1;

            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0,
                                 0,
                                 nullptr,
                                 0,
                                 nullptr,
                                 1,
                                 &toTransfer);

            VkClearColorValue vkClear{};
            vkClear.float32[0] = frameDesc.clearColor.r;
            vkClear.float32[1] = frameDesc.clearColor.g;
            vkClear.float32[2] = frameDesc.clearColor.b;
            vkClear.float32[3] = frameDesc.clearColor.a;

            VkImageSubresourceRange colorRange{};
            colorRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            colorRange.baseMipLevel = 0;
            colorRange.levelCount = 1;
            colorRange.baseArrayLayer = 0;
            colorRange.layerCount = 1;
            vkCmdClearColorImage(cmd,
                                 m_swapchainImages[imageIndex],
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &vkClear,
                                 1,
                                 &colorRange);
        }

        // AO-only debug view. Opt-in through SASAMI_VK_AO_DEBUG because the backend never sees
        // RenderSettings -- only RhiBackendMeshFrameDesc, which has no debug-view field. Replaces
        // the shaded frame with the raw runtime-AO texture, which is the first thing TODO.md's
        // 2026-09-17 retrospective asks for: a wrong AO buffer should be visible as a wrong AO
        // buffer, not inferred from a wrong-looking lit frame. The overlay still draws on top.
        if (m_nativeAmbientOcclusionReadyThisFrame && VulkanAmbientOcclusion::IsDebugViewEnabled()) {
            if (frameOutputLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
                VkImageMemoryBarrier toTransferDst{};
                toTransferDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toTransferDst.srcAccessMask = frameOutputAccess;
                toTransferDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toTransferDst.oldLayout = frameOutputLayout;
                toTransferDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toTransferDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toTransferDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toTransferDst.image = m_swapchainImages[imageIndex];
                toTransferDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                toTransferDst.subresourceRange.baseMipLevel = 0;
                toTransferDst.subresourceRange.levelCount = 1;
                toTransferDst.subresourceRange.baseArrayLayer = 0;
                toTransferDst.subresourceRange.layerCount = 1;
                vkCmdPipelineBarrier(cmd,
                                     frameOutputStage,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &toTransferDst);
            }

            m_nativeAmbientOcclusion.RecordDebugBlit(cmd, m_swapchainImages[imageIndex], m_swapchainExtent);

            frameOutputLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            frameOutputAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
            frameOutputStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }

        if (frameDesc.overlay && EnsureUIOverlayResources()) {
            if (frameOutputLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                // The clear-only path above (no scene drawn) leaves the image in
                // TRANSFER_DST_OPTIMAL; the overlay render pass needs COLOR_ATTACHMENT_OPTIMAL.
                VkImageMemoryBarrier toOverlayColor{};
                toOverlayColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toOverlayColor.srcAccessMask = frameOutputAccess;
                toOverlayColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                toOverlayColor.oldLayout = frameOutputLayout;
                toOverlayColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                toOverlayColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toOverlayColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toOverlayColor.image = m_swapchainImages[imageIndex];
                toOverlayColor.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                toOverlayColor.subresourceRange.baseMipLevel = 0;
                toOverlayColor.subresourceRange.levelCount = 1;
                toOverlayColor.subresourceRange.baseArrayLayer = 0;
                toOverlayColor.subresourceRange.layerCount = 1;

                vkCmdPipelineBarrier(cmd,
                                     frameOutputStage,
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                     0,
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     1,
                                     &toOverlayColor);
            }

            VkRenderPassBeginInfo overlayBeginInfo{};
            overlayBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            overlayBeginInfo.renderPass = m_uiOverlayRenderPass;
            overlayBeginInfo.framebuffer = m_uiOverlayFramebuffers[imageIndex];
            overlayBeginInfo.renderArea.offset = { 0, 0 };
            overlayBeginInfo.renderArea.extent = m_swapchainExtent;
            overlayBeginInfo.clearValueCount = 0; // loadOp is LOAD; nothing to clear here
            vkCmdBeginRenderPass(cmd, &overlayBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

            RhiOverlayFrameContext overlayContext{};
            overlayContext.api = RhiBackendApi::Vulkan;
            overlayContext.commandBuffer = reinterpret_cast<void*>(cmd);
            overlayContext.imageIndex = imageIndex;
            overlayContext.frameIndex = frame;
            overlayContext.width = m_swapchainExtent.width;
            overlayContext.height = m_swapchainExtent.height;
            frameDesc.overlay->RecordOverlay(overlayContext);

            vkCmdEndRenderPass(cmd);

            // The present barrier below and vkQueueSubmit's pWaitDstStageMask both read
            // frameOutputLayout/Access/Stage; keep them in sync with what the overlay pass
            // just left the image in, or the present-layout transition will use stale state.
            frameOutputLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            frameOutputAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            frameOutputStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        }

        // Capture before the present transition: after Present the back buffer is undefined, so this
        // is the last point the finished frame -- overlay included -- still exists.
        bool captureRecorded = false;
        if (frameDesc.capture != nullptr && frameDesc.capture->path != nullptr) {
            captureRecorded = RecordBackBufferCapture(cmd, imageIndex, frameOutputLayout,
                                                      frameOutputAccess, frameOutputStage);
            if (!captureRecorded) {
                frameDesc.capture->completed = true;
                frameDesc.capture->succeeded = false;
            }
        }

        VkImageMemoryBarrier toPresent{};
        toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toPresent.srcAccessMask = frameOutputAccess;
        toPresent.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        toPresent.oldLayout = frameOutputLayout;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image = m_swapchainImages[imageIndex];
        toPresent.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toPresent.subresourceRange.baseMipLevel = 0;
        toPresent.subresourceRange.levelCount = 1;
        toPresent.subresourceRange.baseArrayLayer = 0;
        toPresent.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd,
                             frameOutputStage,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &toPresent);

        result = vkEndCommandBuffer(cmd);
        if (result != VK_SUCCESS) {
            return false;
        }

        const VkPipelineStageFlags waitStage = frameOutputStage;
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &m_imageAvailableSemaphores[frame];
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &m_renderFinishedSemaphores[imageIndex];

        vkResetFences(m_device, 1, &m_frameFences[frame]);
        result = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_frameFences[frame]);
        if (result != VK_SUCCESS) {
            return false;
        }

        // The copy is on the GPU timeline; the mapped buffer only holds the frame once the submit
        // that recorded it has completed. Waiting here costs a stall, which is acceptable for a
        // screenshot and is what the other two backends effectively do as well.
        if (captureRecorded) {
            vkWaitForFences(m_device, 1, &m_frameFences[frame], VK_TRUE, UINT64_MAX);
            frameDesc.capture->succeeded = WriteCapturedBackBufferToPng(frameDesc.capture->path);
            frameDesc.capture->completed = true;
        }

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &m_renderFinishedSemaphores[imageIndex];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &m_swapchain;
        presentInfo.pImageIndices = &imageIndex;
        result = vkQueuePresentKHR(m_presentQueue, &presentInfo);
        const bool presentNeedsRecreate = result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR;
        if (result != VK_SUCCESS && !presentNeedsRecreate) {
            return false;
        }

        m_swapchainImageLayouts[imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        m_currentFrame = (m_currentFrame + 1u) % static_cast<UINT>(m_commandBuffers.size());
        if (recreateAfterPresent || presentNeedsRecreate) {
            return ResizeBackendSwapChain(m_swapchainExtent.width, m_swapchainExtent.height);
        }
        return true;
    }

    bool VulkanGraphicsDevice::GetOverlayDeviceContext(RhiOverlayDeviceContext& context)
    {
        if (m_instance == VK_NULL_HANDLE || m_physicalDevice == VK_NULL_HANDLE || m_device == VK_NULL_HANDLE) {
            return false;
        }

        // ImGui's Vulkan backend requires a VkRenderPass at init time (it bakes it into the
        // pipeline it builds internally), so the UI render pass/framebuffers must already exist
        // by the time this context is handed to the UI layer.
        if (!EnsureUIOverlayResources()) {
            return false;
        }

        context.api = RhiBackendApi::Vulkan;
        context.instance = reinterpret_cast<void*>(m_instance);
        context.physicalDevice = reinterpret_cast<void*>(m_physicalDevice);
        context.device = reinterpret_cast<void*>(m_device);
        context.queue = reinterpret_cast<void*>(m_graphicsQueue);
        context.queueFamily = m_graphicsQueueFamily;
        context.renderPass = reinterpret_cast<void*>(m_uiOverlayRenderPass);
        context.imageCount = static_cast<uint32_t>(m_swapchainImages.size());
        // Must match the VkApplicationInfo::apiVersion requested in CreateInstance().
        context.apiVersion = VK_API_VERSION_1_3;
        return true;
    }


    void VulkanGraphicsDevice::DestroyUIOverlayFramebuffers()
    {
        if (m_device != VK_NULL_HANDLE) {
            for (VkFramebuffer framebuffer : m_uiOverlayFramebuffers) {
                if (framebuffer != VK_NULL_HANDLE) {
                    vkDestroyFramebuffer(m_device, framebuffer, nullptr);
                }
            }
        }
        m_uiOverlayFramebuffers.clear();
    }

    void VulkanGraphicsDevice::DestroyUIOverlayResources()
    {
        DestroyUIOverlayFramebuffers();
        if (m_device != VK_NULL_HANDLE && m_uiOverlayRenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(m_device, m_uiOverlayRenderPass, nullptr);
        }
        m_uiOverlayRenderPass = VK_NULL_HANDLE;
        m_uiOverlayRenderPassFormat = VK_FORMAT_UNDEFINED;
    }

    bool VulkanGraphicsDevice::EnsureUIOverlayResources()
    {
        if (m_device == VK_NULL_HANDLE || m_swapchainImageViews.empty()) {
            return false;
        }

        // The render pass is kept alive across resizes: ImGui's Vulkan backend bakes the
        // VkRenderPass it was initialized with into the pipeline it creates internally, so
        // recreating this render pass on every resize would leave that pipeline pointing at a
        // stale handle. Only rebuild it when the swapchain format actually changes.
        if (m_uiOverlayRenderPassFormat != m_swapchainFormat) {
            DestroyUIOverlayResources();

            // Color attachment only, no depth — the overlay draws on top of the already
            // resolved scene, so loadOp is LOAD (never clear what is already there).
            VkAttachmentDescription colorAttachment{};
            colorAttachment.format         = m_swapchainFormat;
            colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
            colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
            colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            VkAttachmentReference colorRef{};
            colorRef.attachment = 0;
            colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments    = &colorRef;

            VkRenderPassCreateInfo renderPassInfo{};
            renderPassInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            renderPassInfo.attachmentCount = 1;
            renderPassInfo.pAttachments    = &colorAttachment;
            renderPassInfo.subpassCount    = 1;
            renderPassInfo.pSubpasses      = &subpass;
            if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_uiOverlayRenderPass) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureUIOverlayResources: vkCreateRenderPass failed.\n");
                return false;
            }
            m_uiOverlayRenderPassFormat = m_swapchainFormat;
        }

        if (m_uiOverlayFramebuffers.size() == m_swapchainImageViews.size()) {
            return true;
        }

        DestroyUIOverlayFramebuffers();

        const size_t imageCount = m_swapchainImageViews.size();
        m_uiOverlayFramebuffers.resize(imageCount, VK_NULL_HANDLE);
        for (size_t i = 0; i < imageCount; ++i) {
            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass      = m_uiOverlayRenderPass;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments    = &m_swapchainImageViews[i];
            framebufferInfo.width           = m_swapchainExtent.width;
            framebufferInfo.height          = m_swapchainExtent.height;
            framebufferInfo.layers          = 1;
            if (vkCreateFramebuffer(m_device, &framebufferInfo, nullptr, &m_uiOverlayFramebuffers[i]) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::EnsureUIOverlayResources: vkCreateFramebuffer failed.\n");
                DestroyUIOverlayFramebuffers();
                return false;
            }
        }
        return true;
    }

    uint32_t VulkanGraphicsDevice::BuildNativeCasterList(const RhiBackendMeshFrameDesc& desc)
    {
        // Resolving handles is the device's job -- it owns m_rhiResources -- so the shadow map and
        // the AO prepass only ever see ready-made VkBuffer handles. Mirrors
        // Dx11GraphicsDeviceMesh.cpp::RenderNativeShadowMap's resolveBuffer.
        const auto resolveBuffer = [this](uint64_t handle) -> VkBuffer {
            if (handle == 0) {
                return VK_NULL_HANDLE;
            }
            const auto it = m_rhiResources.find(handle);
            return it != m_rhiResources.end() ? it->second.buffer : VK_NULL_HANDLE;
        };

        m_nativeShadowCasters.clear();
        m_nativeShadowCasters.reserve(static_cast<size_t>(desc.drawCount) + desc.skinnedDrawCount);

        for (uint32_t i = 0; i < desc.drawCount; ++i) {
            const RhiBackendMeshDrawDesc& draw = desc.draws[i];
            // Transparent geometry is excluded: a cut-out or blended surface casting a solid
            // opaque shadow is worse than it casting none, and the depth-only pass has no alpha.
            if (draw.transparent || (draw.indexCount == 0 && draw.vertexCount == 0)) {
                continue;
            }
            const VkBuffer vertexBuffer = resolveBuffer(draw.vertexBufferHandle);
            if (vertexBuffer == VK_NULL_HANDLE) {
                continue;
            }
            VulkanShadowCaster caster{};
            caster.vertexBuffer = vertexBuffer;
            caster.indexBuffer = resolveBuffer(draw.indexBufferHandle);
            caster.vertexCount = draw.vertexCount;
            caster.indexCount = draw.indexCount;
            caster.index32Bit = draw.index32Bit;
            caster.model = draw.model;
            m_nativeShadowCasters.push_back(caster);
        }

        for (uint32_t i = 0; i < desc.skinnedDrawCount; ++i) {
            const RhiBackendSkinnedMeshDrawDesc& draw = desc.skinnedDraws[i];
            if (draw.transparent || draw.boneMatrices == nullptr ||
                (draw.indexCount == 0 && draw.vertexCount == 0)) {
                continue;
            }
            const VkBuffer vertexBuffer = resolveBuffer(draw.vertexBufferHandle);
            if (vertexBuffer == VK_NULL_HANDLE) {
                continue;
            }
            VulkanShadowCaster caster{};
            caster.vertexBuffer = vertexBuffer;
            caster.indexBuffer = resolveBuffer(draw.indexBufferHandle);
            caster.vertexCount = draw.vertexCount;
            caster.indexCount = draw.indexCount;
            caster.index32Bit = draw.index32Bit;
            caster.model = draw.model;
            caster.boneMatrices = draw.boneMatrices;
            m_nativeShadowCasters.push_back(caster);
        }

        return static_cast<uint32_t>(m_nativeShadowCasters.size());
    }

    bool VulkanGraphicsDevice::RenderNativeShadowMap(VkCommandBuffer cmd, const RhiBackendMeshFrameDesc& desc)
    {
        if (!desc.enableShadow || desc.shadowMapResolution == 0u || !EnsureNativeMeshResources() ||
            !m_nativeShadowMap.IsValid()) {
            return false;
        }

        const uint32_t casterCount = static_cast<uint32_t>(m_nativeShadowCasters.size());
        if (casterCount == 0) {
            return false;
        }

        m_nativeShadowMap.Render(cmd, m_nativeShadowCasters.data(), casterCount, desc.lightViewProjection);
        return true;
    }

    bool VulkanGraphicsDevice::RenderNativeAmbientOcclusion(VkCommandBuffer cmd,
                                                            const RhiBackendMeshFrameDesc& desc)
    {
        if (!desc.enableSsao || !EnsureNativeMeshResources() || !m_nativeAmbientOcclusion.IsValid()) {
            return false;
        }

        const uint32_t occluderCount = static_cast<uint32_t>(m_nativeShadowCasters.size());
        if (occluderCount == 0) {
            return false;
        }

        VulkanAmbientOcclusion::Params params{};
        params.viewProjection = desc.viewProjection;
        params.invViewProjection = desc.invViewProjection;
        params.cameraPos = desc.cameraPos;
        // The live RenderSettings tuning, carried on the frame description since 2026-10-05. It
        // used to be a local copy of those settings' defaults, which made the UI sliders a no-op
        // on this backend.
        params.radius = desc.runtimeAoRadius;
        params.bias = desc.runtimeAoBias;
        params.intensity = desc.runtimeAoIntensity;
        params.thickness = desc.runtimeAoThickness;
        params.quality = desc.runtimeAoQuality;

        return m_nativeAmbientOcclusion.Render(cmd, m_nativeShadowCasters.data(), occluderCount, params);
    }

    bool VulkanGraphicsDevice::RenderMeshFrame(uint32_t,
                                               uint32_t imageIndex,
                                               VkCommandBuffer cmd,
                                               const RhiBackendMeshFrameDesc& desc,
                                               const RhiClearColor& clearColor)
    {
        if (imageIndex >= m_swapchainImages.size() ||
            !EnsureNativeMeshResources()) {
            return false;
        }
        if (imageIndex >= m_nativeMeshFramebuffers.size()) {
            return false;
        }

        VkImageMemoryBarrier toColor{};
        toColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toColor.srcAccessMask = 0;
        toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toColor.oldLayout = m_swapchainImageLayouts[imageIndex];
        toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toColor.image = m_swapchainImages[imageIndex];
        toColor.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toColor.subresourceRange.baseMipLevel = 0;
        toColor.subresourceRange.levelCount = 1;
        toColor.subresourceRange.baseArrayLayer = 0;
        toColor.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &toColor);

        VkClearValue clearValues[2]{};
        clearValues[0].color.float32[0] = clearColor.r;
        clearValues[0].color.float32[1] = clearColor.g;
        clearValues[0].color.float32[2] = clearColor.b;
        clearValues[0].color.float32[3] = clearColor.a;
        clearValues[1].depthStencil.depth = 0.0f;  // Reverse-Z: the far plane is 0.0
        clearValues[1].depthStencil.stencil = 0;

        VkRenderPassBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = m_nativeMeshRenderPass;
        beginInfo.framebuffer = m_nativeMeshFramebuffers[imageIndex];
        beginInfo.renderArea.offset = { 0, 0 };
        beginInfo.renderArea.extent = m_swapchainExtent;
        beginInfo.clearValueCount = static_cast<uint32_t>(std::size(clearValues));
        beginInfo.pClearValues = clearValues;
        vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);

        const float renderW = desc.renderWidth  > 0.0f ? desc.renderWidth  : static_cast<float>(m_swapchainExtent.width);
        const float renderH = desc.renderHeight > 0.0f ? desc.renderHeight : static_cast<float>(m_swapchainExtent.height);
        VkViewport viewport{};
        viewport.x        = 0.0f;
        viewport.y        = renderH;    // Y-flip: start at bottom of image
        viewport.width    = renderW;
        viewport.height   = -renderH;   // negative height flips NDC Y (VK_KHR_maintenance1, core in 1.1)
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.offset = { 0, 0 };
        scissor.extent = {
            static_cast<uint32_t>(renderW),
            static_cast<uint32_t>(renderH),
        };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // Sky first, inside the same render pass: it is the background, so anything drawn after
        // covers it. Its pipeline has depth off entirely, so it neither reads nor writes the depth
        // the meshes are about to use.
        RecordNativeSky(cmd, desc);

        // Only claim the ambient cube is present when its handle actually resolves to a real view;
        // an unresolved/zero handle must render identically to before this existed. Computed once
        // per frame (same value for every draw) rather than per-descriptor-set, since
        // GetOrCreateNativeMeshTextureDescriptorSet folds a failed resolution into the dummy cube
        // without reporting that back to the caller.
        const bool hasAmbientCube = desc.ambientCubeSrv != 0 &&
            m_rhiImageViews.count(desc.ambientCubeSrv) != 0 &&
            m_rhiImageViews.at(desc.ambientCubeSrv) != VK_NULL_HANDLE;

        // Per-frame constants (viewProjection/lightViewProjection/shadowParams) are identical for
        // every draw this frame, so they are written once here into a small UBO instead of being
        // duplicated into every draw's push constant (see VulkanNativeMeshFrameConstants).
        if (m_nativeMeshFrameUboMapped != nullptr) {
            VulkanNativeMeshFrameConstants frameConstants{};
            std::memcpy(frameConstants.viewProjection, desc.viewProjection, sizeof(frameConstants.viewProjection));
            std::memcpy(frameConstants.lightViewProjection, desc.lightViewProjection,
                        sizeof(frameConstants.lightViewProjection));
            frameConstants.shadowParams[0] = desc.shadowDepthBias;
            frameConstants.shadowParams[1] = desc.shadowNormalBias;
            frameConstants.shadowParams[2] = desc.shadowTexelWorld;
            // Resolution doubles as the "is there a shadow map" switch in the shader, so a frame
            // whose shadow pass found no casters (or where shadows are disabled/unallocated) reads
            // as unshadowed instead of sampling garbage. Matches
            // Dx11GraphicsDeviceMesh.cpp::RenderMeshFrame's constants.shadowParams[3] exactly.
            frameConstants.shadowParams[3] = m_nativeShadowMapReadyThisFrame
                ? static_cast<float>(desc.shadowMapResolution)
                : 0.0f;
            // Runtime AO gate + the inverse resolution the shaders divide SV_POSITION by. The gate
            // is the frame's "was the AO texture actually refreshed" flag, not just "is a view
            // bound": a view is always bound (white dummy when unavailable), and without the gate
            // a frame whose AO pass found no geometry would shade against whatever the previous
            // successful frame left in the texture. Same role as shadowParams[3] above.
            //
            // The divisor is this pass's own render size, not the AO texture's extent, even when
            // the two differ: both passes project with the same matrix, so dividing each by its
            // own viewport size yields the same normalised NDC -- which is exactly the UV over the
            // AO image, since the AO chain draws across the whole of it.
            frameConstants.aoParams[0] = m_nativeAmbientOcclusionReadyThisFrame ? 1.0f : 0.0f;
            frameConstants.aoParams[1] = renderW > 0.0f ? 1.0f / renderW : 0.0f;
            frameConstants.aoParams[2] = renderH > 0.0f ? 1.0f / renderH : 0.0f;
            frameConstants.aoParams[3] = desc.exposure; // RenderSettings::exposure
            frameConstants.lightDirIntensity[0] = desc.sunDir[0];
            frameConstants.lightDirIntensity[1] = desc.sunDir[1];
            frameConstants.lightDirIntensity[2] = desc.sunDir[2];
            frameConstants.lightDirIntensity[3] = desc.sunIntensity;
            frameConstants.lightColor[0] = desc.sunColor[0];
            frameConstants.lightColor[1] = desc.sunColor[1];
            frameConstants.lightColor[2] = desc.sunColor[2];
            // "Ambient cube is bound" flag read by NativeMesh*.hlsl.
            frameConstants.lightColor[3] = hasAmbientCube ? 1.0f : 0.0f;
            frameConstants.cameraPosFlags[0] = desc.cameraPos[0];
            frameConstants.cameraPosFlags[1] = desc.cameraPos[1];
            frameConstants.cameraPosFlags[2] = desc.cameraPos[2];
            frameConstants.cameraPosFlags[3] = desc.enableIbl ? 1.0f : 0.0f;
            std::memcpy(m_nativeMeshFrameUboMapped, &frameConstants, sizeof(frameConstants));
        }

        // Shared draw body for both the opaque and transparent static-mesh passes below;
        // only the bound pipeline and the draw order differ between the two.
        const auto drawStaticMesh = [&](const RhiBackendMeshDrawDesc& draw) {
            const auto vbIt = m_rhiResources.find(draw.vertexBufferHandle);
            if (vbIt == m_rhiResources.end() || vbIt->second.buffer == VK_NULL_HANDLE) {
                return;
            }

            // Bind texture descriptor set (create and cache on first use per
            // albedo/ambient-cube/occlusion combination). occlusionSrv's texture slot may hold
            // metallic-roughness instead of AO (see RhiBackendMeshDrawDesc::occlusionSlotHasAo);
            // only bind it as AO when the flag confirms the R channel really is ambient occlusion,
            // otherwise fall back to the dummy (materialAo=1.0, i.e. no change vs. before AO
            // support existed).
            {
                const uint64_t occlusionSrv = draw.occlusionSlotHasAo ? draw.occlusionSrv : 0;
                const VkDescriptorSet bindSet =
                    GetOrCreateNativeMeshTextureDescriptorSet(draw.albedoSrv, desc.ambientCubeSrv, occlusionSrv);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_nativeMeshPipelineLayout, 0, 1, &bindSet, 0, nullptr);
            }

            VulkanNativeMeshPushConstants constants{};
            // viewProjection now lives in the per-frame UBO (set1 binding0, bound once below) --
            // the push constant only needs the per-draw model matrix.
            std::memcpy(constants.model, draw.model, sizeof(constants.model));
            std::memcpy(constants.baseColor, draw.baseColor, sizeof(constants.baseColor));
            constants.emissiveRoughness[0] = draw.emissive[0];
            constants.emissiveRoughness[1] = draw.emissive[1];
            constants.emissiveRoughness[2] = draw.emissive[2];
            constants.emissiveRoughness[3] = draw.roughness;
            constants.materialParams[0] = draw.metallic;
            vkCmdPushConstants(cmd,
                               m_nativeMeshPipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0,
                               sizeof(constants),
                               &constants);

            VkBuffer vertexBuffer = vbIt->second.buffer;
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);

            const auto ibIt = m_rhiResources.find(draw.indexBufferHandle);
            if (draw.indexCount > 0 && ibIt != m_rhiResources.end() && ibIt->second.buffer != VK_NULL_HANDLE) {
                vkCmdBindIndexBuffer(cmd,
                                     ibIt->second.buffer,
                                     0,
                                     draw.index32Bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
                vkCmdDrawIndexed(cmd, draw.indexCount, 1, 0, 0, 0);
            } else if (draw.vertexCount > 0) {
                vkCmdDraw(cmd, draw.vertexCount, 1, 0, 0);
            }
        };

        // Split submitted draws into opaque/transparent buckets. The buckets are members
        // reused every frame (clear() keeps the backing storage) so this split doesn't
        // allocate on the heap each frame.
        m_nativeMeshOpaqueDrawOrder.clear();
        m_nativeMeshTransparentDrawOrder.clear();
        for (uint32_t i = 0; i < desc.drawCount; ++i) {
            if (desc.draws[i].transparent) {
                m_nativeMeshTransparentDrawOrder.push_back(i);
            } else {
                m_nativeMeshOpaqueDrawOrder.push_back(i);
            }
        }

        // Pass 1: opaque, submission order, depth write on.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_nativeMeshPipeline);
        // set1 (the per-frame UBO) is frame-constant, so it is bound once here rather than inside
        // drawStaticMesh; it stays bound across the transparent pipeline switch below because both
        // pipelines share m_nativeMeshPipelineLayout, and drawStaticMesh only ever rebinds set0.
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_nativeMeshPipelineLayout, 1, 1, &m_nativeMeshFrameDescSet, 0, nullptr);
        for (uint32_t index : m_nativeMeshOpaqueDrawOrder) {
            drawStaticMesh(desc.draws[index]);
        }

        // Pass 2: transparent, back-to-front by distance to the camera, depth write off,
        // alpha blended. Skipped entirely (no pipeline bind) when there is nothing to draw.
        if (!m_nativeMeshTransparentDrawOrder.empty()) {
            // This renderer's model matrices are consumed as mul(float4(pos, 1), model)
            // (row-vector convention, shared with the DX11 VS via the same draw.model
            // layout), so world-space translation lives in row 3: model[12], model[13], model[14].
            const auto distanceSquaredToCamera = [&](uint32_t index) {
                const float* m = desc.draws[index].model;
                const float dx = m[12] - desc.cameraPos[0];
                const float dy = m[13] - desc.cameraPos[1];
                const float dz = m[14] - desc.cameraPos[2];
                return dx * dx + dy * dy + dz * dz;
            };
            std::sort(m_nativeMeshTransparentDrawOrder.begin(), m_nativeMeshTransparentDrawOrder.end(),
                      [&](uint32_t a, uint32_t b) {
                          return distanceSquaredToCamera(a) > distanceSquaredToCamera(b);
                      });

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_nativeMeshTransparentPipeline);
            for (uint32_t index : m_nativeMeshTransparentDrawOrder) {
                drawStaticMesh(desc.draws[index]);
            }
        }

        if (desc.skinnedDrawCount > 0 && desc.skinnedDraws && EnsureNativeSkinnedMeshResources()) {
            constexpr uint32_t kMaxSimultaneousSkinnedDraws = 16;
            const uint32_t skinnedDrawCount = std::min<uint32_t>(desc.skinnedDrawCount, kMaxSimultaneousSkinnedDraws);
            if (desc.skinnedDrawCount > kMaxSimultaneousSkinnedDraws) {
                DebugLog("VulkanGraphicsDevice::RenderMeshFrame: skinnedDrawCount exceeds the "
                         "per-frame bone UBO slot budget; extra skinned draws are skipped.\n");
            }

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_nativeSkinnedMeshPipeline);

            for (uint32_t i = 0; i < skinnedDrawCount; ++i) {
                const RhiBackendSkinnedMeshDrawDesc& draw = desc.skinnedDraws[i];
                const auto vbIt = m_rhiResources.find(draw.vertexBufferHandle);
                if (vbIt == m_rhiResources.end() || vbIt->second.buffer == VK_NULL_HANDLE) {
                    continue;
                }

                // Upload this draw's bone palette into its dedicated UBO slot. Command buffers
                // are recorded synchronously here but executed later on the GPU; ExecuteBackendFrame
                // already waits on this frame-in-flight slot's fence before reaching this point, so
                // reusing the same fixed slot index per draw ordinal each frame cannot race a still
                // in-flight read of the same bytes (same pattern as m_nativeRayMarchUboMapped).
                const VkDeviceSize slotOffset = static_cast<VkDeviceSize>(i) * m_nativeSkinnedMeshBoneSlotStride;
                if (draw.boneMatrices && m_nativeSkinnedMeshBoneUboMapped) {
                    std::memcpy(static_cast<uint8_t*>(m_nativeSkinnedMeshBoneUboMapped) + slotOffset,
                                draw.boneMatrices,
                                static_cast<size_t>(Skeleton::kMaxBones) * 16 * sizeof(float));
                }

                // The skinned loader only ever fills this slot from glTF's dedicated occlusion
                // texture, never from metallic-roughness, so its R channel is always real AO;
                // occlusionSlotHasAo then only carries the asset's occlusionStrength (see
                // RhiBackendSkinnedMeshDrawDesc::occlusionSlotHasAo). Falls back to the white
                // dummy so materialAo reads 1.0 and the result matches pre-AO rendering.
                const uint64_t occlusionSrv = draw.occlusionSlotHasAo ? draw.occlusionSrv : 0;
                const VkDescriptorSet texSet =
                    GetOrCreateNativeMeshTextureDescriptorSet(draw.albedoSrv, desc.ambientCubeSrv, occlusionSrv);
                const VkDescriptorSet boundSets[] = { texSet, m_nativeSkinnedMeshBoneDescSet };
                const uint32_t dynamicOffset = static_cast<uint32_t>(slotOffset);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_nativeSkinnedMeshPipelineLayout, 0,
                    static_cast<uint32_t>(std::size(boundSets)), boundSets,
                    1, &dynamicOffset);

                VulkanNativeMeshPushConstants constants{};
                // viewProjection now lives in the per-frame UBO (set1 binding1, part of
                // boundSets[1] above) -- the push constant only needs the per-draw model matrix.
                std::memcpy(constants.model, draw.model, sizeof(constants.model));
                std::memcpy(constants.baseColor, draw.baseColor, sizeof(constants.baseColor));
                constants.emissiveRoughness[0] = draw.emissive[0];
                constants.emissiveRoughness[1] = draw.emissive[1];
                constants.emissiveRoughness[2] = draw.emissive[2];
                constants.emissiveRoughness[3] = draw.roughness;
                constants.materialParams[0] = draw.metallic;
                vkCmdPushConstants(cmd,
                                   m_nativeSkinnedMeshPipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0,
                                   sizeof(constants),
                                   &constants);

                VkBuffer vertexBuffer = vbIt->second.buffer;
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);

                const auto ibIt = m_rhiResources.find(draw.indexBufferHandle);
                if (draw.indexCount > 0 && ibIt != m_rhiResources.end() && ibIt->second.buffer != VK_NULL_HANDLE) {
                    vkCmdBindIndexBuffer(cmd,
                                         ibIt->second.buffer,
                                         0,
                                         draw.index32Bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
                    vkCmdDrawIndexed(cmd, draw.indexCount, 1, 0, 0, 0);
                } else if (draw.vertexCount > 0) {
                    vkCmdDraw(cmd, draw.vertexCount, 1, 0, 0);
                }
            }
        }

        vkCmdEndRenderPass(cmd);
        return true;
    }

    HRESULT VulkanGraphicsDevice::CreateDescriptorHeap(const DescriptorHeapDesc&, DescriptorHeap&)
    {
        return E_NOTIMPL;
    }

    HRESULT VulkanGraphicsDevice::CreateCommittedResource(const HeapProperties*,
                                                          HeapFlags,
                                                          const ResourceDesc*,
                                                          ResourceState,
                                                          const ClearValue*,
                                                          Resource&)
    {
        return E_NOTIMPL;
    }

    HRESULT VulkanGraphicsDevice::CreateCommandAllocator(CommandListType, CommandAllocator&)
    {
        return E_NOTIMPL;
    }

    HRESULT VulkanGraphicsDevice::CreateCommandList(UINT, CommandListType, CommandAllocator&, PipelineState*, CommandList&)
    {
        return E_NOTIMPL;
    }

    HRESULT VulkanGraphicsDevice::CreateGraphicsPipelineState(const GraphicsPipelineDesc&, PipelineState&)
    {
        return E_NOTIMPL;
    }

    HRESULT VulkanGraphicsDevice::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC&, PipelineState&)
    {
        return E_NOTIMPL;
    }

    HRESULT VulkanGraphicsDevice::CreatePipelineStateFromStream(const void*, size_t, PipelineState&)
    {
        return E_NOTIMPL;
    }

    HRESULT VulkanGraphicsDevice::CreateRootSignature(UINT, const void*, size_t, RootSignature&)
    {
        return E_NOTIMPL;
    }

    void VulkanGraphicsDevice::CreateShaderResourceView(Resource&, const ShaderResourceViewDesc*, CpuDescriptorHandle)
    {
    }

    void VulkanGraphicsDevice::CreateDepthStencilView(Resource&, const DepthStencilViewDesc*, CpuDescriptorHandle)
    {
    }

    void VulkanGraphicsDevice::CreateRenderTargetView(Resource&, const D3D12_RENDER_TARGET_VIEW_DESC*, CpuDescriptorHandle)
    {
    }

    void VulkanGraphicsDevice::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC*, CpuDescriptorHandle)
    {
    }

    void VulkanGraphicsDevice::CreateSampler(const D3D12_SAMPLER_DESC*, CpuDescriptorHandle)
    {
    }

    HRESULT VulkanGraphicsDevice::CreateFence(UINT64, D3D12_FENCE_FLAGS, ID3D12Fence**)
    {
        return E_NOTIMPL;
    }

} // namespace SasamiEngine::Render
#endif
