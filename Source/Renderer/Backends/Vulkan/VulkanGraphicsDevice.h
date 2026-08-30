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
        bool ReadRhiBuffer(RhiBufferHandle buffer,
                           uint64_t offsetInBytes,
                           void* data,
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
        bool CreateRhiUnorderedAccessView(RhiTextureHandle texture,
                                          const RhiTextureViewDesc& desc,
                                          RhiCpuDescriptorHandle destination) override;
        bool CreateRhiBufferUnorderedAccessView(RhiBufferHandle buffer,
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

        // Ray tracing (VK_KHR_acceleration_structure + VK_KHR_ray_tracing_pipeline)
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

        // Self-contained validation of the Vulkan HW ray-tracing path: builds a
        // one-triangle BLAS/TLAS, an RT pipeline + SBT, traces one ray per pixel
        // into a storage image, and verifies hit=red / miss=blue on readback.
        // Returns true on success; details are written to outMessage when provided.
        bool RunRayTracingSmokeTest(std::string* outMessage);

        // Runs RunRayTracingSmokeTest once at init when SASAMI_VK_RT_SMOKETEST=1.
        void RunRayTracingSmokeTestIfRequested();

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
        bool EnsureRhiDescriptorPool();
        bool EnsureNativeMeshResources();
        void DestroyNativeMeshResources();
        bool EnsureNativeSkinnedMeshResources();
        void DestroyNativeSkinnedMeshResources();
        VkDescriptorSet GetOrCreateNativeMeshTextureDescriptorSet(uint64_t albedoSrv);
        bool RenderMeshFrame(uint32_t frame,
                             uint32_t imageIndex,
                             VkCommandBuffer cmd,
                             const RhiBackendMeshFrameDesc& desc,
                             const RhiClearColor& clearColor);
        bool EnsureRayMarchResources();
        void DestroyRayMarchResources();
        bool RenderRayMarchFrame(uint32_t imageIndex,
                                 VkCommandBuffer cmd,
                                 const RhiBackendRayMarchFrameDesc& desc,
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
            VkDescriptorType           type                = VK_DESCRIPTOR_TYPE_MAX_ENUM;
            VkImageView                imageView           = VK_NULL_HANDLE;
            VkBuffer                   buffer              = VK_NULL_HANDLE;
            VkDeviceSize               offset              = 0;
            VkDeviceSize               range               = 0;
            uint64_t                   resourceId          = 0;
            VkAccelerationStructureKHR accelerationStructure = VK_NULL_HANDLE;
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

        // Optional extension availability (detected at PickPhysicalDevice)
        bool m_hasVkKhrDynamicRendering      = false;
        bool m_hasVkExtDescriptorIndexing    = false;
        bool m_hasVkKhrTimelineSemaphore     = false;
        bool m_hasVkKhrAccelerationStructure = false;
        bool m_hasVkKhrRayTracingPipeline    = false;
        bool m_hasVkKhrRayQuery              = false;
        bool m_hasVkKhrBufferDeviceAddress   = false;
        bool m_hasVkKhrDeferredHostOps       = false;

        // RT function pointers (loaded after vkCreateDevice when extensions are available)
        PFN_vkGetAccelerationStructureBuildSizesKHR    m_pfnGetAsBuildSizes  = nullptr;
        PFN_vkCreateAccelerationStructureKHR           m_pfnCreateAs         = nullptr;
        PFN_vkDestroyAccelerationStructureKHR          m_pfnDestroyAs        = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR        m_pfnCmdBuildAs       = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR m_pfnGetAsAddress     = nullptr;
        PFN_vkCreateRayTracingPipelinesKHR             m_pfnCreateRtPipeline = nullptr;
        PFN_vkGetRayTracingShaderGroupHandlesKHR       m_pfnGetRtHandles     = nullptr;
        PFN_vkCmdTraceRaysKHR                          m_pfnCmdTraceRays     = nullptr;
        PFN_vkGetBufferDeviceAddressKHR                m_pfnGetBufAddr       = nullptr;

        // Ray-tracing pipeline properties (queried when VK_KHR_ray_tracing_pipeline
        // is available). Used to size and align the shader binding table.
        uint32_t m_rtShaderGroupHandleSize      = 0;
        uint32_t m_rtShaderGroupBaseAlignment   = 0;
        uint32_t m_rtShaderGroupHandleAlignment = 0;
        uint32_t m_rtMaxRayRecursionDepth       = 0;

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
        struct VulkanAccelStruct {
            VkAccelerationStructureKHR as            = VK_NULL_HANDLE;
            VkBuffer                   buffer        = VK_NULL_HANDLE;
            VkDeviceMemory             memory        = VK_NULL_HANDLE;
            VkDeviceAddress            deviceAddress = 0;
        };
        std::unordered_map<uint64_t, VulkanAccelStruct> m_accelStructures;

        // Ray-tracing pipeline record. Owns the VkPipeline plus an internal fixed
        // descriptor set layout (binding 0 = acceleration structure, binding 1 =
        // storage image) and its pipeline layout — mirroring how the DX12 backend
        // creates an internal root signature for RT pipelines.
        struct VulkanRtPipeline {
            VkPipeline            pipeline      = VK_NULL_HANDLE;
            VkPipelineLayout      pipelineLayout= VK_NULL_HANDLE;
            VkDescriptorSetLayout descSetLayout = VK_NULL_HANDLE;
            VkShaderModule        module        = VK_NULL_HANDLE;
            uint32_t              groupCount    = 0;
            // Descriptor set bound at dispatch time. RhiDispatchRaysDesc carries no
            // resource bindings (full render-graph binding is a later step), so the
            // caller (e.g. the RT smoke test) populates this before DispatchRays.
            VkDescriptorSet       boundSet      = VK_NULL_HANDLE;
        };
        std::unordered_map<uint64_t, VulkanRtPipeline> m_rtPipelines;

        // Shader binding table record. Holds the SBT buffer plus the strided
        // device-address regions passed to vkCmdTraceRaysKHR.
        struct VulkanShaderBindingTable {
            VkBuffer        buffer     = VK_NULL_HANDLE;
            VkDeviceMemory  memory     = VK_NULL_HANDLE;
            uint64_t        pipelineId = 0;
            VkStridedDeviceAddressRegionKHR raygen{};
            VkStridedDeviceAddressRegionKHR miss{};
            VkStridedDeviceAddressRegionKHR hit{};
            VkStridedDeviceAddressRegionKHR callable{};
        };
        std::unordered_map<uint64_t, VulkanShaderBindingTable> m_shaderBindingTables;

        std::unordered_map<uint64_t, VkImageView> m_rhiImageViews;
        std::unordered_map<uint64_t, uint64_t> m_rhiImageViewResources;
        std::unordered_map<uint64_t, VulkanRhiDescriptor> m_rhiDescriptors;
        VkDescriptorPool m_rhiDescriptorPool = VK_NULL_HANDLE;
        VkRenderPass          m_nativeMeshRenderPass       = VK_NULL_HANDLE;
        VkPipelineLayout      m_nativeMeshPipelineLayout   = VK_NULL_HANDLE;
        VkPipeline            m_nativeMeshPipeline         = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_nativeMeshDescSetLayout    = VK_NULL_HANDLE;
        VkDescriptorPool      m_nativeMeshDescPool         = VK_NULL_HANDLE;
        VkSampler             m_nativeMeshSampler          = VK_NULL_HANDLE;
        VkImage               m_nativeMeshDummyImage       = VK_NULL_HANDLE;
        VkImageView           m_nativeMeshDummyImageView   = VK_NULL_HANDLE;
        VkDeviceMemory        m_nativeMeshDummyMemory      = VK_NULL_HANDLE;
        std::unordered_map<uint64_t, VkDescriptorSet> m_nativeMeshDescSets;
        std::vector<VkFramebuffer> m_nativeMeshFramebuffers;
        std::vector<VkImage> m_nativeMeshDepthImages;
        std::vector<VkDeviceMemory> m_nativeMeshDepthMemory;
        std::vector<VkImageView> m_nativeMeshDepthViews;

        // Skinned-mesh draws share m_nativeMeshRenderPass/Framebuffers/DescSetLayout/Sampler
        // (same render pass scope, same set-0 texture binding) — only the pipeline, the
        // extra set-1 bone descriptor, and the bone UBO are dedicated to this path.
        VkPipelineLayout      m_nativeSkinnedMeshPipelineLayout    = VK_NULL_HANDLE;
        VkPipeline            m_nativeSkinnedMeshPipeline          = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_nativeSkinnedMeshBoneDescSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool      m_nativeSkinnedMeshBoneDescPool      = VK_NULL_HANDLE;
        VkDescriptorSet       m_nativeSkinnedMeshBoneDescSet       = VK_NULL_HANDLE;
        VkBuffer              m_nativeSkinnedMeshBoneUbo           = VK_NULL_HANDLE;
        VkDeviceMemory        m_nativeSkinnedMeshBoneUboMemory     = VK_NULL_HANDLE;
        void*                 m_nativeSkinnedMeshBoneUboMapped     = nullptr;
        VkDeviceSize          m_nativeSkinnedMeshBoneSlotStride    = 0;

        VkRenderPass            m_nativeRayMarchRenderPass     = VK_NULL_HANDLE;
        VkDescriptorSetLayout   m_nativeRayMarchDescSetLayout  = VK_NULL_HANDLE;
        VkDescriptorPool        m_nativeRayMarchDescPool       = VK_NULL_HANDLE;
        VkDescriptorSet         m_nativeRayMarchDescSet        = VK_NULL_HANDLE;
        VkPipelineLayout        m_nativeRayMarchPipelineLayout = VK_NULL_HANDLE;
        VkPipeline              m_nativeRayMarchPipeline       = VK_NULL_HANDLE;
        VkBuffer                m_nativeRayMarchUbo            = VK_NULL_HANDLE;
        VkDeviceMemory          m_nativeRayMarchUboMemory      = VK_NULL_HANDLE;
        void*                   m_nativeRayMarchUboMapped      = nullptr;
        std::vector<VkFramebuffer> m_nativeRayMarchFramebuffers;

        // Records pipeline bind + descriptor bind + vkCmdTraceRaysKHR. Shared by the
        // command encoder's DispatchRays and the RT smoke test. Takes raw Vulkan
        // handles so it can be declared before the RT record structs.
        void RecordTraceRays(VkCommandBuffer cmd,
                             VkPipeline pipeline, VkPipelineLayout layout,
                             VkDescriptorSet descriptorSet,
                             const VkStridedDeviceAddressRegionKHR& raygen,
                             const VkStridedDeviceAddressRegionKHR& miss,
                             const VkStridedDeviceAddressRegionKHR& hit,
                             const VkStridedDeviceAddressRegionKHR& callable,
                             uint32_t width, uint32_t height, uint32_t depth);

        friend class VulkanRhiCommandEncoder;
    };
#endif
}
