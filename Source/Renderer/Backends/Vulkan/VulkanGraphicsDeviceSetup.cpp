// VulkanGraphicsDeviceSetup.cpp
// Bringing the Vulkan device up and tearing it down: instance, surface, physical-device
// selection, logical device, swap chain, per-frame resources and the capability query.
//
// Separated from the drawing paths because none of it runs per frame -- it is the part that
// answers "what hardware is this and what can it do", and it is where every extension decision
// is made.
#include "Renderer/Backends/Vulkan/VulkanGraphicsDevice.h"
#include "Renderer/Backends/Vulkan/VulkanGraphicsDeviceUtils.h"

#if RHI_VULKAN

#include "Foundation/Tools/DebugOutput.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace SasamiEngine::Render
{
    bool VulkanGraphicsDevice::CreateInstance()
    {
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "SasamiEngine";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "SasamiEngine";
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

                // Detect optional extensions for capabilities
                m_hasVkKhrDynamicRendering   = HasExtension(extensions, "VK_KHR_dynamic_rendering");
                m_hasVkExtDescriptorIndexing = HasExtension(extensions, "VK_EXT_descriptor_indexing");
                m_hasVkKhrTimelineSemaphore  = HasExtension(extensions, "VK_KHR_timeline_semaphore");
                m_hasVkKhrBufferDeviceAddress = HasExtension(extensions, "VK_KHR_buffer_device_address");
                m_hasVkKhrDeferredHostOps    = HasExtension(extensions, "VK_KHR_deferred_host_operations");
                m_hasVkKhrAccelerationStructure = HasExtension(extensions, "VK_KHR_acceleration_structure")
                    && m_hasVkKhrBufferDeviceAddress
                    && m_hasVkExtDescriptorIndexing
                    && m_hasVkKhrDeferredHostOps;
                m_hasVkKhrRayTracingPipeline = m_hasVkKhrAccelerationStructure
                    && HasExtension(extensions, "VK_KHR_ray_tracing_pipeline")
                    && HasExtension(extensions, "VK_KHR_spirv_1_4");
                m_hasVkKhrRayQuery           = m_hasVkKhrAccelerationStructure
                    && HasExtension(extensions, "VK_KHR_ray_query");

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

        std::vector<const char*> extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

        if (m_hasVkKhrDynamicRendering)      extensions.push_back("VK_KHR_dynamic_rendering");
        if (m_hasVkExtDescriptorIndexing)    extensions.push_back("VK_EXT_descriptor_indexing");
        if (m_hasVkKhrTimelineSemaphore)     extensions.push_back("VK_KHR_timeline_semaphore");
        if (m_hasVkKhrBufferDeviceAddress)   extensions.push_back("VK_KHR_buffer_device_address");
        if (m_hasVkKhrDeferredHostOps)       extensions.push_back("VK_KHR_deferred_host_operations");
        if (m_hasVkKhrAccelerationStructure) extensions.push_back("VK_KHR_acceleration_structure");
        if (m_hasVkKhrRayTracingPipeline) {
            extensions.push_back("VK_KHR_ray_tracing_pipeline");
            extensions.push_back("VK_KHR_spirv_1_4");
        }
        if (m_hasVkKhrRayQuery) extensions.push_back("VK_KHR_ray_query");

        // Build pNext chain for optional device features
        void* featureChainHead = nullptr;

        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
        if (m_hasVkKhrRayQuery) {
            rayQueryFeatures.sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
            rayQueryFeatures.rayQuery = VK_TRUE;
            rayQueryFeatures.pNext    = featureChainHead;
            featureChainHead          = &rayQueryFeatures;
        }

        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures{};
        if (m_hasVkKhrRayTracingPipeline) {
            rtPipelineFeatures.sType              = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
            rtPipelineFeatures.rayTracingPipeline = VK_TRUE;
            rtPipelineFeatures.pNext              = featureChainHead;
            featureChainHead                      = &rtPipelineFeatures;
        }

        VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
        if (m_hasVkKhrAccelerationStructure) {
            asFeatures.sType                 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
            asFeatures.accelerationStructure = VK_TRUE;
            asFeatures.pNext                 = featureChainHead;
            featureChainHead                 = &asFeatures;
        }

        VkPhysicalDeviceBufferDeviceAddressFeaturesKHR bufAddrFeatures{};
        if (m_hasVkKhrBufferDeviceAddress) {
            bufAddrFeatures.sType               = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR;
            bufAddrFeatures.bufferDeviceAddress = VK_TRUE;
            bufAddrFeatures.pNext               = featureChainHead;
            featureChainHead                    = &bufAddrFeatures;
        }

        VkPhysicalDeviceFeatures features{};
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = featureChainHead;
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

        // Load RT function pointers if extensions are available
        if (m_hasVkKhrBufferDeviceAddress) {
            m_pfnGetBufAddr = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(
                vkGetDeviceProcAddr(m_device, "vkGetBufferDeviceAddressKHR"));
        }
        if (m_hasVkKhrAccelerationStructure) {
            m_pfnGetAsBuildSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
                vkGetDeviceProcAddr(m_device, "vkGetAccelerationStructureBuildSizesKHR"));
            m_pfnCreateAs = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
                vkGetDeviceProcAddr(m_device, "vkCreateAccelerationStructureKHR"));
            m_pfnDestroyAs = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
                vkGetDeviceProcAddr(m_device, "vkDestroyAccelerationStructureKHR"));
            m_pfnCmdBuildAs = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
                vkGetDeviceProcAddr(m_device, "vkCmdBuildAccelerationStructuresKHR"));
            m_pfnGetAsAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
                vkGetDeviceProcAddr(m_device, "vkGetAccelerationStructureDeviceAddressKHR"));
        }
        if (m_hasVkKhrRayTracingPipeline) {
            m_pfnCreateRtPipeline = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
                vkGetDeviceProcAddr(m_device, "vkCreateRayTracingPipelinesKHR"));
            m_pfnGetRtHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
                vkGetDeviceProcAddr(m_device, "vkGetRayTracingShaderGroupHandlesKHR"));
            m_pfnCmdTraceRays = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(
                vkGetDeviceProcAddr(m_device, "vkCmdTraceRaysKHR"));

            // Query ray-tracing pipeline properties (SBT handle sizes / alignments)
            // needed to lay out the shader binding table correctly.
            VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
            rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
            VkPhysicalDeviceProperties2 props2{};
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            props2.pNext = &rtProps;
            vkGetPhysicalDeviceProperties2(m_physicalDevice, &props2);
            m_rtShaderGroupHandleSize      = rtProps.shaderGroupHandleSize;
            m_rtShaderGroupBaseAlignment   = rtProps.shaderGroupBaseAlignment;
            m_rtShaderGroupHandleAlignment = rtProps.shaderGroupHandleAlignment;
            m_rtMaxRayRecursionDepth       = rtProps.maxRayRecursionDepth;
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
        if (extent.width == 0 || extent.height == 0) {
            return false;
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
        createInfo.oldSwapchain = m_swapchain;

        VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
        const VkResult result = vkCreateSwapchainKHR(m_device, &createInfo, nullptr, &newSwapchain);
        if (result != VK_SUCCESS) {
            DebugLog("VulkanGraphicsDevice::CreateSwapChain: vkCreateSwapchainKHR failed.\n");
            return false;
        }

        uint32_t actualImageCount = 0;
        if (vkGetSwapchainImagesKHR(m_device, newSwapchain, &actualImageCount, nullptr) != VK_SUCCESS ||
            actualImageCount == 0) {
            vkDestroySwapchainKHR(m_device, newSwapchain, nullptr);
            return false;
        }
        std::vector<VkImage> newImages(actualImageCount);
        if (vkGetSwapchainImagesKHR(m_device, newSwapchain, &actualImageCount, newImages.data()) != VK_SUCCESS) {
            vkDestroySwapchainKHR(m_device, newSwapchain, nullptr);
            return false;
        }

        std::vector<VkImageView> newImageViews(actualImageCount, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < actualImageCount; ++i) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = newImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = selectedFormat.format;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;
            if (vkCreateImageView(m_device, &viewInfo, nullptr, &newImageViews[i]) != VK_SUCCESS) {
                for (VkImageView imageView : newImageViews) {
                    if (imageView != VK_NULL_HANDLE) {
                        vkDestroyImageView(m_device, imageView, nullptr);
                    }
                }
                vkDestroySwapchainKHR(m_device, newSwapchain, nullptr);
                return false;
            }
        }

        DestroyNativeMeshResources();
        DestroyNativeSkyResources();
        DestroyNativeSkinnedMeshResources();
        DestroyRayMarchResources();
        // The UI overlay framebuffers reference the old m_swapchainImageViews, which are
        // destroyed right below; the render pass itself is left alone (see EnsureUIOverlayResources).
        DestroyUIOverlayFramebuffers();
        for (VkImageView imageView : m_swapchainImageViews) {
            if (imageView != VK_NULL_HANDLE) {
                vkDestroyImageView(m_device, imageView, nullptr);
            }
        }
        if (m_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
        }

        m_swapchain = newSwapchain;
        m_swapchainFormat = selectedFormat.format;
        m_swapchainExtent = extent;
        m_swapchainImages = std::move(newImages);
        m_swapchainImageViews = std::move(newImageViews);
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

        // Optional Vulkan features — set from detected extensions
        m_capabilities.supportsVulkanDynamicRendering = m_hasVkKhrDynamicRendering;
        m_capabilities.supportsDynamicRenderPass      = m_hasVkKhrDynamicRendering;
        m_capabilities.supportsDescriptorIndexing     = m_hasVkExtDescriptorIndexing;
        m_capabilities.supportsTimelineSemaphore      = m_hasVkKhrTimelineSemaphore;
        m_capabilities.supportsHardwareRayTracing     = m_hasVkKhrAccelerationStructure;
        m_capabilities.supportsRayTracingPipeline     = m_hasVkKhrRayTracingPipeline;
        m_capabilities.supportsRayQuery               = m_hasVkKhrRayQuery;
        m_capabilities.supportsMeshShaders = false;
        // Device-level creates are allowed concurrently in Vulkan; per-object external sync applies.
        m_capabilities.supportsThreadedResourceCreation = true;
        m_capabilities.supportsThreadedPipelineCreation = true;
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
        DestroyNativeMeshResources();
        DestroyNativeSkyResources();
        DestroyNativeSkinnedMeshResources();
        DestroyRayMarchResources();
        DestroyUIOverlayResources();
        if (m_device != VK_NULL_HANDLE) {
            for (VkImageView imageView : m_swapchainImageViews) {
                if (imageView != VK_NULL_HANDLE) {
                    vkDestroyImageView(m_device, imageView, nullptr);
                }
            }
        }
        m_swapchainImageViews.clear();
        m_swapchainImages.clear();
        m_swapchainImageLayouts.clear();
        m_swapchainFormat = VK_FORMAT_UNDEFINED;
        m_swapchainExtent = {};
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
            if (m_rhiDescriptorPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(m_device, m_rhiDescriptorPool, nullptr);
                m_rhiDescriptorPool = VK_NULL_HANDLE;
            }
            for (auto& entry : m_rhiPipelineLayouts) {
                VulkanRhiPipelineLayout& layout = entry.second;
                if (layout.pipelineLayout != VK_NULL_HANDLE) {
                    vkDestroyPipelineLayout(m_device, layout.pipelineLayout, nullptr);
                }
                if (layout.descriptorSetLayout != VK_NULL_HANDLE) {
                    vkDestroyDescriptorSetLayout(m_device, layout.descriptorSetLayout, nullptr);
                }
                for (VkSampler sampler : layout.immutableSamplers) {
                    if (sampler != VK_NULL_HANDLE) {
                        vkDestroySampler(m_device, sampler, nullptr);
                    }
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
        m_rhiImageViewResources.clear();
        m_rhiDescriptors.clear();
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
} // namespace SasamiEngine::Render
#endif
