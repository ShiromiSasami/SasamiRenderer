#pragma once

#include "Renderer/RHI/RhiTypes.h"

#include <cstdint>

namespace SasamiEngine::Render
{
    // Everything the UI layer needs to stand up an ImGui renderer backend for the running device.
    //
    // Native objects travel as void* so this header stays free of vulkan.h / d3d11.h -- the same
    // trade the RHI already makes for IRHIDevice::GetNativeDeviceHandle(). Each backend fills only
    // the fields its API has; the reader keys off `api` to know which those are.
    struct RhiOverlayDeviceContext
    {
        RhiBackendApi api = RhiBackendApi::Unknown;

        // Vulkan: VkInstance / VkPhysicalDevice / VkDevice / VkQueue / VkRenderPass.
        // DirectX 11: device = ID3D11Device*, queue = ID3D11DeviceContext*.
        // OpenGL: none of these -- the ImGui backend loads its own entry points.
        void* instance = nullptr;
        void* physicalDevice = nullptr;
        void* device = nullptr;
        void* queue = nullptr;
        void* renderPass = nullptr;

        uint32_t queueFamily = 0;
        uint32_t imageCount = 0;
        uint32_t apiVersion = 0;
    };

    // Where in the backend's own frame the overlay is being recorded.
    struct RhiOverlayFrameContext
    {
        RhiBackendApi api = RhiBackendApi::Unknown;

        // Vulkan: VkCommandBuffer. DirectX 12: ID3D12GraphicsCommandList*.
        // DirectX 11 / OpenGL: null -- those backends record through their immediate context.
        void* commandBuffer = nullptr;

        // DirectX 12 only: the D3D12_CPU_DESCRIPTOR_HANDLE of the target RTV.
        void* renderTarget = nullptr;

        uint32_t imageIndex = 0;
        uint32_t frameIndex = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    // Implemented by the UI layer, called by the backend from inside its own frame.
    //
    // The non-D3D12 backends run scene submission and Present inside one ExecuteBackendFrame call,
    // so an overlay cannot be handed a command list from the outside the way the D3D12 path does.
    // The frame calls back instead, at the point where the swap-chain image is ready to be drawn on.
    class IRhiFrameOverlay
    {
    public:
        virtual ~IRhiFrameOverlay() = default;

        virtual void RecordOverlay(const RhiOverlayFrameContext& context) = 0;
    };
}

namespace SasamiEngine
{
    using namespace Render;
}
