#pragma once

#include <cstdint>

namespace SasamiRenderer
{
    using RhiGpuAddress = uint64_t;

    enum class RhiBackendApi : uint32_t
    {
        Unknown,
        DirectX12,
        Vulkan,
        DirectX11,
        OpenGL,
        Metal,
    };

    enum class RhiFormat : uint32_t
    {
        Unknown,
        R8UNorm,
        R8G8B8A8UNorm,
        B8G8R8A8UNorm,
        R16G16B16A16Float,
        R32G32Float,
        R32Float,
        R32UInt,
        D32Float,
        D24UNormS8UInt,
    };

    enum class RhiResourceState : uint32_t
    {
        Common,
        RenderTarget,
        DepthWrite,
        DepthRead,
        ShaderResource,
        UnorderedAccess,
        CopySource,
        CopyDest,
        Present,
    };

    enum class RhiResourceDimension : uint32_t
    {
        Buffer,
        Texture1D,
        Texture2D,
        Texture3D,
    };

    enum class RhiMemoryUsage : uint32_t
    {
        GpuOnly,
        CpuToGpu,
        GpuToCpu,
    };

    enum class RhiQueueType : uint32_t
    {
        Graphics,
        Compute,
        Copy,
        Present,
    };

    enum class RhiCommandListType : uint32_t
    {
        Graphics,
        Compute,
        Copy,
    };

    enum class RhiDescriptorHeapType : uint32_t
    {
        CbvSrvUav,
        Sampler,
        RenderTarget,
        DepthStencil,
    };

    enum class RhiTextureViewDimension : uint32_t
    {
        Texture1D,
        Texture1DArray,
        Texture2D,
        Texture2DArray,
        Texture3D,
        TextureCube,
        TextureCubeArray,
    };

    enum class RhiBufferViewType : uint32_t
    {
        Raw,
        Structured,
        Typed,
        Constant,
    };

    enum class RhiTextureUsageFlags : uint32_t
    {
        None = 0,
        ShaderResource = 1u << 0,
        RenderTarget = 1u << 1,
        DepthStencil = 1u << 2,
        UnorderedAccess = 1u << 3,
        CopySource = 1u << 4,
        CopyDest = 1u << 5,
        Present = 1u << 6,
    };

