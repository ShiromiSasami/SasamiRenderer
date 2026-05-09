#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/RayTracing/RayTracingScene.h"

#include <cstdint>
#include <memory>
#include <vector>

// Forward-declare NRD Integration to avoid pulling NRI/NRD headers into every TU.
namespace nrd { struct Integration; }

namespace SasamiRenderer
{
    //
    // GpuSoftwareRayTracer
    // Builds BVH on CPU (SAH, 32-byte nodes), uploads to GPU once per dirty frame,
    // then dispatches Compute Shaders for shadow map and reflection passes.
    // Drop-in successor to SoftwareRayTracer for the raster pipeline.
    //
    class GpuSoftwareRayTracer
    {
    public:
        // ---- Public descriptor types (mirror SoftwareRayTracer for drop-in use) ----

        struct DirectionalShadowMapDesc
        {
            uint32_t width  = 0;
            uint32_t height = 0;
            uint32_t arraySlice = 0;
            uint32_t constantBufferSlot = 0;
            float depthBias = 0.0035f;
            float lightViewProjection[16] = {
                1,0,0,0,
                0,1,0,0,
                0,0,1,0,
                0,0,0,1,
            };
            float inverseLightViewProjection[16] = {
                1,0,0,0,
                0,1,0,0,
                0,0,1,0,
                0,0,0,1,
            };
        };

        struct ReflectionTextureDesc
        {
            uint32_t width  = 0;
            uint32_t height = 0;
            uint32_t updatePhaseCount  = 1;
            uint32_t updatePhaseIndex  = 0;
            float    maxSurfaceRoughness      = 0.45f;
            float    minReflectionEnergy      = 0.02f;
            float    maxPrimaryHitDistance    = 35.0f;
            float    maxReflectionTraceDistance = 60.0f;
            uint32_t samplesPerPixel          = 1;
            uint32_t samplingMode             = 2;  // 0=IS Only, 1=NEE Only, 2=MIS (IS+NEE)
            bool     preserveExistingPixels   = false;
            bool     iblEnabled               = false;
            float    iblIntensity             = 1.0f;
            float    iblPrefilterMaxMip       = 0.0f;
            RayTracingFrameDesc frameDesc{};

            // G-Buffer inputs (t6-t8 in the reflection CS).
            // If null, null SRVs are bound and the shader outputs black for all pixels.
            ID3D12Resource* gbufferNormalTex   = nullptr; // R16G16B16A16_FLOAT: N*0.5+0.5 + camera distance in W
            ID3D12Resource* gbufferMaterialTex = nullptr; // R8G8B8A8_UNORM:    roughness.r, metallic.g
            ID3D12Resource* gbufferAlbedoTex   = nullptr; // R8G8B8A8_UNORM:    base color
            ID3D12Resource* iblPrefilterTex    = nullptr; // TextureCube used for reflection-ray misses

            // Native resolution of the G-Buffer textures above.
            // May differ from width/height when reflection is rendered at reduced resolution.
            // Used by the CS to scale Load() coordinates so pixels map to the correct
            // G-Buffer texel rather than a top-left-biased subset of the screen.
            uint32_t gbufferWidth  = 0;
            uint32_t gbufferHeight = 0;

            // Maximum number of reflection bounces per sample ray (1 = single bounce).
            uint32_t maxBounces = 1;
            uint32_t debugView = 0;

            // True when the camera moved since the last frame.
            // Causes forceFullRefresh and disables temporal history accumulation.
            bool cameraChanged = false;

            bool     denoiserEnabled = true;
            float    temporalAlpha = 0.1f;
            uint32_t atrousIterations = 3u;
            float    atrousPhiDepth = 0.35f;
        };

        struct AmbientOcclusionTextureDesc
        {
            uint32_t width = 0;
            uint32_t height = 0;
            float inverseViewProjection[16] = {
                1,0,0,0,
                0,1,0,0,
                0,0,1,0,
                0,0,0,1,
            };
            float cameraPosition[3] = {};
            float tMin = 0.001f;
            uint32_t gbufferWidth = 0;
            uint32_t gbufferHeight = 0;
            float radius = 0.75f;
            float power = 1.0f;
            uint32_t sampleCount = 8;
            uint32_t frameIndex = 0;

