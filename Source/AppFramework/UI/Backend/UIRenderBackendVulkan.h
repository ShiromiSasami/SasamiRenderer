#pragma once

#include "IUIRenderBackend.h"

#if RHI_VULKAN

namespace SasamiEngine::App::UI
{
    // ImGui renderer backend for the Vulkan build. Records into the command buffer the caller
    // hands in through RhiOverlayFrameContext, inside a UI render pass the caller has already begun.
    class UIRenderBackendVulkan final : public IUIRenderBackend
    {
    public:
        bool Initialize(const UIRenderBackendInitDesc& desc) override;
        void Shutdown() override;
        void NewFrame() override;
        void RenderDrawData(const Render::RhiOverlayFrameContext& context) override;
        bool IsInitialized() const override;

    private:
        bool m_initialized = false;
    };
}

#endif // RHI_VULKAN
