#pragma once

#include "Renderer/RHI/GraphicsDevice.h"

#if RHI_VULKAN
#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>
#include <windows.h>
#include <string>
#include <unordered_map>
#include <vector>
#endif

namespace SasamiRenderer
{
#if RHI_VULKAN
    class VulkanGraphicsDevice final : public IRHIDevice
    {
    public:
        ~VulkanGraphicsDevice() override;

        bool Initialize(HWND hWnd, UINT width, UINT height, UINT bufferCount = 2) override;
        GraphicsRuntime GetBackend() const override;
        void* GetNativeDeviceHandle() const override;
        void* GetNativeGraphicsQueueHandle() const override;
        ID3D12Device* GetDevice() const override;
        ID3D12Device5* GetRayTracingDevice() const override;
        const RhiBackendCapabilities& GetCapabilities() const override;
        bool SupportsHardwareRayTracing() const override;
        CommandQueue& GetCommandQueue() override;
        CommandQueue& GetComputeQueue() override;
        SwapChain& GetSwapChain() override;
        UINT GetDescriptorHandleIncrementSize(DescriptorHeapType type) const override;
        void WaitForGPU() override;
        bool ExecuteBackendFrame(const RhiBackendFrameDesc& frameDesc) override;
        bool RenderBackendClearFrame(const float clearColor[4]) override;
        bool ResizeBackendSwapChain(UINT width, UINT height) override;
        RhiTextureHandle CreateRhiTexture2DFromRgba8(uint32_t width,
                                                     uint32_t height,
                                                     const void* pixels,
                                                     uint32_t rowPitchBytes) override;
        RhiTextureHandle CreateRhiTexture(const RhiTextureDesc& desc) override;
        RhiBufferHandle CreateRhiBuffer(const RhiBufferDesc& desc, const void* initialData = nullptr) override;
        bool UpdateRhiBuffer(RhiBufferHandle buffer,
                             uint64_t offsetInBytes,
                             const void* data,
                             uint64_t sizeInBytes) override;
        bool DestroyRhiResource(RhiResourceHandle resource) override;
        RhiShaderHandle CreateRhiShaderModule(const RhiShaderModuleDesc& desc) override;
        RhiPipelineLayoutHandle CreateRhiPipelineLayout(const RhiPipelineLayoutDesc& desc) override;
        RhiPipelineHandle CreateRhiGraphicsPipeline(const RhiGraphicsPipelineDesc& desc) override;
        RhiPipelineHandle CreateRhiComputePipeline(const RhiComputePipelineDesc& desc) override;
        RhiDescriptorAllocation AllocateRhiDescriptors(RhiDescriptorHeapType type,
                                                      uint32_t count,
                                                      bool shaderVisible) override;
        bool CreateRhiShaderResourceView(RhiResourceHandle resource,
                                         const RhiTextureViewDesc& desc,
                                         RhiCpuDescriptorHandle destination) override;
        bool CreateRhiBufferShaderResourceView(RhiBufferHandle buffer,
                                               const RhiBufferViewDesc& desc,
                                               RhiCpuDescriptorHandle destination) override;
        bool CreateRhiRenderTargetView(RhiTextureHandle texture,
                                       const RhiRenderTargetViewDesc& desc,
                                       RhiCpuDescriptorHandle destination) override;
        bool CreateRhiDepthStencilView(RhiTextureHandle texture,
                                       const RhiDepthStencilViewDesc& desc,
                                       RhiCpuDescriptorHandle destination) override;
        std::unique_ptr<IRhiCommandEncoder> CreateCommandEncoder(RhiQueueType queueType) override;
        bool SubmitCommandEncoder(IRhiCommandEncoder& encoder, RhiQueueType queueType) override;