    inline RhiTextureUsageFlags operator|(RhiTextureUsageFlags lhs, RhiTextureUsageFlags rhs)
    {
        return static_cast<RhiTextureUsageFlags>(
            static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    inline bool HasFlag(RhiTextureUsageFlags value, RhiTextureUsageFlags flag)
    {
        return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
    }

    enum class RhiBufferUsageFlags : uint32_t
    {
        None = 0,
        Vertex = 1u << 0,
        Index = 1u << 1,
        Constant = 1u << 2,
        Structured = 1u << 3,
        ShaderResource = 1u << 4,
        UnorderedAccess = 1u << 5,
        CopySource = 1u << 6,
        CopyDest = 1u << 7,
        AccelerationStructure = 1u << 8,
    };

    inline RhiBufferUsageFlags operator|(RhiBufferUsageFlags lhs, RhiBufferUsageFlags rhs)
    {
        return static_cast<RhiBufferUsageFlags>(
            static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    inline bool HasFlag(RhiBufferUsageFlags value, RhiBufferUsageFlags flag)
    {
        return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
    }

    enum class RhiShaderStageFlags : uint32_t
    {
        None = 0,
        Vertex = 1u << 0,
        Hull = 1u << 1,
        Domain = 1u << 2,
        Geometry = 1u << 3,
        Pixel = 1u << 4,
        Compute = 1u << 5,
        RayGeneration = 1u << 6,
        AnyHit = 1u << 7,
        ClosestHit = 1u << 8,
        Miss = 1u << 9,
        Mesh = 1u << 10,
        Amplification = 1u << 11,
        AllGraphics = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 10) | (1u << 11),
        All = 0xffffffffu,
    };

    inline RhiShaderStageFlags operator|(RhiShaderStageFlags lhs, RhiShaderStageFlags rhs)
    {
        return static_cast<RhiShaderStageFlags>(
            static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    }

    enum class RhiLoadOp : uint32_t
    {
        Load,
        Clear,
        DontCare,
    };

    enum class RhiStoreOp : uint32_t
    {
        Store,
        DontCare,
    };

    enum class RhiPrimitiveTopology : uint32_t
    {
        TriangleList,
        TriangleStrip,
        LineList,
        LineStrip,
        PointList,
        PatchList,
    };

    enum class RhiCullMode : uint32_t
    {
        None,
        Front,
        Back,
    };

    enum class RhiFillMode : uint32_t
    {
        Solid,
        Wireframe,
    };

    struct RhiHandle
    {
        uint64_t id = 0;
        bool IsValid() const { return id != 0; }
    };

    using RhiResourceHandle = RhiHandle;
    using RhiTextureHandle = RhiHandle;
    using RhiBufferHandle = RhiHandle;
    using RhiShaderHandle = RhiHandle;
    using RhiPipelineHandle = RhiHandle;
    using RhiPipelineLayoutHandle = RhiHandle;
    using RhiDescriptorSetHandle = RhiHandle;

    enum class RhiBindingType : uint32_t
    {
        ConstantBuffer,
        ShaderResource,
        UnorderedAccess,
        Sampler,
        RootConstants,
    };

    enum class RhiInputRate : uint32_t
    {
        PerVertex,
        PerInstance,
    };

    enum class RhiCompareOp : uint32_t
    {
        Never,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always,
    };

    struct RhiCpuDescriptorHandle
    {
        uint64_t ptr = 0;
        bool IsValid() const { return ptr != 0; }
    };

    struct RhiGpuDescriptorHandle
    {
        uint64_t ptr = 0;
        bool IsValid() const { return ptr != 0; }
    };

    struct RhiDescriptorAllocation
    {
        RhiDescriptorHeapType type = RhiDescriptorHeapType::CbvSrvUav;
        RhiCpuDescriptorHandle cpu{};
        RhiGpuDescriptorHandle gpu{};
        uint32_t count = 0;
        uint32_t increment = 0;
        bool IsValid() const { return count > 0 && cpu.IsValid(); }
    };

    struct RhiExtent3D
    {
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t depth = 1;
    };

    struct RhiViewport
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float minDepth = 0.0f;
        float maxDepth = 1.0f;
    };

    struct RhiRect
    {
        int32_t left = 0;
        int32_t top = 0;
        int32_t right = 0;
        int32_t bottom = 0;
    };

    struct RhiClearColor
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float a = 1.0f;
    };

    struct RhiClearDepthStencil
    {
        float depth = 1.0f;
        uint32_t stencil = 0;
    };

    struct RhiTextureDesc
    {
        RhiResourceDimension dimension = RhiResourceDimension::Texture2D;
        RhiExtent3D extent{};
        uint32_t mipLevels = 1;
        uint32_t arrayLayers = 1;
        RhiFormat format = RhiFormat::Unknown;
        RhiTextureUsageFlags usage = RhiTextureUsageFlags::ShaderResource;
        RhiMemoryUsage memoryUsage = RhiMemoryUsage::GpuOnly;
        RhiResourceState initialState = RhiResourceState::Common;
    };

    struct RhiTextureViewDesc
    {
        RhiFormat format = RhiFormat::Unknown;
        RhiTextureViewDimension dimension = RhiTextureViewDimension::Texture2D;
        uint32_t baseMipLevel = 0;
        uint32_t mipLevelCount = 1;
        uint32_t baseArrayLayer = 0;
        uint32_t arrayLayerCount = 1;
    };

    struct RhiBufferViewDesc
    {
        RhiBufferViewType type = RhiBufferViewType::Structured;
        RhiFormat format = RhiFormat::Unknown;
        uint64_t offset = 0;
        uint64_t sizeInBytes = 0;
        uint32_t strideInBytes = 0;
    };

    struct RhiRenderTargetViewDesc
    {
        RhiFormat format = RhiFormat::Unknown;
        RhiTextureViewDimension dimension = RhiTextureViewDimension::Texture2D;
        uint32_t mipLevel = 0;
        uint32_t baseArrayLayer = 0;
        uint32_t arrayLayerCount = 1;
    };

    struct RhiDepthStencilViewDesc
    {
        RhiFormat format = RhiFormat::Unknown;
        RhiTextureViewDimension dimension = RhiTextureViewDimension::Texture2D;
        uint32_t mipLevel = 0;
        uint32_t baseArrayLayer = 0;
        uint32_t arrayLayerCount = 1;
        bool readOnlyDepth = false;
        bool readOnlyStencil = false;
    };

    struct RhiBufferDesc
    {
        uint64_t sizeInBytes = 0;
        uint32_t strideInBytes = 0;
        RhiBufferUsageFlags usage = RhiBufferUsageFlags::ShaderResource;
        RhiMemoryUsage memoryUsage = RhiMemoryUsage::GpuOnly;
        RhiResourceState initialState = RhiResourceState::Common;
    };

    struct RhiAttachmentDesc
    {
        RhiTextureHandle texture{};
        RhiFormat format = RhiFormat::Unknown;
        RhiResourceState initialState = RhiResourceState::Common;
        RhiResourceState finalState = RhiResourceState::Common;
        RhiLoadOp loadOp = RhiLoadOp::Load;
        RhiStoreOp storeOp = RhiStoreOp::Store;
        RhiClearColor clearColor{};
        RhiClearDepthStencil clearDepthStencil{};
    };

    struct RhiRenderPassDesc
    {
        const RhiAttachmentDesc* colorAttachments = nullptr;
        uint32_t colorAttachmentCount = 0;
        const RhiAttachmentDesc* depthStencilAttachment = nullptr;
    };

    struct RhiShaderModuleDesc
    {
        const void* bytecode = nullptr;
        uint64_t bytecodeSize = 0;
        const char* entryPoint = nullptr;
        RhiShaderStageFlags stage = RhiShaderStageFlags::None;
    };

    struct RhiBindingRangeDesc
    {
        RhiBindingType type = RhiBindingType::ShaderResource;
        RhiShaderStageFlags visibility = RhiShaderStageFlags::All;
        uint32_t baseRegister = 0;
        uint32_t registerSpace = 0;
        uint32_t descriptorCount = 1;
        bool inlineRootDescriptor = false;
        uint32_t rootConstantCount = 0;
    };

    struct RhiStaticSamplerDesc
    {
        RhiShaderStageFlags visibility = RhiShaderStageFlags::Pixel;
        uint32_t shaderRegister = 0;
        uint32_t registerSpace = 0;
        bool linearFilter = true;
        bool clamp = false;
    };

    struct RhiPipelineLayoutDesc
    {
        const RhiBindingRangeDesc* bindings = nullptr;
        uint32_t bindingCount = 0;
        const RhiStaticSamplerDesc* staticSamplers = nullptr;
        uint32_t staticSamplerCount = 0;
        bool allowInputAssembler = true;
    };

    struct RhiVertexAttributeDesc
    {
        const char* semantic = nullptr;
        uint32_t semanticIndex = 0;
        RhiFormat format = RhiFormat::Unknown;
        uint32_t binding = 0;
        uint32_t offset = 0;
    };

    struct RhiVertexBindingDesc
    {
        uint32_t binding = 0;
        uint32_t stride = 0;
        RhiInputRate inputRate = RhiInputRate::PerVertex;
    };

    struct RhiRasterStateDesc
    {
        RhiFillMode fillMode = RhiFillMode::Solid;
        RhiCullMode cullMode = RhiCullMode::Back;
        bool frontCounterClockwise = false;
        bool depthClipEnabled = true;
    };

    struct RhiDepthStencilStateDesc
    {
        bool depthTestEnabled = true;
        bool depthWriteEnabled = true;
        bool stencilEnabled = false;
        RhiCompareOp depthCompare = RhiCompareOp::LessEqual;
    };

    struct RhiBlendStateDesc
    {
        bool alphaBlendEnabled = false;
    };

    struct RhiGraphicsPipelineDesc
    {
        RhiPipelineLayoutHandle layout{};
        const RhiShaderHandle* shaderHandles = nullptr;
        uint32_t shaderHandleCount = 0;
        const RhiShaderModuleDesc* shaders = nullptr;
        uint32_t shaderCount = 0;
        const RhiVertexBindingDesc* vertexBindings = nullptr;
        uint32_t vertexBindingCount = 0;
        const RhiVertexAttributeDesc* vertexAttributes = nullptr;
        uint32_t vertexAttributeCount = 0;
        RhiPrimitiveTopology topology = RhiPrimitiveTopology::TriangleList;
        RhiRasterStateDesc raster{};
        RhiDepthStencilStateDesc depthStencil{};
        RhiBlendStateDesc blend{};
        const RhiFormat* colorFormats = nullptr;
        uint32_t colorFormatCount = 0;
        RhiFormat depthStencilFormat = RhiFormat::Unknown;
    };

    struct RhiComputePipelineDesc
    {
        RhiShaderModuleDesc shader{};
    };

    struct RhiBackendFrameDesc
    {
        RhiClearColor clearColor{};
        bool present = true;
    };

    struct RhiResourceTransitionDesc
    {
        RhiResourceHandle resource{};
        RhiResourceState before = RhiResourceState::Common;
        RhiResourceState after = RhiResourceState::Common;
        uint32_t subresource = 0xffffffffu;
    };

    struct RhiDrawIndexedDesc
    {
        uint32_t indexCount = 0;
        uint32_t instanceCount = 1;
        uint32_t startIndex = 0;
        int32_t baseVertex = 0;
        uint32_t startInstance = 0;
    };

    struct RhiDrawDesc
    {
        uint32_t vertexCount = 0;
        uint32_t instanceCount = 1;
        uint32_t startVertex = 0;
        uint32_t startInstance = 0;
    };

    struct RhiDispatchDesc
    {
        uint32_t groupCountX = 1;
        uint32_t groupCountY = 1;
        uint32_t groupCountZ = 1;
    };

    using RhiDescriptorHeapHandle = RhiHandle;

    struct RhiVertexBufferView
    {
        RhiGpuAddress gpuAddress   = 0;
        uint32_t      strideInBytes = 0;
        uint32_t      sizeInBytes   = 0;
    };

    struct RhiIndexBufferView
    {
        RhiGpuAddress gpuAddress  = 0;
        uint32_t      sizeInBytes = 0;
        bool          is32Bit     = true;
    };

    struct RhiBackendCapabilities
    {
        RhiBackendApi api = RhiBackendApi::Unknown;
        bool supportsGraphicsQueue = true;
        bool supportsComputeQueue = false;
        bool supportsSwapChain = true;
        bool supportsNativeFrame = false;
        bool supportsFeatureRenderPasses = false;
        bool supportsD3D12CompatibilitySurface = false;
        bool supportsRhiResourceCreation = false;
        bool supportsRhiDescriptorCreation = false;
        bool supportsRhiPipelineCreation = false;
        bool supportsRhiCommandEncoding = false;
        bool supportsDynamicRenderPass = false;
        bool supportsHardwareRayTracing = false;
        bool supportsMeshShaders = false;
        bool supportsPipelineStateStream = false;
        bool supportsVulkanDynamicRendering = false;
        bool supportsRayQuery = false;
        bool supportsRayTracingPipeline = false;
        bool supportsDescriptorIndexing = false;
        bool supportsTimelineSemaphore = false;
    };
}