            ID3D12Resource* gbufferNormalTex = nullptr;
            ID3D12Resource* gbufferMaterialTex = nullptr;
            ID3D12Resource* gbufferAlbedoTex = nullptr;
        };

        // ---- SWRT mode selection ----
        enum class SwrtMode : uint32_t
        {
            Legacy  = 0,   // Single-pass NEE (original)
            ReSTIR  = 1,   // ReSTIR DI + SVGF 6-pass pipeline
        };

        // GPU virtual addresses of internal BVH buffers.
        // Exposed so external compute passes (e.g. IrradianceProbeGrid) can bind
        // the same BVH without duplicating GPU memory.
        struct BvhGpuAddresses
        {
            D3D12_GPU_VIRTUAL_ADDRESS bvhNodes  = 0;
            D3D12_GPU_VIRTUAL_ADDRESS triangles = 0;
            D3D12_GPU_VIRTUAL_ADDRESS meshInfo  = 0;
            D3D12_GPU_VIRTUAL_ADDRESS instances = 0;
            D3D12_GPU_VIRTUAL_ADDRESS tlasNodes = 0;
            D3D12_GPU_VIRTUAL_ADDRESS materials = 0;
            bool valid = false;
        };
        BvhGpuAddresses GetBvhGpuAddresses() const;

        GpuSoftwareRayTracer();
        ~GpuSoftwareRayTracer();

        GpuSoftwareRayTracer(const GpuSoftwareRayTracer&) = delete;
        GpuSoftwareRayTracer& operator=(const GpuSoftwareRayTracer&) = delete;
        GpuSoftwareRayTracer(GpuSoftwareRayTracer&&) = delete;
        GpuSoftwareRayTracer& operator=(GpuSoftwareRayTracer&&) = delete;

        SwrtMode GetMode() const { return m_swrtMode; }
        void     SetMode(SwrtMode mode) { m_swrtMode = mode; }

        // Called once at startup.
        bool Initialize(IRHIDevice& device);

        // Must be called every frame before Render* calls.
        // Rebuilds BVH if scene is dirty and re-uploads GPU buffers.
        void UpdateScene(const RayTracingScene& scene, IRHIDevice& device, CommandList& cmdList);

        // Dispatches shadow compute shader; outTexture must have
        // D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS and be in UAV state on entry.
        // On return the texture is left in UAV state (caller must transition).
        bool RenderDirectionalShadowMap(const DirectionalShadowMapDesc& desc,
                                        IRHIDevice& device,
                                        CommandList& cmdList,
                                        Resource& outTexture,
                                        RayTracingRuntimeStats& outStats);

        // Dispatches reflection compute shader; same UAV state contract as shadow.
        bool RenderReflectionTexture(const ReflectionTextureDesc& desc,
                                     IRHIDevice& device,
                                     CommandList& cmdList,
                                     Resource& outTexture,
                                     RayTracingRuntimeStats& outStats);

        // Populates the internal ReSTIR frame data used by RenderShadowReSTIR.
        // scratchTexture must be in UAV state on entry and is left in UAV state.
        bool PrepareReSTIRFrame(const ReflectionTextureDesc& desc,
                                IRHIDevice& device,
                                CommandList& cmdList,
                                Resource& scratchTexture,
                                RayTracingRuntimeStats& outStats);

        bool RenderAmbientOcclusionTexture(const AmbientOcclusionTextureDesc& desc,
                                           IRHIDevice& device,
                                           CommandList& cmdList,
                                           Resource& outTexture,
                                           RayTracingRuntimeStats& outStats);

