#pragma once

#include "Renderer/RHI/GraphicsDevice.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#if RHI_OPENGL
#include <windows.h>
#endif

namespace SasamiEngine::Render
{
#if RHI_OPENGL
    class OpenGLGraphicsDevice final : public IRHIDevice
    {
    public:
        ~OpenGLGraphicsDevice() override;

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
        bool GetOverlayDeviceContext(RhiOverlayDeviceContext& context) override;
        RhiTextureHandle CreateRhiTexture2DFromRgba8(uint32_t width,
                                                     uint32_t height,
                                                     const void* pixels,
                                                     uint32_t rowPitchBytes,
                                                     RhiFormat format) override;
        RhiTextureHandle CreateRhiTexture3DFromData(uint32_t width,
                                                    uint32_t height,
                                                    uint32_t depth,
                                                    const void* texels,
                                                    uint32_t rowPitchBytes,
                                                    uint32_t slicePitchBytes,
                                                    RhiFormat format) override;
        RhiTextureHandle CreateRhiTexture(const RhiTextureDesc& desc) override;
        RhiTextureHandle CreateRhiTextureCubeFromFaces(uint32_t faceSize,
                                                       const void* const faces[6],
                                                       uint32_t rowPitchBytes,
                                                       RhiFormat format) override;
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
        struct OpenGLRhiShader
        {
            unsigned int shader = 0;
            RhiShaderStageFlags stage = RhiShaderStageFlags::None;
            std::string entryPoint;
        };

        struct OpenGLRhiPipeline
        {
            unsigned int program = 0;
        };

        struct OpenGLRhiPipelineLayout
        {
            struct StaticSampler
            {
                unsigned int sampler = 0;
                RhiShaderStageFlags visibility = RhiShaderStageFlags::None;
                uint32_t shaderRegister = 0;
            };
            uint32_t bindingCount = 0;
            std::vector<StaticSampler> staticSamplers;
        };

        // GL binds a texture through a target-qualified name (glBindTexture(target, texture)), so a
        // view has to remember whether it names a GL_TEXTURE_2D or a GL_TEXTURE_CUBE_MAP; the
        // texture name alone is not enough to bind it correctly.
        struct OpenGLRhiTextureView
        {
            unsigned int texture = 0;
            unsigned int target = 0;
        };

        struct OpenGLRhiBufferView
        {
            unsigned int buffer = 0;
            unsigned int target = 0;
            uint64_t resourceId = 0;
        };

        void Cleanup();
        bool EnsureMeshFrameResources();
        // Reads the back buffer with glReadPixels and writes it as a PNG. Must run before
        // SwapBuffers: after the swap the back buffer contents are undefined, the same constraint
        // the DirectX 11 path documents.
        bool CaptureBackBufferToPng(const wchar_t* path);
        bool RenderMeshFrame(const RhiBackendMeshFrameDesc& desc);
        bool EnsureSkinnedMeshFrameResources();
        bool RenderSkinnedMeshFrame(const RhiBackendMeshFrameDesc& desc);
        bool EnsureRayMarchFrameResources();
        bool RenderRayMarchFrame(const RhiBackendRayMarchFrameDesc& desc);

        // --- Native ambient occlusion ---
        // Two passes, each owning its own file: the prepass renders the depth and world-normal
        // buffers that the native path has no GBuffer to supply (OpenGLGraphicsDeviceAoPrepass.cpp),
        // and the SSAO pass consumes them (OpenGLGraphicsDeviceSsao.cpp). The mesh programs then
        // sample the result as runtimeAo, the same term DX12's DeferredLighting_PS.hlsl reads.
        bool EnsureAoPrepassTargets(uint32_t width, uint32_t height);
        bool EnsureAoPrepassPrograms();
        bool RenderAoPrepass(const RhiBackendMeshFrameDesc& desc);
        void ReleaseAoPrepassResources();
        bool EnsureSsaoTargets(uint32_t width, uint32_t height);
        bool EnsureSsaoPrograms();
        // Runs the prepass, the generation pass and the blur. Sets m_runtimeAoTexture, which is
        // what the mesh programs sample, and m_ssaoValid, which gates whether they sample it.
        void RenderNativeAmbientOcclusion(const RhiBackendMeshFrameDesc& desc);
        void ReleaseSsaoResources();
        // Optional grey-scale view of the finished AO, for SASAMI_GL_AO_DEBUG=1
        // (OpenGLGraphicsDeviceAoDebug.cpp). Diagnostics only, and a no-op unless the variable is
        // set, so the caller does not test it separately.
        void RenderAoDebugView(const RhiBackendMeshFrameDesc& desc);
        void ReleaseAoDebugResources();

