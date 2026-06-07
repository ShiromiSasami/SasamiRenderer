// VulkanGraphicsDevice.cpp
// Vulkan ExecuteBackendFrame, D3D12 compatibility wrappers.
#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"
#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice_Utils.h"

#if RHI_VULKAN

#include "Foundation/Tools/DebugOutput.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>


namespace SasamiRenderer
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
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
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

        VkImageMemoryBarrier toPresent = toTransfer;
        toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toPresent.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
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

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &m_imageAvailableSemaphores[frame];
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &m_renderFinishedSemaphores[frame];

        vkResetFences(m_device, 1, &m_frameFences[frame]);
        result = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, m_frameFences[frame]);
        if (result != VK_SUCCESS) {
            return false;
        }

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &m_renderFinishedSemaphores[frame];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &m_swapchain;
        presentInfo.pImageIndices = &imageIndex;
        result = vkQueuePresentKHR(m_presentQueue, &presentInfo);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            return false;
        }

        m_swapchainImageLayouts[imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        m_currentFrame = (m_currentFrame + 1u) % static_cast<UINT>(m_commandBuffers.size());
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

    bool VulkanGraphicsDevice::CreateInstance()
    {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "SasamiRenderer";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "SasamiRenderer";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_3;

        const std::array<const char*, 2> extensions = {
            VK_KHR_SURFACE_EXTENSION_NAME,
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        };

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        const VkResult result = vkCreateInstance(&createInfo, nullptr, &m_instance);
        if (result != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateInstance: vkCreateInstance failed.\n");
            return false;
        }
        return true;
    }

    bool VulkanGraphicsDevice::CreateSurface(HWND hWnd)
    {
        VkWin32SurfaceCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        createInfo.hinstance = GetModuleHandle(nullptr);
        createInfo.hwnd = hWnd;

        const VkResult result = vkCreateWin32SurfaceKHR(m_instance, &createInfo, nullptr, &m_surface);
        if (result != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateSurface: vkCreateWin32SurfaceKHR failed.\n");
            return false;
        }
        return true;
    }

    bool VulkanGraphicsDevice::PickPhysicalDevice()
    {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            DebugLog("VulkanGraphicsDevice::PickPhysicalDevice: no Vulkan physical devices found.\n");
            return false;
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

        for (VkPhysicalDevice device : devices) {
            const std::vector<VkExtensionProperties> extensions = EnumerateDeviceExtensions(device);
            if (!HasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                continue;
            }

            uint32_t queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
            std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

            bool foundGraphics = false;
            bool foundPresent = false;
            bool foundCompute = false;
            for (uint32_t i = 0; i < queueFamilyCount; ++i) {
                if (!foundGraphics && (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                    m_graphicsQueueFamily = i;
                    foundGraphics = true;
                }

                VkBool32 presentSupported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &presentSupported);
                if (!foundPresent && presentSupported) {
                    m_presentQueueFamily = i;
                    foundPresent = true;
                }

                if ((queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                    !(queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                    m_computeQueueFamily = i;
                    m_hasDedicatedComputeQueue = true;
                    foundCompute = true;
                }
            }

            if (!foundCompute) {
                for (uint32_t i = 0; i < queueFamilyCount; ++i) {
                    if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                        m_computeQueueFamily = i;
                        foundCompute = true;
                        break;
                    }
                }
            }

            if (foundGraphics && foundPresent && foundCompute) {
                m_physicalDevice = device;
                return true;
            }
        }

        DebugLog("VulkanGraphicsDevice::PickPhysicalDevice: no suitable Vulkan physical device found.\n");
        return false;
    }

    bool VulkanGraphicsDevice::CreateDevice()
    {
        std::vector<uint32_t> uniqueFamilies = { m_graphicsQueueFamily, m_presentQueueFamily, m_computeQueueFamily };
        std::sort(uniqueFamilies.begin(), uniqueFamilies.end());
        uniqueFamilies.erase(std::unique(uniqueFamilies.begin(), uniqueFamilies.end()), uniqueFamilies.end());

        const float priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queues;
        queues.reserve(uniqueFamilies.size());
        for (uint32_t family : uniqueFamilies) {
            VkDeviceQueueCreateInfo queueInfo{};
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = family;
            queueInfo.queueCount = 1;
            queueInfo.pQueuePriorities = &priority;
            queues.push_back(queueInfo);
        }

        const std::array<const char*, 1> extensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        };

        VkPhysicalDeviceFeatures features{};
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queues.size());
        createInfo.pQueueCreateInfos = queues.data();
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.pEnabledFeatures = &features;

        const VkResult result = vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device);
        if (result != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateDevice: vkCreateDevice failed.\n");
            return false;
        }

        vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
        vkGetDeviceQueue(m_device, m_computeQueueFamily, 0, &m_computeQueue);
        vkGetDeviceQueue(m_device, m_presentQueueFamily, 0, &m_presentQueue);
        return true;
    }

    bool VulkanGraphicsDevice::CreateSwapChain(UINT width, UINT height, UINT bufferCount)
    {
        VkSurfaceCapabilitiesKHR surfaceCaps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physicalDevice, m_surface, &surfaceCaps) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateSwapChain: failed to query surface capabilities.\n");
            return false;
        }

        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(m_physicalDevice, m_surface, &formatCount, formats.data());

        VkSurfaceFormatKHR selectedFormat = formats.empty()
            ? VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR }
            : formats[0];
        for (const VkSurfaceFormatKHR& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM ||
                format.format == VK_FORMAT_R8G8B8A8_UNORM) {
                selectedFormat = format;
                break;
            }
        }

        uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, nullptr);
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, presentModes.data());
        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
        for (VkPresentModeKHR mode : presentModes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                presentMode = mode;
                break;
            }
        }

        VkExtent2D extent{};
        if (surfaceCaps.currentExtent.width != UINT32_MAX) {
            extent = surfaceCaps.currentExtent;
        } else {
            extent.width = std::clamp<uint32_t>(width, surfaceCaps.minImageExtent.width, surfaceCaps.maxImageExtent.width);
            extent.height = std::clamp<uint32_t>(height, surfaceCaps.minImageExtent.height, surfaceCaps.maxImageExtent.height);
        }

        uint32_t imageCount = std::max<uint32_t>(bufferCount, surfaceCaps.minImageCount);
        if (surfaceCaps.maxImageCount > 0) {
            imageCount = std::min<uint32_t>(imageCount, surfaceCaps.maxImageCount);
        }

        uint32_t queueFamilyIndices[] = { m_graphicsQueueFamily, m_presentQueueFamily };
        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = m_surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = selectedFormat.format;
        createInfo.imageColorSpace = selectedFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        createInfo.imageSharingMode = (m_graphicsQueueFamily != m_presentQueueFamily)
            ? VK_SHARING_MODE_CONCURRENT
            : VK_SHARING_MODE_EXCLUSIVE;
        createInfo.queueFamilyIndexCount = (createInfo.imageSharingMode == VK_SHARING_MODE_CONCURRENT) ? 2u : 0u;
        createInfo.pQueueFamilyIndices = (createInfo.imageSharingMode == VK_SHARING_MODE_CONCURRENT) ? queueFamilyIndices : nullptr;
        createInfo.preTransform = surfaceCaps.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        const VkResult result = vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &m_swapchain);
        if (result != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateSwapChain: vkCreateSwapchainKHR failed.\n");
            return false;
        }

        uint32_t actualImageCount = 0;
        vkGetSwapchainImagesKHR(m_device, m_swapchain, &actualImageCount, nullptr);
        m_swapchainImages.resize(actualImageCount);
        if (actualImageCount > 0) {
            vkGetSwapchainImagesKHR(m_device, m_swapchain, &actualImageCount, m_swapchainImages.data());
        }
        m_swapchainImageLayouts.assign(actualImageCount, VK_IMAGE_LAYOUT_UNDEFINED);
        return true;
    }

    bool VulkanGraphicsDevice::CreateFrameResources(UINT bufferCount)
    {
        const uint32_t frameCount = std::max<uint32_t>(1u, bufferCount);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
        if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateFrameResources: vkCreateCommandPool failed.\n");
            return false;
        }

        m_commandBuffers.resize(frameCount);
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = frameCount;
        if (vkAllocateCommandBuffers(m_device, &allocInfo, m_commandBuffers.data()) != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateFrameResources: vkAllocateCommandBuffers failed.\n");
            return false;
        }

        m_imageAvailableSemaphores.resize(frameCount);
        m_renderFinishedSemaphores.resize(frameCount);
        m_frameFences.resize(frameCount);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (uint32_t i = 0; i < frameCount; ++i) {
            if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_imageAvailableSemaphores[i]) != VK_SUCCESS ||
                vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &m_renderFinishedSemaphores[i]) != VK_SUCCESS ||
                vkCreateFence(m_device, &fenceInfo, nullptr, &m_frameFences[i]) != VK_SUCCESS) {
                DebugLog("VulkanGraphicsDevice::CreateFrameResources: sync object creation failed.\n");
                return false;
            }
        }

        m_currentFrame = 0;
        return true;
    }

    void VulkanGraphicsDevice::QueryCapabilities()
    {
        m_capabilities = {};
        m_capabilities.api = RhiBackendApi::Vulkan;
        m_capabilities.supportsGraphicsQueue = true;
        m_capabilities.supportsComputeQueue = (m_computeQueue != VK_NULL_HANDLE);
        m_capabilities.supportsSwapChain = (m_swapchain != VK_NULL_HANDLE);
        m_capabilities.supportsNativeFrame = true;
        m_capabilities.supportsFeatureRenderPasses = false;
        m_capabilities.supportsD3D12CompatibilitySurface = false;
        m_capabilities.supportsRhiResourceCreation = true;
        m_capabilities.supportsRhiDescriptorCreation = true;
        m_capabilities.supportsRhiPipelineCreation = true;
        m_capabilities.supportsRhiCommandEncoding = true;
        m_capabilities.supportsDynamicRenderPass = true;
        m_capabilities.supportsVulkanDynamicRendering = true;

        const std::vector<VkExtensionProperties> extensions = EnumerateDeviceExtensions(m_physicalDevice);
        m_capabilities.supportsRayQuery = HasExtension(extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME);
        m_capabilities.supportsRayTracingPipeline = HasExtension(extensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        m_capabilities.supportsHardwareRayTracing =
            m_capabilities.supportsRayQuery || m_capabilities.supportsRayTracingPipeline;
        m_capabilities.supportsDescriptorIndexing = HasExtension(extensions, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        m_capabilities.supportsTimelineSemaphore = HasExtension(extensions, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        m_capabilities.supportsMeshShaders = HasExtension(extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME);
    }

    uint32_t VulkanGraphicsDevice::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const
    {
        if (m_physicalDevice == VK_NULL_HANDLE) {
            return UINT32_MAX;
        }

        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
            const bool typeMatches = (typeBits & (1u << i)) != 0;
            const bool propertiesMatch =
                (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties;
            if (typeMatches && propertiesMatch) {
                return i;
            }
        }
        return UINT32_MAX;
    }

    void VulkanGraphicsDevice::DestroySwapChain()
    {
        m_swapchainImages.clear();
        m_swapchainImageLayouts.clear();
        if (m_device != VK_NULL_HANDLE && m_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
            m_swapchain = VK_NULL_HANDLE;
        }
    }

    void VulkanGraphicsDevice::DestroyFrameResources()
    {
        if (m_device == VK_NULL_HANDLE) {
            m_commandBuffers.clear();
            m_imageAvailableSemaphores.clear();
            m_renderFinishedSemaphores.clear();
            m_frameFences.clear();
            m_commandPool = VK_NULL_HANDLE;
            return;
        }

        for (VkFence fence : m_frameFences) {
            if (fence != VK_NULL_HANDLE) {
                vkDestroyFence(m_device, fence, nullptr);
            }
        }
        for (VkSemaphore semaphore : m_renderFinishedSemaphores) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(m_device, semaphore, nullptr);
            }
        }
        for (VkSemaphore semaphore : m_imageAvailableSemaphores) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(m_device, semaphore, nullptr);
            }
        }
        if (m_commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device, m_commandPool, nullptr);
            m_commandPool = VK_NULL_HANDLE;
        }

        m_commandBuffers.clear();
        m_imageAvailableSemaphores.clear();
        m_renderFinishedSemaphores.clear();
        m_frameFences.clear();
        m_currentFrame = 0;
    }

    void VulkanGraphicsDevice::Cleanup()
    {
        if (m_device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(m_device);
            for (auto& entry : m_rhiPipelines) {
                VulkanRhiPipeline& pipeline = entry.second;
                if (pipeline.pipeline != VK_NULL_HANDLE) {
                    vkDestroyPipeline(m_device, pipeline.pipeline, nullptr);
                }
                if (pipeline.renderPass != VK_NULL_HANDLE) {
                    vkDestroyRenderPass(m_device, pipeline.renderPass, nullptr);
                }
                if (pipeline.ownedPipelineLayout != VK_NULL_HANDLE) {
                    vkDestroyPipelineLayout(m_device, pipeline.ownedPipelineLayout, nullptr);
                }
            }
            for (auto& entry : m_rhiPipelineLayouts) {
                VulkanRhiPipelineLayout& layout = entry.second;
                if (layout.pipelineLayout != VK_NULL_HANDLE) {
                    vkDestroyPipelineLayout(m_device, layout.pipelineLayout, nullptr);
                }
                if (layout.descriptorSetLayout != VK_NULL_HANDLE) {
                    vkDestroyDescriptorSetLayout(m_device, layout.descriptorSetLayout, nullptr);
                }
            }
            for (auto& entry : m_rhiShaders) {
                if (entry.second.module != VK_NULL_HANDLE) {
                    vkDestroyShaderModule(m_device, entry.second.module, nullptr);
                }
            }
            for (auto& entry : m_rhiImageViews) {
                if (entry.second != VK_NULL_HANDLE) {
                    vkDestroyImageView(m_device, entry.second, nullptr);
                }
            }
            for (auto& entry : m_rhiResources) {
                VulkanRhiResource& resource = entry.second;
                if (resource.image != VK_NULL_HANDLE) {
                    vkDestroyImage(m_device, resource.image, nullptr);
                }
                if (resource.buffer != VK_NULL_HANDLE) {
                    vkDestroyBuffer(m_device, resource.buffer, nullptr);
                }
                if (resource.memory != VK_NULL_HANDLE) {
                    vkFreeMemory(m_device, resource.memory, nullptr);
                }
            }
        }
        m_rhiResources.clear();
        m_rhiShaders.clear();
        m_rhiPipelineLayouts.clear();
        m_rhiPipelines.clear();
        m_rhiImageViews.clear();
        m_nextRhiResourceHandle = 1;
        m_nextRhiDescriptorHandle = 1;
        m_nextRhiShaderHandle = 1;
        m_nextRhiPipelineLayoutHandle = 1;
        m_nextRhiPipelineHandle = 1;
        DestroyFrameResources();
        DestroySwapChain();
        if (m_device != VK_NULL_HANDLE) {
            vkDestroyDevice(m_device, nullptr);
            m_device = VK_NULL_HANDLE;
        }
        if (m_surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
            m_surface = VK_NULL_HANDLE;
        }
        if (m_instance != VK_NULL_HANDLE) {
            vkDestroyInstance(m_instance, nullptr);
            m_instance = VK_NULL_HANDLE;
        }
        m_physicalDevice = VK_NULL_HANDLE;
        m_graphicsQueue = VK_NULL_HANDLE;
        m_computeQueue = VK_NULL_HANDLE;
        m_presentQueue = VK_NULL_HANDLE;
        m_capabilities = {};
        m_hasDedicatedComputeQueue = false;
    }
} // namespace SasamiRenderer
#endif