        // Dispatches ReSTIR shadow compute shader (SWRT_Shadow_ReSTIR_CS).
        // Requires RenderReflectionTexture (ReSTIR mode) to have run first this frame
        // so that m_gBufferTexture is populated.
        // outTexture must have UAV flag and be in UAV state on entry; left in UAV state on return.
        bool RenderShadowReSTIR(const ReflectionTextureDesc& desc,
                                IRHIDevice& device,
                                CommandList& cmdList,
                                Resource& outTexture,
                                RayTracingRuntimeStats& outStats);

    public:
        // ---- CPU BVH node (32 bytes, matches HLSL BvhNode in SWRT_Common.hlsli) ----
        // Public so build helpers in the .cpp anonymous namespace can use the type.
        struct BvhNode
        {
            float   boundsMin[3] = {};
            int32_t leftChild    = -1;   // <0 → leaf: firstIndex = ~leftChild
            float   boundsMax[3] = {};
            int32_t rightOrCount = 0;    // leaf: count; interior: right child index

            bool     IsLeaf()        const { return leftChild < 0; }
            uint32_t GetFirstIndex() const { return static_cast<uint32_t>(~leftChild); }
            uint32_t GetIndexCount() const { return static_cast<uint32_t>(rightOrCount); }
            int32_t  GetLeftChild()  const { return leftChild; }
            int32_t  GetRightChild() const { return rightOrCount; }
        };
        static_assert(sizeof(BvhNode) == 32u, "BvhNode must be 32 bytes to match HLSL");

        // ---- TLAS node (same layout as BvhNode) ----
        struct TlasNode
        {
            float   boundsMin[3] = {};
            int32_t leftChild    = -1;   // <0 → leaf: firstInstance = ~leftChild
            float   boundsMax[3] = {};
            int32_t rightOrCount = 0;    // leaf: instanceCount; interior: right child

            bool     IsLeaf()           const { return leftChild < 0; }
            uint32_t GetFirstInstance() const { return static_cast<uint32_t>(~leftChild); }
            uint32_t GetInstanceCount() const { return static_cast<uint32_t>(rightOrCount); }
            int32_t  GetLeftChild()     const { return leftChild; }
            int32_t  GetRightChild()    const { return rightOrCount; }
        };
        static_assert(sizeof(TlasNode) == 32u, "TlasNode must be 32 bytes to match HLSL");

    private:

        // ---- GPU triangle (Möller-Trumbore precomputed + normals + UVs) ----
        // Matches HLSL GpuTriangle in SWRT_Common.hlsli (128 bytes total)
        struct GpuTriangle
        {
            float p0[3];    float pad0;
            float edge1[3]; float pad1;
            float edge2[3]; float pad2;
            float n0[3];    float pad3;
            float n1[3];    float pad4;
            float n2[3];    float pad5;
            float uv0[2];   float uv1[2];
            float uv2[2];   float pad6[2];
        };
        static_assert(sizeof(GpuTriangle) == 128u, "GpuTriangle must be 128 bytes to match HLSL");

        // ---- Mesh info (matches HLSL GpuMeshInfo, 16 bytes) ----
        struct GpuMeshInfo
        {
            uint32_t nodeOffset;
            uint32_t triOffset;
            uint32_t pad0;
            uint32_t pad1;
        };
        static_assert(sizeof(GpuMeshInfo) == 16u, "GpuMeshInfo must be 16 bytes to match HLSL");

        // ---- Instance info (matches HLSL GpuInstanceInfo, 176 bytes) ----
        struct GpuInstanceInfo
        {
            uint32_t meshIndex;
            uint32_t materialIndex;
            uint32_t pad0;
            uint32_t pad1;
            float    world[16];
            float    invWorld[16];
            float    worldBoundsMin[3]; float pad2;
            float    worldBoundsMax[3]; float pad3;
        };
        static_assert(sizeof(GpuInstanceInfo) == 176u, "GpuInstanceInfo must be 176 bytes to match HLSL");