        HWND m_hwnd = nullptr;
        HDC m_hdc = nullptr;
        HGLRC m_context = nullptr;
        RhiBackendCapabilities m_capabilities{};
        CommandQueue m_emptyGraphicsQueue;
        CommandQueue m_emptyComputeQueue;
        SwapChain m_emptySwapChain;
        uint64_t m_nextRhiResourceHandle = 1;
        uint64_t m_nextRhiDescriptorHandle = 1;
        uint64_t m_nextRhiShaderHandle = 1;
        uint64_t m_nextRhiPipelineLayoutHandle = 1;
        uint64_t m_nextRhiPipelineHandle = 1;
        std::unordered_map<uint64_t, unsigned int> m_rhiTextures;
        std::unordered_map<uint64_t, unsigned int> m_rhiBuffers;
        std::unordered_map<uint64_t, uint64_t> m_rhiBufferSizes;
        std::unordered_map<uint64_t, unsigned int> m_rhiBufferTargets;
        std::unordered_map<uint64_t, OpenGLRhiShader> m_rhiShaders;
        std::unordered_map<uint64_t, OpenGLRhiPipelineLayout> m_rhiPipelineLayouts;
        std::unordered_map<uint64_t, OpenGLRhiPipeline> m_rhiPipelines;
        std::unordered_map<uint64_t, OpenGLRhiTextureView> m_rhiTextureViews;
        std::unordered_map<uint64_t, OpenGLRhiBufferView> m_rhiBufferViews;
        std::unordered_map<uint64_t, RhiFormat> m_rhiTextureUavs;
        std::unordered_map<uint64_t, bool> m_rhiBufferUavs;
        unsigned int m_meshProgram = 0;
        int m_meshMvpLocation = -1;
        int m_meshBaseColorLocation = -1;
        int m_meshLightDirIntensityLocation = -1;
        int m_meshLightColorLocation = -1;
        int m_meshEmissiveRoughnessLocation = -1;
        int m_meshAlbedoTextureLocation = -1;
        int m_meshHasAlbedoTextureLocation = -1;
        int m_meshOcclusionTextureLocation = -1;
        int m_meshHasOcclusionTextureLocation = -1;
        int m_meshAmbientCubeLocation = -1;
        int m_meshHasAmbientCubeLocation = -1;
        // Screen-space AO from the native SSAO pass, on texture unit 3 (0 albedo, 1 occlusion,
        // 2 ambient cube). u_renderSize turns gl_FragCoord into the UV that samples it.
        int m_meshRuntimeAoTextureLocation = -1;
        int m_meshHasRuntimeAoTextureLocation = -1;
        int m_meshRenderSizeLocation = -1;
        int m_meshExposureLocation = -1;
        int m_meshModelLocation = -1;
        int m_meshCameraPosLocation = -1;
        int m_meshMetallicLocation = -1;
        int m_meshEnableIblLocation = -1;
        // Reused per-frame draw-order scratch buffers for RenderMeshFrame's opaque/transparent
        // split; cleared and refilled every frame instead of reallocated to avoid per-frame heap churn.
        std::vector<uint32_t> m_meshOpaqueDrawOrder;
        std::vector<uint32_t> m_meshTransparentDrawOrder;

        unsigned int m_skinnedMeshProgram = 0;
        int m_skinnedMeshMvpLocation = -1;
        int m_skinnedMeshBaseColorLocation = -1;
        int m_skinnedMeshLightDirIntensityLocation = -1;
        int m_skinnedMeshLightColorLocation = -1;
        int m_skinnedMeshEmissiveRoughnessLocation = -1;
        int m_skinnedMeshAlbedoTextureLocation = -1;
        int m_skinnedMeshHasAlbedoTextureLocation = -1;
        // The skinned program compiles the same fragment shader as the static one, which samples
        // occlusion from texture unit 1; without pointing this sampler uniform at unit 1 ourselves
        // it would default to unit 0 and read the albedo texture instead.
        int m_skinnedMeshOcclusionTextureLocation = -1;
        int m_skinnedMeshHasOcclusionTextureLocation = -1;
        int m_skinnedMeshAmbientCubeLocation = -1;
        int m_skinnedMeshHasAmbientCubeLocation = -1;
        int m_skinnedMeshBoneMatricesLocation = -1;
        // Same three as the static path: the skinned program compiles the same fragment shader.
        int m_skinnedMeshRuntimeAoTextureLocation = -1;
        int m_skinnedMeshHasRuntimeAoTextureLocation = -1;
        int m_skinnedMeshRenderSizeLocation = -1;
        int m_skinnedMeshExposureLocation = -1;
        int m_skinnedMeshModelLocation = -1;
        int m_skinnedMeshCameraPosLocation = -1;
        int m_skinnedMeshMetallicLocation = -1;
        int m_skinnedMeshEnableIblLocation = -1;

