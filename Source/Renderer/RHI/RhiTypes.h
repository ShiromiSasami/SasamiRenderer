#pragma once

#include <cstdint>

namespace SasamiEngine::Render
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
        // Same 32bpp layout as R8G8B8A8UNorm; the difference is that the sampler decodes to
        // linear on read and encodes on write. Base-colour textures and the G-Buffer albedo
        // target use it so 8-bit storage keeps its precision in the darks. A fully typed
        // D3D12 resource cannot be viewed through a different format, so the resource itself
        // has to be created with this -- not created as UNorm and viewed as sRGB.
        R8G8B8A8UNormSrgb,
        B8G8R8A8UNorm,
        R16G16B16A16Float,
        R32G32B32Float,
        R32G32Float,
        R16Float,
        R16Typeless,
        R16UNorm,
        R32Float,
        R32UInt,
        R32Typeless,
        D16UNorm,
        D32Float,
        D24UNormS8UInt,
        // Block-compressed (4x4 texel blocks; BC1/BC4 8 bytes per block, the rest 16). Only
        // sampled textures uploaded through CreateRhiTexture2DFromData accept them.
        BC1UNorm,
        BC1UNormSrgb,
        BC3UNorm,
        BC3UNormSrgb,
        BC4UNorm,
        BC5UNorm,
        BC7UNorm,
        BC7UNormSrgb,
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
        // Argument / count buffer read by DrawIndexedIndirect (D3D12 INDIRECT_ARGUMENT,
        // Vulkan DRAW_INDIRECT stage + INDIRECT_COMMAND_READ access). Appended last so the
        // numeric values of the older states do not move.
        IndirectArgument,
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
        // Heap to pass to IRhiCommandEncoder::SetDescriptorHeap (DX12: id is the raw
        // ID3D12DescriptorHeap*). Left invalid on backends without descriptor heaps.
        RhiHandle heap{};
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
        float depth = 0.0f; // Reverse-Z: far plane clears to 0.0, near plane is 1.0
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
        // Set when the texture will be viewed as a cube (RhiTextureViewDimension::TextureCube or
        // TextureCubeArray). Vulkan needs VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT at image-creation
        // time -- a cube view over an image created without it is invalid -- so the intent cannot
        // be inferred later from the view desc alone. Requires arrayLayers to be a multiple of 6.
        // D3D12/D3D11 derive the cube-ness from the view and ignore this.
        bool cubeCompatible = false;
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
        bool bufferResource = false;
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
        // D3D-style rasterizer depth bias (constant in minimum-resolvable-depth units, clamp, slope).
        // Reverse-Z engines pass negative values to push casters away from the light.
        // The OpenGL backend ignores the whole raster block for RHI pipelines (known gap).
        int32_t depthBias = 0;
        float depthBiasClamp = 0.0f;
        float slopeScaledDepthBias = 0.0f;
    };

    struct RhiDepthStencilStateDesc
    {
        bool depthTestEnabled = true;
        bool depthWriteEnabled = true;
        bool stencilEnabled = false;
        // Reverse-Z: near=1.0, far=0.0, so a fragment passes when its depth is >= what's stored.
        RhiCompareOp depthCompare = RhiCompareOp::GreaterEqual;
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
        RhiPipelineLayoutHandle layout{};
        RhiShaderModuleDesc shader{};
    };

    struct RhiBackendRayMarchFrameDesc
    {
        bool enabled = false;
        float invViewProjection[16] = {};
        float cameraPos[3] = {};
        float sceneTimeSec = 0.0f;
        float sunDir[3] = { 0.0f, -1.0f, 0.0f };
        float sunIntensity = 1.0f;
        float sunColor[3] = { 1.0f, 1.0f, 1.0f };
        float cloudCover = 0.35f;
        float renderWidth = 1.0f;
        float renderHeight = 1.0f;
        float fluidMode = 0.0f;
        float cloudDensity = 2.0f;
        float debugMode = 0.0f;
        float tanHalfFovY = 0.577350269f;
        float aspectRatio = 1.0f;
        float cameraRight[3] = { 1.0f, 0.0f, 0.0f };
        float cameraUp[3] = { 0.0f, 1.0f, 0.0f };
        bool explicitCameraBasis = false;
    };

    struct RhiBackendMeshDrawDesc
    {
        uint64_t vertexBufferHandle = 0;
        uint64_t indexBufferHandle = 0;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        uint32_t vertexStride = 0;
        uint32_t indexSizeInBytes = 0;
        bool index32Bit = true;
        bool transparent = false;
        uint64_t albedoSrv = 0;
        uint64_t occlusionSrv = 0;
        // Whether occlusionSrv's R channel may be read as ambient occlusion. The raster material
        // layout has a single texture slot, so when a model ships occlusion and metallic-roughness
        // as separate images the loader keeps metallic-roughness here and drops AO (see
        // MeshComponent.cpp), leaving this false. Without this flag a consumer cannot tell whether
        // .r is ambient occlusion or someone else's channel. Derived from the same material field
        // the D3D12 path gates on, MaterialConstants' extra[2][1] (occlusionStrength); D3D12 uses
        // it as a continuous scale, while the native paths can only treat it as on/off because
        // their push constants are already at Vulkan's guaranteed 128-byte minimum.
        bool occlusionSlotHasAo = false;
        float model[16] = {};
        float baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float emissive[3] = {};
        float roughness = 0.5f;
        float metallic = 0.0f;
    };

    struct RhiBackendSkinnedMeshDrawDesc
    {
        uint64_t vertexBufferHandle = 0;
        uint64_t indexBufferHandle = 0;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        uint32_t vertexStride = 0;
        uint32_t indexSizeInBytes = 0;
        bool index32Bit = true;
        bool transparent = false;
        uint64_t albedoSrv = 0;
        uint64_t occlusionSrv = 0;
        // Whether occlusionSrv's R channel may be read as ambient occlusion. Unlike the static
        // path, the skinned loader (ModelLoaderSkinned.cpp) only ever populates occlusionSrv from
        // glTF's dedicated occlusionTexture slot and never substitutes metallic-roughness into it,
        // so the slot always holds real AO; this then only reflects the asset-authored
        // occlusionStrength, matching the static path's test.
        bool occlusionSlotHasAo = false;
        float model[16] = {};
        float baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float emissive[3] = {};
        float roughness = 0.5f;
        float metallic = 0.0f;
        // Points at Skeleton::kMaxBones * 16 floats (row-major 4x4 palette); owned by the caller
        // for the duration of the synchronous ExecuteBackendFrame call, same convention as `draws`.
        const float* boneMatrices = nullptr;
    };

    struct RhiBackendMeshFrameDesc
    {
        bool enabled = false;
        const RhiBackendMeshDrawDesc* draws = nullptr;
        uint32_t drawCount = 0;
        const RhiBackendSkinnedMeshDrawDesc* skinnedDraws = nullptr;
        uint32_t skinnedDrawCount = 0;
        float viewProjection[16] = {};
        // Inverse of the above. The sky pass needs it to turn a screen position back into a
        // world-space ray; inverting on the GPU per pixel would be absurd, and the camera
        // already has it (RhiBackendRayMarchFrameDesc carries the same thing for the same reason).
        float invViewProjection[16] = {};
        float cameraPos[3] = {};
        float sunDir[3] = { 0.0f, -1.0f, 0.0f };
        float sunIntensity = 1.0f;
        // RenderSettings::exposure, applied before the native ACES tonemap (NativeTonemap.hlsli).
        float exposure = 1.3f;
        float sunColor[3] = { 1.0f, 1.0f, 1.0f };
        float renderWidth = 1.0f;
        float renderHeight = 1.0f;
        bool enableIbl = false;
        bool enableShadow = false;
        bool enableSsao = false;
        bool enablePostProcess = false;

        // Opaque descriptor handle of the environment cubemap for the native sky/ambient path, or 0
        // when no HDR environment is loaded. Same convention as the per-draw albedo handle: the value
        // is produced by CreateRhiShaderResourceView and only the owning backend can interpret it.
        uint64_t environmentCubeSrv = 0;

        // Low-resolution counterpart of environmentCubeSrv used as diffuse ambient. Sampling the full
        // resolution cube along the normal would read like a mirror, not an irradiance term.
        uint64_t ambientCubeSrv = 0;

        // Directional-shadow parameters for the native path. DirectX 11 and Vulkan both consume
        // them (Vulkan since 2026-09-25); OpenGL still reads none of them, so it renders the
        // native path unshadowed.
        //
        // `lightViewProjection` follows this renderer's row-vector convention and reverse-Z depth
        // (near = 1.0, far = 0.0), same as `viewProjection` above. Build it with
        // Math::BuildDirectionalLightViewProjection rather than by hand.
        float lightViewProjection[16] = {};
        float shadowDepthBias = 0.0f;
        // World-space offset along the surface normal, scaled by `shadowTexelWorld` in the shader.
        float shadowNormalBias = 0.0f;
        // World size of one shadow-map texel. The normal bias is meaningless without it.
        float shadowTexelWorld = 0.0f;
        // 0 means "no shadow map this frame" -- the backend falls back to unshadowed lighting even
        // when enableShadow is set, so a failed shadow allocation degrades instead of going black.
        uint32_t shadowMapResolution = 0u;

        // SSAO tuning for the native path. These mirror the RenderSettings fields of the same
        // names that the DirectX 12 generator is fed through its SSAOCB; before they existed here
        // all three native backends duplicated the defaults as local constants, so moving the UI
        // sliders changed nothing outside DirectX 12. The defaults below are those same
        // RenderSettings defaults, so a description left untouched renders exactly as before.
        float    runtimeAoRadius = 0.5f;
        float    runtimeAoBias = 0.025f;
        float    runtimeAoIntensity = 2.5f;
        float    runtimeAoThickness = 0.15f;
        // 0 = 12 samples, 1 = 20, 2 = 32 (ResolveSampleCount in SSAO_PS.hlsl).
        uint32_t runtimeAoQuality = 1u;
    };

    // Defined in RhiUIOverlay.h. Backends that render their whole frame internally call this back
    // between the scene and Present so the UI layer can draw on top; null means "no overlay".
    class IRhiFrameOverlay;

    // A back-buffer capture asked of a backend that owns its whole frame.
    //
    // The capture has to happen after the overlay and before Present: with a discard swap effect
    // the back buffer is undefined once presented, so there is nothing left to read afterwards.
    // The backend fills in the result fields before returning from ExecuteBackendFrame.
    struct RhiBackendCaptureRequest
    {
        const wchar_t* path = nullptr;
        bool completed = false;
        bool succeeded = false;
    };

    struct RhiBackendFrameDesc
    {
        RhiClearColor clearColor{};
        RhiBackendRayMarchFrameDesc rayMarch{};
        RhiBackendMeshFrameDesc mesh{};
        IRhiFrameOverlay* overlay = nullptr;
        RhiBackendCaptureRequest* capture = nullptr;
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

    // GPU-driven indexed draws. `argsBuffer` holds tightly packed records of five uint32
    // {indexCountPerInstance, instanceCount, startIndex, baseVertex(int), startInstance}, the
    // D3D12_DRAW_INDEXED_ARGUMENTS / VkDrawIndexedIndirectCommand layout (stride 20 bytes).
    // With a valid `countBuffer`, the GPU draws min(maxDrawCount, *count) records; otherwise
    // exactly maxDrawCount. Both buffers must be in RhiResourceState::IndirectArgument.
    struct RhiDrawIndexedIndirectDesc
    {
        RhiBufferHandle argsBuffer{};
        uint64_t argsOffsetInBytes = 0;
        uint32_t maxDrawCount = 1;
        RhiBufferHandle countBuffer{};
        uint64_t countOffsetInBytes = 0;
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

    struct RhiBufferCopyDesc
    {
        RhiBufferHandle source{};
        RhiBufferHandle destination{};
        uint64_t sourceOffsetInBytes = 0;
        uint64_t destinationOffsetInBytes = 0;
        uint64_t sizeInBytes = 0;
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

    struct RhiVertexBufferBinding
    {
        RhiBufferHandle buffer{};
        uint64_t        offsetInBytes = 0;
        uint32_t        strideInBytes = 0;
        uint32_t        sizeInBytes   = 0;
    };

    struct RhiIndexBufferBinding
    {
        RhiBufferHandle buffer{};
        uint64_t        offsetInBytes = 0;
        uint32_t        sizeInBytes   = 0;
        bool            is32Bit       = true;
    };

    // =========================================================================
    // Ray Tracing types
    // =========================================================================

    using RhiAccelerationStructureHandle = RhiHandle;
    using RhiRayTracingPipelineHandle    = RhiHandle;
    using RhiShaderBindingTableHandle    = RhiHandle;

    struct RhiRayTracingGeometryDesc
    {
        RhiGpuAddress vertexBufferAddress  = 0;   // GPU virtual address of vertex data
        RhiFormat     vertexFormat         = RhiFormat::R32G32B32Float;
        uint32_t      vertexCount          = 0;
        uint32_t      vertexStrideInBytes  = 0;
        RhiGpuAddress indexBufferAddress   = 0;   // GPU virtual address of index data (0 = no index buffer)
        uint32_t      indexCount           = 0;
        bool          index32Bit           = true;
        bool          opaque               = true;
    };

    struct RhiBlasDesc
    {
        const RhiRayTracingGeometryDesc* geometries    = nullptr;
        uint32_t                         geometryCount = 0;
        bool                             preferFastTrace = true;
    };

    struct RhiTlasInstanceDesc
    {
        RhiAccelerationStructureHandle blasHandle{};
        float    transform[12]  = {};  // 3×4 row-major affine transform
        uint32_t instanceID     = 0;
        uint32_t instanceMask   = 0xFF;
        uint32_t hitGroupIndex  = 0;
        bool     forceOpaque    = true;
    };

    struct RhiTlasDesc
    {
        const RhiTlasInstanceDesc* instances      = nullptr;
        uint32_t                   instanceCount  = 0;
        bool                       preferFastTrace = true;
    };

    struct RhiRayTracingShaderGroupDesc
    {
        enum class Type : uint32_t { General, TrianglesHit } type = Type::General;
        // General group: name of the shader export (raygen, miss, callable)
        const char* exportName       = nullptr;
        // TrianglesHit group: unique name for this hit group and the closest-hit shader import
        const char* hitGroupExport   = nullptr;
        const char* closestHitExport = nullptr;
    };

    struct RhiRayTracingPipelineDesc
    {
        // Pre-compiled shader library bytecode (DXIL lib_6_6 for DX12)
        const uint8_t* libraryBytecode              = nullptr;
        uint64_t       libraryBytecodeSizeInBytes   = 0;

        // Optional pre-compiled SPIR-V library bytecode (for Vulkan). When null the
        // DX12 backend uses libraryBytecode (DXIL); the Vulkan backend requires this.
        // DX12 ignores these fields, so setting them does not affect the DXIL path.
        const uint8_t* spirvBytecode                = nullptr;
        size_t         spirvBytecodeSizeInBytes     = 0;

        const RhiRayTracingShaderGroupDesc* shaderGroups     = nullptr;
        uint32_t                            shaderGroupCount = 0;

        uint32_t maxRecursionDepth     = 1;
        uint32_t maxPayloadSizeBytes   = 32;
        uint32_t maxAttributeSizeBytes = 32;
    };

    struct RhiShaderBindingTableDesc
    {
        RhiRayTracingPipelineHandle pipeline{};
        uint32_t        rayGenGroupIndex  = 0;
        const uint32_t* missGroupIndices  = nullptr;
        uint32_t        missGroupCount    = 0;
        const uint32_t* hitGroupIndices   = nullptr;
        uint32_t        hitGroupCount     = 0;
    };

    struct RhiDispatchRaysDesc
    {
        RhiShaderBindingTableHandle sbt{};
        uint32_t width  = 0;
        uint32_t height = 0;
        uint32_t depth  = 1;
    };

    // =========================================================================
    // Capabilities
    // =========================================================================

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
        // Whether device-level resource creation (buffers/textures/views/allocators/fences)
        // may be called from worker threads. True for D3D12 (device is free-threaded per
        // Microsoft docs), D3D11 (ID3D11Device, not the immediate context) and Vulkan
        // (device-level creates with per-object external sync). False for OpenGL
        // (single context bound to one thread) -> use main-thread time slicing instead.
        bool supportsThreadedResourceCreation = false;
        // Whether shader/PSO creation may run on worker threads. Same backend rationale
        // as supportsThreadedResourceCreation; D3D12 PSO creation is officially
        // free-threaded.
        bool supportsThreadedPipelineCreation = false;
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