        // ---- Material (matches HLSL GpuMaterial, 64 bytes) ----
        struct GpuMaterial
        {
            float    baseColor[4];
            float    roughness;
            float    metallic;
            float    transmission;
            float    ior;
            float    specularColor[3];
            float    workflow;
            float    emissive[3];
            float    occlusionStrength;
        };
        static_assert(sizeof(GpuMaterial) == 64u, "GpuMaterial must be 64 bytes to match HLSL");

        // ---- Per-mesh CPU acceleration structure ----
        struct MeshAcceleration
        {
            std::vector<BvhNode>     nodes;
            std::vector<GpuTriangle> triangles;
            uint32_t nodeOffset = 0;
            uint32_t triOffset  = 0;
        };

        // ---- Shadow cbuffer (matches HLSL ShadowFrameConstants, 148 bytes) ----
        struct alignas(256) ShadowFrameConstants
        {
            float    invLightVP[16];
            float    lightVP[16];
            uint32_t width;
            uint32_t height;
            float    tMin;
            float    depthBias;
            uint32_t arraySlice;
        };
        static_assert(sizeof(ShadowFrameConstants) >= 148u);

        // ---- Reflection cbuffer (matches HLSL ReflectionFrameConstants) ----
        struct alignas(256) ReflectionFrameConstants
        {
            float    invVP[16];
            float    cameraPos[3];
            float    tMin;
            uint32_t renderWidth;
            uint32_t renderHeight;
            uint32_t updatePhaseCount;
            uint32_t updatePhaseIndex;
            float    maxSurfaceRoughness;
            float    maxPrimaryHitDistance;
            float    maxReflectionDistance;
            float    minReflectionEnergy;
            float    dirLightDir[3];
            float    dirLightIntensity;
            float    dirLightColor[3];
            float    shadowBias;
            float    ambientColor[3];
            float    ambientIntensity;
            float    iblEnabled;
            float    iblIntensity;
            float    iblPrefilterMaxMip;
            float    pad0;
            uint32_t pointLightCount;
            uint32_t spotLightCount;
            uint32_t samplesPerPixel;
            uint32_t samplingMode;  // 0=IS Only, 1=NEE Only, 2=MIS (IS+NEE)
            uint32_t frameIndex;
            uint32_t gbufferWidth;   // native G-Buffer width  (may differ from renderWidth)
            uint32_t gbufferHeight;  // native G-Buffer height (may differ from renderHeight)
            uint32_t maxBounces;     // max reflection bounces per sample (1 = single-bounce)
            uint32_t debugView;
            uint32_t pad1;
            uint32_t pad2;
            uint32_t pad3;
        };
        static_assert(sizeof(ReflectionFrameConstants) >= 208u);

        struct alignas(256) ReflectionTemporalReprojectionConstants
        {
            float invVP[16];
            float prevVP[16];
            float cameraPos[3];
            float pad0;
            float prevCameraPos[3];
            float pad1;
        };
        static_assert(sizeof(ReflectionTemporalReprojectionConstants) == 256u);

        struct alignas(256) AmbientOcclusionFrameConstants
        {
            float invVP[16];
            float cameraPos[3];
            float tMin;
            uint32_t renderWidth;
            uint32_t renderHeight;
            uint32_t sampleCount;
            uint32_t frameIndex;
            float radius;
            float power;
            uint32_t gbufferWidth;
            uint32_t gbufferHeight;
            uint32_t pad0;
            uint32_t pad1;
        };
        static_assert(sizeof(AmbientOcclusionFrameConstants) >= 112u);

        // ---- ReSTIR / SVGF cbuffer (matches all SWRT ReSTIR HLSL shaders, 256 bytes) ----
        struct alignas(256) ReSTIRFrameConstants
        {
            float    invVP[16];           // row_major float4x4 g_invVP
            float    prevVP[16];          // row_major float4x4 g_prevVP
            float    cameraPos[3];        // float3 g_cameraPos
            float    tMin;
            uint32_t renderWidth;
            uint32_t renderHeight;
            uint32_t frameIndex;
            uint32_t reservoirWidth;
            float    temporalAlpha;
            float    phiColor;
            float    phiNormal;
            float    phiDepth;
            float    stepWidth;
            float    maxSurfaceRoughness;
            float    maxPrimaryHitDistance;
            float    minReflectionEnergy;
            float    dirLightDir[3];
            float    dirLightIntensity;
            float    dirLightColor[3];
            float    shadowBias;
            float    ambientColor[3];
            float    ambientIntensity;
            uint32_t pointLightCount;
            uint32_t spotLightCount;
            uint32_t cbPad0;
            uint32_t cbPad1;
        };
        static_assert(sizeof(ReSTIRFrameConstants) == 256u);