        // Analytic sky for the native mesh path; without it the background is the flat clear.
        unsigned int m_nativeSkyProgram  = 0;
        bool         m_nativeSkyFailed   = false;
        int m_skyLocInvVP = -1, m_skyLocSunDir = -1, m_skyLocSunColor = -1, m_skyLocSunI = -1, m_skyLocExposure = -1, m_skyLocRenderSize = -1;
        // Environment cube sampler and its "a real cube is bound" gate. The sky pass binds no
        // other texture, so the cube goes on unit 0.
        int m_skyLocEnvCube = -1, m_skyLocHasEnvCube = -1;
        bool EnsureNativeSkyProgram();
        void RenderNativeSky(const RhiBackendMeshFrameDesc& desc);

        // Depth + world-normal prepass targets, sized to the render resolution and rebuilt when it
        // changes. The default framebuffer's depth buffer cannot be sampled as a texture, which is
        // the whole reason this offscreen pair exists.
        unsigned int m_aoPrepassFbo       = 0;
        unsigned int m_aoNormalTexture    = 0;
        unsigned int m_aoDepthTexture     = 0;
        uint32_t     m_aoTargetWidth      = 0;
        uint32_t     m_aoTargetHeight     = 0;
        unsigned int m_aoPrepassProgram        = 0;
        unsigned int m_aoPrepassSkinnedProgram = 0;
        bool         m_aoPrepassFailed         = false;
        int m_aoPrepassMvpLocation       = -1;
        int m_aoPrepassModelLocation     = -1;
        int m_aoPrepassAlbedoLocation    = -1;
        int m_aoPrepassHasAlbedoLocation = -1;
        int m_aoPrepassSkinnedMvpLocation          = -1;
        int m_aoPrepassSkinnedModelLocation        = -1;
        int m_aoPrepassSkinnedAlbedoLocation       = -1;
        int m_aoPrepassSkinnedHasAlbedoLocation    = -1;
        int m_aoPrepassSkinnedBoneMatricesLocation = -1;

        // SSAO generation and blur targets. m_runtimeAoTexture names whichever of the two the mesh
        // programs should sample, and m_ssaoValid is per-frame: they only sample it when this frame
        // actually refreshed it, so a frame that skipped generation falls back to no occlusion
        // instead of reusing the occlusion of whatever the camera last looked at.
        unsigned int m_ssaoFbo           = 0;
        unsigned int m_ssaoTexture       = 0;
        unsigned int m_ssaoBlurFbo       = 0;
        unsigned int m_ssaoBlurTexture   = 0;
        unsigned int m_runtimeAoTexture  = 0;
        uint32_t     m_ssaoTargetWidth   = 0;
        uint32_t     m_ssaoTargetHeight  = 0;
        unsigned int m_ssaoProgram     = 0;
        unsigned int m_ssaoBlurProgram = 0;
        bool         m_ssaoFailed      = false;
        bool         m_ssaoValid       = false;
        int m_ssaoLocCameraPV      = -1;
        int m_ssaoLocCameraInvPV   = -1;
        int m_ssaoLocParams        = -1;
        int m_ssaoLocCameraPos     = -1;
        int m_ssaoLocDepthTexture  = -1;
        int m_ssaoLocNormalTexture = -1;
        int m_ssaoBlurLocScreenSize    = -1;
        int m_ssaoBlurLocSsaoTexture   = -1;
        int m_ssaoBlurLocDepthTexture  = -1;
        int m_ssaoBlurLocNormalTexture = -1;

        // SASAMI_GL_AO_DEBUG view. Built on first use, since most runs never ask for it.
        unsigned int m_aoDebugProgram = 0;
        bool         m_aoDebugFailed  = false;
        int m_aoDebugLocAoTexture = -1;
        // Whether glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE) was applied this run. Without it
        // clip z maps to depth [0.5, 1.0] instead of [0, 1], which still renders correctly but
        // makes reconstructing a world position from a sampled depth wrong -- so SSAO is skipped.
        bool m_depthZeroToOne = false;

        unsigned int m_rayMarchProgram   = 0;
        int m_rmLocInvVP                 = -1;
        int m_rmLocCamPos                = -1;
        int m_rmLocTime                  = -1;
        int m_rmLocSunDir                = -1;
        int m_rmLocSunI                  = -1;
        int m_rmLocSunColor              = -1;
        int m_rmLocCloudCover            = -1;
        int m_rmLocRenderW               = -1;
        int m_rmLocRenderH               = -1;
        int m_rmLocFluidMode             = -1;
        int m_rmLocCloudDensity          = -1;
        int m_rmLocExtra0                = -1;
        int m_rmLocExtra1                = -1;
        int m_rmLocExtra2                = -1;

        friend class OpenGLRhiCommandEncoder;
    };
#endif
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