        // Ray tracing (Vulkan stubs — VK_KHR_ray_tracing_pipeline not yet implemented)
        bool BuildRhiBlases(const RhiBlasDesc* descs, uint32_t count,
                             RhiAccelerationStructureHandle* outHandles) override;
        RhiAccelerationStructureHandle BuildRhiTlas(const RhiTlasDesc& desc) override;
        bool DestroyRhiAccelerationStructure(RhiAccelerationStructureHandle handle) override;
        RhiGpuAddress GetRhiAccelerationStructureGpuAddress(RhiAccelerationStructureHandle handle) override;
        bool CreateRhiAccelerationStructureSrv(
            RhiAccelerationStructureHandle handle, RhiCpuDescriptorHandle dest) override;
        RhiRayTracingPipelineHandle CreateRhiRayTracingPipeline(
            const RhiRayTracingPipelineDesc& desc) override;
        RhiShaderBindingTableHandle CreateRhiShaderBindingTable(
            const RhiShaderBindingTableDesc& desc) override;

        HRESULT CreateDescriptorHeap(const DescriptorHeapDesc& desc, DescriptorHeap& out) override;
        HRESULT CreateCommittedResource(const HeapProperties* heapProps,
                                        HeapFlags heapFlags,
                                        const ResourceDesc* desc,
                                        ResourceState initialState,
                                        const ClearValue* clearValue,
                                        Resource& out) override;
        HRESULT CreateCommandAllocator(CommandListType type, CommandAllocator& out) override;
        HRESULT CreateCommandList(UINT nodeMask,
                                  CommandListType type,
                                  CommandAllocator& allocator,
                                  PipelineState* initialPSO,
                                  CommandList& out) override;
        HRESULT CreateGraphicsPipelineState(const GraphicsPipelineDesc& desc, PipelineState& out) override;
        HRESULT CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, PipelineState& out) override;
        HRESULT CreatePipelineStateFromStream(const void* streamData, size_t streamSize, PipelineState& out) override;
        HRESULT CreateRootSignature(UINT nodeMask, const void* blobData, size_t blobSize, RootSignature& out) override;
        void CreateShaderResourceView(Resource& resource, const ShaderResourceViewDesc* desc, CpuDescriptorHandle dest) override;
        void CreateDepthStencilView(Resource& resource, const DepthStencilViewDesc* desc, CpuDescriptorHandle dest) override;
        void CreateRenderTargetView(Resource& resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, CpuDescriptorHandle dest) override;
        void CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc, CpuDescriptorHandle dest) override;
        void CreateSampler(const D3D12_SAMPLER_DESC* desc, CpuDescriptorHandle dest) override;
        HRESULT CreateFence(UINT64 initialValue, D3D12_FENCE_FLAGS flags, ID3D12Fence** fence) override;

    private:
        bool CreateInstance();
        bool CreateSurface(HWND hWnd);
        bool PickPhysicalDevice();
        bool CreateDevice();
        bool CreateSwapChain(UINT width, UINT height, UINT bufferCount);
        bool CreateFrameResources(UINT bufferCount);
        bool CreateSwapChainImageViews();
        bool EnsureRhiDescriptorPool();
        bool EnsureNativeMeshResources();
        void DestroyNativeMeshResources();
        bool RenderMeshFrame(uint32_t frame,
                             uint32_t imageIndex,
                             VkCommandBuffer cmd,
                             const RhiBackendMeshFrameDesc& desc,
                             const RhiClearColor& clearColor);
        void QueryCapabilities();
        uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const;
        void DestroyFrameResources();
        void DestroySwapChain();
        void Cleanup();

        struct VulkanRhiResource
        {
            VkImage image = VK_NULL_HANDLE;
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            uint64_t sizeInBytes = 0;
            RhiMemoryUsage memoryUsage = RhiMemoryUsage::GpuOnly;
            RhiExtent3D extent{};
            RhiFormat format = RhiFormat::Unknown;
        };

        struct VulkanRhiShader
        {
            VkShaderModule module = VK_NULL_HANDLE;
            RhiShaderStageFlags stage = RhiShaderStageFlags::None;
            std::string entryPoint;
        };

        struct VulkanRhiPipelineLayout
        {
            struct Binding
            {
                uint32_t binding = 0;
                uint32_t descriptorCount = 0;
                VkDescriptorType descriptorType = VK_DESCRIPTOR_TYPE_MAX_ENUM;
                bool valid = false;
            };

            VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
            std::vector<Binding> bindings;
            std::vector<VkSampler> immutableSamplers;
        };

        struct VulkanRhiPipeline
        {
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkRenderPass renderPass = VK_NULL_HANDLE;
            VkPipelineLayout ownedPipelineLayout = VK_NULL_HANDLE;
            RhiPipelineLayoutHandle layout{};
        };

        struct VulkanRhiDescriptor
        {
            VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
            VkImageView imageView = VK_NULL_HANDLE;
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            VkDeviceSize range = 0;
            uint64_t resourceId = 0;
        };

        VkInstance m_instance = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkDevice m_device = VK_NULL_HANDLE;
        VkSurfaceKHR m_surface = VK_NULL_HANDLE;
        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        VkQueue m_computeQueue = VK_NULL_HANDLE;
        VkQueue m_presentQueue = VK_NULL_HANDLE;
        VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
        VkCommandPool m_commandPool = VK_NULL_HANDLE;
        VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D m_swapchainExtent{};
        std::vector<VkImage> m_swapchainImages;
        std::vector<VkImageView> m_swapchainImageViews;
        std::vector<VkImageLayout> m_swapchainImageLayouts;
        std::vector<VkCommandBuffer> m_commandBuffers;
        std::vector<VkSemaphore> m_imageAvailableSemaphores;
        std::vector<VkSemaphore> m_renderFinishedSemaphores;
        std::vector<VkFence> m_frameFences;
        UINT m_graphicsQueueFamily = 0;
        UINT m_computeQueueFamily = 0;
        UINT m_presentQueueFamily = 0;
        UINT m_currentFrame = 0;
        bool m_hasDedicatedComputeQueue = false;
        RhiBackendCapabilities m_capabilities{};
        CommandQueue m_emptyGraphicsQueue;
        CommandQueue m_emptyComputeQueue;
        SwapChain m_emptySwapChain;
        uint64_t m_nextRhiResourceHandle = 1;
        uint64_t m_nextRhiDescriptorHandle = 1;
        uint64_t m_nextRhiShaderHandle = 1;
        uint64_t m_nextRhiPipelineLayoutHandle = 1;
        uint64_t m_nextRhiPipelineHandle = 1;
        std::unordered_map<uint64_t, VulkanRhiResource> m_rhiResources;
        std::unordered_map<uint64_t, VulkanRhiShader> m_rhiShaders;
        std::unordered_map<uint64_t, VulkanRhiPipelineLayout> m_rhiPipelineLayouts;
        std::unordered_map<uint64_t, VulkanRhiPipeline> m_rhiPipelines;
        std::unordered_map<uint64_t, VkImageView> m_rhiImageViews;
        std::unordered_map<uint64_t, uint64_t> m_rhiImageViewResources;
        std::unordered_map<uint64_t, VulkanRhiDescriptor> m_rhiDescriptors;
        VkDescriptorPool m_rhiDescriptorPool = VK_NULL_HANDLE;
        VkRenderPass m_nativeMeshRenderPass = VK_NULL_HANDLE;
        VkPipelineLayout m_nativeMeshPipelineLayout = VK_NULL_HANDLE;
        VkPipeline m_nativeMeshPipeline = VK_NULL_HANDLE;
        std::vector<VkFramebuffer> m_nativeMeshFramebuffers;
        std::vector<VkImage> m_nativeMeshDepthImages;
        std::vector<VkDeviceMemory> m_nativeMeshDepthMemory;
        std::vector<VkImageView> m_nativeMeshDepthViews;

        friend class VulkanRhiCommandEncoder;
    };
#endif
}