        // ---- GPU light structs for ReSTIR passes ----
        struct GpuPointLightRT
        {
            float pos[3]; float range;
            float colorIntensity[3]; float pad;
        };
        static_assert(sizeof(GpuPointLightRT) == 32u);

        struct GpuSpotLightRT
        {
            float pos[3]; float range;
            float dir[3]; float cosInner;
            float colorIntensity[3]; float cosOuter;
        };
        static_assert(sizeof(GpuSpotLightRT) == 48u);

        // ---- BVH / scene CPU state ----
        RayTracingScene              m_scene;
        std::vector<MeshAcceleration> m_meshAccelerations;
        std::vector<TlasNode>         m_topLevelNodes;
        std::vector<uint32_t>         m_topLevelInstanceOrder;
        uint64_t m_bvhGeometryVersion  = 0;
        uint64_t m_bvhMaterialVersion  = 0;
        uint64_t m_bvhInstanceVersion  = 0;

        void RebuildAccelerationStructures();
        void BuildMeshBvhSah(uint32_t meshIdx);
        void RebuildTlas();

        // ---- GPU buffers ----
        Resource m_bvhNodesBuffer;      // SRV t0
        Resource m_triangleBuffer;      // SRV t1
        Resource m_meshInfoBuffer;      // SRV t2
        Resource m_instanceBuffer;      // SRV t3
        Resource m_tlasBuffer;          // SRV t4
        Resource m_materialBuffer;      // SRV t5
        Resource m_frameConstantsBuffer;
        uint8_t* m_frameConstantsMapped = nullptr;

        uint64_t m_uploadedGeometryVersion = UINT64_MAX;
        uint64_t m_uploadedMaterialVersion = UINT64_MAX;
        uint64_t m_uploadedInstanceVersion  = UINT64_MAX;

        // ---- Descriptor heap (GPU-visible CBV_SRV_UAV) ----
        // [0..5]   = BVH SRVs (t0-t5, permanent)
        // [6]      = shadow output UAV (legacy)
        // [7]      = reflection output UAV (legacy)
        // [8..11]  = ReSTIR scratch SRVs (t6-t9, set per-pass)
        // [12..13] = ReSTIR scratch UAVs (u0-u1, set per-pass)
        DescriptorHeap m_descHeap;
        UINT           m_descIncrementSize = 0;

        static constexpr UINT kSrvCount           = 6u;
        static constexpr UINT kShadowUavSlot      = 6u;
        static constexpr UINT kReflectionUavSlot  = 7u;
        static constexpr UINT kScratchSrvBase     = 8u;   // also used as G-Buffer SRV base (legacy path)
        static constexpr UINT kScratchSrvCount    = 4u;
        static constexpr UINT kScratchUavBase     = 12u;
        static constexpr UINT kScratchUavCount    = 3u;   // u0-u2 (NRD Pack needs 3)
        static constexpr UINT kGBufferSrvBase     = 8u;   // t6-t9: Normal, Material, Albedo, IBL Prefilter
        static constexpr UINT kGBufferSrvCount    = 4u;
        static constexpr UINT kReSTIRPassDescBase   = 14u;  // first per-pass descriptor slot
        static constexpr UINT kReSTIRPassDescStride = 7u;   // 4 SRV + 3 UAV per pass
        static constexpr UINT kReSTIRPassCount      = 8u;   // Pass0-Pass5(NRD Pack) + Pass6/7(A-Trous ping/pong)
        static constexpr UINT kReSTIRFrameSlots     = 2u;   // double-buffer to avoid WaitForGPU
        static constexpr UINT kTotalDescriptors     = kReSTIRPassDescBase + kReSTIRFrameSlots * kReSTIRPassCount * kReSTIRPassDescStride; // 14+112=126

