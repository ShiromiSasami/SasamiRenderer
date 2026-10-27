#include "UIRenderBackendVulkan.h"

#if RHI_VULKAN

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"

#include <vulkan/vulkan.h>

namespace SasamiEngine::App::UI
{
    bool UIRenderBackendVulkan::Initialize(const UIRenderBackendInitDesc& desc)
    {
        const auto& native = desc.native;
        if (native.instance == nullptr || native.physicalDevice == nullptr || native.device == nullptr ||
            native.queue == nullptr || native.renderPass == nullptr) {
            return false;
        }

        // Must be zero-cleared before use -- ImGui_ImplVulkan_InitInfo carries optional fields
        // (dynamic rendering, allocator callbacks, ...) that need to default to inactive.
        ImGui_ImplVulkan_InitInfo info = {};
        info.ApiVersion = native.apiVersion;
        info.Instance = reinterpret_cast<VkInstance>(native.instance);
        info.PhysicalDevice = reinterpret_cast<VkPhysicalDevice>(native.physicalDevice);
        info.Device = reinterpret_cast<VkDevice>(native.device);
        info.QueueFamily = native.queueFamily;
        info.Queue = reinterpret_cast<VkQueue>(native.queue);
        info.RenderPass = reinterpret_cast<VkRenderPass>(native.renderPass);

        // Let ImGui create its own descriptor pool rather than sharing the renderer's general-purpose
        // m_rhiDescriptorPool -- keeps UI descriptor lifetime and exhaustion isolated from the rest of
        // the renderer.
        info.DescriptorPool = VK_NULL_HANDLE;
        info.DescriptorPoolSize = 64;

        // Vulkan requires MinImageCount >= 2.
        info.MinImageCount = native.imageCount < 2 ? 2 : native.imageCount;
        info.ImageCount = native.imageCount < 2 ? 2 : native.imageCount;
        info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.PipelineCache = VK_NULL_HANDLE;
        info.Subpass = 0;

        // This Vulkan backend never calls vkCmdBeginRendering -- m_hasVkKhrDynamicRendering only
        // records whether the extension is present, it is not used to drive rendering -- so dynamic
        // rendering stays disabled here.
        info.UseDynamicRendering = false;

        info.Allocator = nullptr;
        info.CheckVkResultFn = nullptr;

        m_initialized = ImGui_ImplVulkan_Init(&info);
        return m_initialized;
    }

    void UIRenderBackendVulkan::Shutdown()
    {
        // ImGui asserts when its backend entry points run without backend data, so every one of
        // these is gated on a successful Initialize rather than on the caller remembering.
        if (!m_initialized) return;
        ImGui_ImplVulkan_Shutdown();
        m_initialized = false;
    }

    void UIRenderBackendVulkan::NewFrame()
    {
        if (!m_initialized) return;
        ImGui_ImplVulkan_NewFrame();
    }

    void UIRenderBackendVulkan::RenderDrawData(const Render::RhiOverlayFrameContext& context)
    {
        if (!m_initialized) return;
        if (context.commandBuffer == nullptr) return;

        // The caller has already begun the UI render pass on this command buffer; ImGui only
        // records draw commands into it here.
        auto cmd = reinterpret_cast<VkCommandBuffer>(context.commandBuffer);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    }

    bool UIRenderBackendVulkan::IsInitialized() const
    {
        return m_initialized;
    }
}

#endif // RHI_VULKAN