        // ---- Legacy Compute PSOs ----
        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12PipelineState> m_shadowPso;
        ComPtr<ID3D12PipelineState> m_reflectionPso;
        ComPtr<ID3D12PipelineState> m_aoPso;

        // ---- Temporal EMA pass (Legacy reflection only) ----
        ComPtr<ID3D12RootSignature> m_reflTemporalRootSignature;
        ComPtr<ID3D12PipelineState> m_reflTemporalPso;
        ComPtr<ID3D12RootSignature> m_reflAtrousRootSignature;
        ComPtr<ID3D12PipelineState> m_reflAtrousPso;
        Resource m_reflHistoryA;     // R16G16B16A16_FLOAT, ping-pong A
        Resource m_reflHistoryB;     // R16G16B16A16_FLOAT, ping-pong B
        Resource m_reflHistoryMetaA; // R16G16B16A16_FLOAT, encoded normal + camera distance
        Resource m_reflHistoryMetaB; // R16G16B16A16_FLOAT, encoded normal + camera distance
        Resource m_reflHistoryMaterialA; // R16G16B16A16_FLOAT, roughness + metallic + AO
        Resource m_reflHistoryMaterialB; // R16G16B16A16_FLOAT, roughness + metallic + AO
        Resource m_reflAtrousScratch; // R16G16B16A16_FLOAT, spatial filter scratch
        uint32_t m_reflHistoryWidth  = 0;
        uint32_t m_reflHistoryHeight = 0;
        bool     m_reflHistoryPingA  = true;   // true = A is write side this frame
        // Descriptor slots 14-22 (ReSTIR range is unused in Legacy mode):
        //   14 = SRV current-frame reflection (t0)
        //   15 = SRV history read side        (t1)
        //   16 = SRV current surface metadata (t2)
        //   17 = SRV history surface metadata (t3)
        //   18 = SRV current material metadata (t4)
        //   19 = SRV history material metadata (t5)
        //   20 = UAV history write side       (u0)
        //   21 = UAV surface metadata write   (u1)
        //   22 = UAV material metadata write  (u2)
        static constexpr UINT kTemporalSrvBase = 14u;  // t0-t5
        static constexpr UINT kTemporalUavBase = 20u;  // u0-u2

        // ---- ReSTIR root signature + PSOs ----
        ComPtr<ID3D12RootSignature> m_restirRootSignature;
        ComPtr<ID3D12PipelineState> m_restirInitialPso;
        ComPtr<ID3D12PipelineState> m_restirTemporalPso;
        ComPtr<ID3D12PipelineState> m_restirSpatialPso;
        ComPtr<ID3D12PipelineState> m_restirShadePso;
        ComPtr<ID3D12PipelineState> m_nrdPackPso;
        ComPtr<ID3D12PipelineState> m_shadowReSTIRPso;
        ComPtr<ID3D12PipelineState> m_restirATrousPso;  // A-Trous spatial denoiser

        // ---- ReSTIR GPU resources ----
        Resource m_gBufferTexture;         // R16G16B16A16_FLOAT  W×H
        Resource m_prevGBufferTexture;     // R16G16B16A16_FLOAT  W×H
        Resource m_reservoirBuffer[3];     // RWStructuredBuffer<Reservoir> W×H  [0]=initial/spatial, [1]/[2]=temporal ping-pong
        Resource m_prevColorTexture;       // R16G16B16A16_FLOAT  W×H
        Resource m_shadedColorTexture;     // R16G16B16A16_FLOAT  W×H  (Pass4 output)
        // ---- NRD textures ----
        Resource m_nrdDiffIn;             // RGBA16F – IN_DIFF_RADIANCE_HITDIST
        Resource m_nrdViewZ;              // R16F    – IN_VIEWZ
        Resource m_nrdNormalRoughness;    // RGBA8   – IN_NORMAL_ROUGHNESS
        Resource m_nrdMotionVec;          // RG16F   – IN_MV  (zeroed, no motion vecs yet)
        Resource m_nrdDiffOut;            // RGBA16F – OUT_DIFF_RADIANCE_HITDIST
        // ---- A-Trous denoising ----
        Resource m_atrousPingPong;        // RGBA16F – ping-pong buffer for A-Trous passes
        bool     m_atrousReady = false;

        // ---- Light data upload buffer (persistently mapped, per-frame) ----
        // Layout: GpuPointLightRT[kMaxPointLights] | GpuSpotLightRT[kMaxSpotLights]
        Resource m_lightDataBuffer;
        uint8_t* m_lightDataMapped = nullptr;
        static constexpr uint32_t kMaxPointLights = 512u;
        static constexpr uint32_t kMaxSpotLights  = 256u;

        // ---- ReSTIR constants buffer (7 × 256 bytes, persistently mapped) ----
        // Slot 0: base constants (passes 1-5)
        // Slots 1-5: A-Trous variants (stepWidth=1,2,4,8,16)
        // Slot 6: shadow ReSTIR constants
        Resource m_restirConstantsBuffer;
        uint8_t* m_restirConstantsMapped = nullptr;
        static constexpr uint32_t kReSTIRCbufSlotCount = 7u;

        // ---- ReSTIR frame state ----
        uint32_t m_restirWidth       = 0;
        uint32_t m_restirHeight      = 0;
        uint32_t m_restirFrameIndex  = 0;
        bool     m_restirPipelineReady = false;
        bool     m_nrdReady            = false;
        float    m_prevVP[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        float    m_prevReflectionCameraPos[3] = {};

        // ---- NRD integration ----
        std::unique_ptr<nrd::Integration> m_nrdIntegration;

        SwrtMode m_swrtMode = SwrtMode::Legacy;
        bool m_initialized = false;

        // ---- Private helpers ----
        bool CreatePipelines(IRHIDevice& device);
        bool CreateReSTIRPipelines(IRHIDevice& device);
        bool AllocateReSTIRBuffers(IRHIDevice& device, uint32_t w, uint32_t h);
        bool UploadBvhBuffers(IRHIDevice& device);
        bool AllocateNrdBuffers(IRHIDevice& device, uint32_t w, uint32_t h);
        bool InitializeNrdIntegration(IRHIDevice& device, uint32_t w, uint32_t h);
        bool CreateOrUpdateBuffer(IRHIDevice& device,
                                  const void* data, UINT64 byteSize,
                                  Resource& outBuffer);
        void UpdateTextureUav(ID3D12Device* dev, Resource& texture,
                              DXGI_FORMAT format, UINT slotIndex, UINT arraySize = 1u);
        void FillShadowConstants(const DirectionalShadowMapDesc& desc,
                                 ShadowFrameConstants& out) const;
        void FillReflectionConstants(const ReflectionTextureDesc& desc,
                                     ReflectionFrameConstants& out) const;
        void FillAmbientOcclusionConstants(const AmbientOcclusionTextureDesc& desc,
                                           AmbientOcclusionFrameConstants& out) const;
        void FillReSTIRConstants(const ReflectionTextureDesc& desc,
                                  float stepWidth, ReSTIRFrameConstants& out) const;
        void SetScratchTexSRV(ID3D12Device* dev, Resource& tex,
                               DXGI_FORMAT fmt, UINT slot);
        void SetScratchTexUAV(ID3D12Device* dev, Resource& tex,
                               DXGI_FORMAT fmt, UINT slot);
        bool RenderReflectionTextureReSTIR(const ReflectionTextureDesc& desc,
                                            IRHIDevice& device,
                                            CommandList& cmdList,
                                            Resource& outTexture,
                                            RayTracingRuntimeStats& outStats);
    };

} // namespace SasamiRenderer
