#pragma once

#include "Renderer/Backends/DirectX11/Dx11DepthNormalPrepass.h"
#include "Renderer/Backends/DirectX11/Dx11ShadowMap.h"
#include "Renderer/Backends/DirectX11/Dx11SsaoGenerator.h"
#include "Renderer/RHI/GraphicsDevice.h"

#include <cstddef>
#include <unordered_map>
#include <vector>

#if RHI_DIRECTX11
#include <d3d11.h>
#include <windows.h>
#endif

namespace SasamiEngine::Render
{
#if RHI_DIRECTX11
    class Dx11GraphicsDevice final : public IRHIDevice
    {
    public:
        ~Dx11GraphicsDevice() override;

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
        RhiTextureHandle CreateRhiTextureCubeFromFaces(uint32_t faceSize,
                                                       const void* const faces[6],
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
        struct RhiShaderModule
        {
            RhiShaderStageFlags stage = RhiShaderStageFlags::None;
            std::vector<uint8_t> bytecode;
        };

        struct RhiPipelineStateObjects
        {
            Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
            Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader;
            Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader;
            Microsoft::WRL::ComPtr<ID3D11GeometryShader> geometryShader;
            Microsoft::WRL::ComPtr<ID3D11HullShader> hullShader;
            Microsoft::WRL::ComPtr<ID3D11DomainShader> domainShader;
            Microsoft::WRL::ComPtr<ID3D11ComputeShader> computeShader;
            Microsoft::WRL::ComPtr<ID3D11BlendState> blendState;
            Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizerState;
            Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState;
            D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        };

        struct RhiPipelineLayoutState
        {
            struct StaticSampler
            {
                Microsoft::WRL::ComPtr<ID3D11SamplerState> state;
                RhiShaderStageFlags visibility = RhiShaderStageFlags::None;
                uint32_t shaderRegister = 0;
            };
            uint32_t bindingCount = 0;
            std::vector<StaticSampler> staticSamplers;
        };

        void Cleanup();
        bool CreateBackBufferView();
        bool CaptureBackBufferToPng(const wchar_t* path);
        bool EnsureRayMarchResources();
        bool RenderRayMarchFrame(const RhiBackendRayMarchFrameDesc& desc);
        bool EnsureMeshFrameResources(UINT width, UINT height);
        // Renders every static and skinned caster into m_shadowMap once per frame, before the
        // scene passes bind the back buffer. Returns false when there is no usable shadow map,
        // which the mesh passes turn into shadowMapResolution = 0 (unshadowed lighting).
        bool RenderNativeShadowMap(const RhiBackendMeshFrameDesc& desc);
        // Resolves every opaque static and skinned draw in `desc` into `out`, replacing whatever
        // was there. Shared by the shadow pass and the ambient-occlusion prepass, which want the
        // same geometry for the same reason: both are depth-only passes with no alpha handling,
        // so a cut-out or blended surface would contribute a solid silhouette it should not.
        void CollectOpaqueDraws(const RhiBackendMeshFrameDesc& desc, std::vector<Dx11ShadowCaster>& out);
        // Renders the depth+normal prepass and the SSAO chain, producing the runtimeAo term the
        // native mesh shader samples. Returns false (and leaves no AO texture) when SSAO is off,
        // the resources are unavailable, or the frame has no opaque geometry to occlude with.
        bool RenderNativeAmbientOcclusion(const RhiBackendMeshFrameDesc& desc);
        bool RenderMeshFrame(const RhiBackendMeshFrameDesc& desc);
        bool EnsureSkinnedMeshFrameResources(UINT width, UINT height);
        bool RenderSkinnedMeshFrame(const RhiBackendMeshFrameDesc& desc);

        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
        Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_backBufferRtv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_backBufferDepth;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_backBufferDsv;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_rayMarchVertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_rayMarchPixelShader;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_rayMarchConstantBuffer;
        // Analytic sky drawn behind the native mesh path; without it the background is the flat
        // clear colour. Fullscreen triangle, so no input layout and no vertex buffer.
        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_skyVertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_skyPixelShader;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_skyConstantBuffer;
        // Sampler for the HDR environment cube the sky samples when one is loaded. Clamped and
        // linear; the sky needs its own because the sky pass binds no other textures and must not
        // depend on the mesh pass having run first to populate a shared sampler slot.
        Microsoft::WRL::ComPtr<ID3D11SamplerState> m_skyEnvSampler;
        // Set once a compile has failed so the attempt is not repeated every frame -- a failed
        // shader compile is not free, and it filled the log with one identical error per frame.
        bool m_skyResourcesFailed = false;
        bool EnsureSkyResources();
        void RenderNativeSky(const RhiBackendMeshFrameDesc& desc);

        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_meshVertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_meshPixelShader;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> m_meshInputLayout;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_meshConstantBuffer;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> m_meshSamplerState;
        // 1x1 white SRV bound to t2 whenever RhiBackendMeshDrawDesc::occlusionSlotHasAo says the
        // slot's R channel is not ambient occlusion (the raster material has a single texture slot,
        // so it may hold metallic-roughness instead), and for draws with no occlusion texture at
        // all. Sampling its .r always returns 1.0, so the shader's materialAo term becomes a no-op
        // rather than reading an unrelated channel or an unbound resource. Owned here rather than
        // at file scope so it is released with the device instead of at static-destruction time,
        // matching the sibling mesh resources above.
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_meshOcclusionDummySrv;
        // 1x1x6 white cube SRV bound to t3 whenever the scene has no Skybox-built ambient cubemap
        // yet (it arrives asynchronously once the HDR source loads). Same static-destruction-order
        // rationale as m_meshOcclusionDummySrv above: owned as a member, not a file-scope static.
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_meshAmbientCubeDummySrv;
        // Single-cascade directional shadow map for the native mesh path. Owns its own depth
        // target, states and depth-only shaders; see Dx11ShadowMap.h.
        Dx11ShadowMap m_shadowMap;
        // Depth and world normals for the ambient-occlusion chain. The native path is forward-
        // shaded, so the two inputs the SSAO shaders need have to be rendered on purpose; see
        // Dx11DepthNormalPrepass.h.
        Dx11DepthNormalPrepass m_aoPrepass;
        Dx11SsaoGenerator m_ssaoGenerator;
        // Whether this frame produced a usable runtime-AO texture. Same role as
        // m_shadowMapReadyThisFrame: the mesh passes clear the shader's SSAO feature flag when it
        // is false, so a failed AO pass degrades to materialAo-only instead of sampling nothing.
        bool m_ssaoReadyThisFrame = false;
        // Last reported value of the above, -1 before the first report. A silently absent AO term
        // is indistinguishable from a weak one in a screenshot, so the state is announced when it
        // changes -- once, not once per frame.
        int m_lastReportedSsaoState = -1;
        // Scratch list of resolved opaque draws, cleared (not freed) once per pass like
        // m_meshTransparentDrawOrder. Shared by the shadow pass and the AO prepass, which run one
        // after the other and want the identical geometry.
        std::vector<Dx11ShadowCaster> m_resolvedOpaqueDraws;
        // Whether this frame's shadow map actually holds casters. The mesh passes zero out
        // shadowParams.w when it does not, so the pixel shader samples nothing stale.
        bool m_shadowMapReadyThisFrame = false;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_meshRasterizerState;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_meshDepthStencilState;
        Microsoft::WRL::ComPtr<ID3D11BlendState> m_meshBlendState;
        // Opaque draws must not blend (the source coverage was never meant to composite against
        // whatever was drawn before it) and must write depth so later opaque/transparent draws can
        // test against them; transparent draws blend but leave depth untouched so a transparent
        // fragment never occludes another transparent fragment behind it.
        Microsoft::WRL::ComPtr<ID3D11BlendState> m_meshOpaqueBlendState;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_meshTransparentDepthState;
        // Reused across frames to avoid a per-frame heap allocation for the opaque/transparent
        // draw-order sort; cleared (not freed) at the start of each RenderMeshFrame call.
        std::vector<uint32_t> m_meshOpaqueDrawOrder;
        std::vector<uint32_t> m_meshTransparentDrawOrder;

        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_skinnedMeshVertexShader;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> m_skinnedMeshInputLayout;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_skinnedMeshConstantBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_skinnedMeshBoneConstantBuffer;
        UINT m_backBufferDepthWidth = 0;
        UINT m_backBufferDepthHeight = 0;
        // Last Present return code, so a persistent non-S_OK status is reported once instead of
        // once per frame.
        HRESULT m_lastPresentStatus = S_OK;
        // Same idea for the mesh draw tally: report when the number of issued draws changes,
        // not every frame.
        uint32_t m_lastReportedIssuedMeshDraws = 0xFFFFFFFFu;
        // The per-draw transparent dump is a one-shot: it identifies the materials, it is not a
        // per-frame statistic.
        bool m_reportedTransparentDraws = false;
        // One-shot size comparison between the render target and the depth buffer.
        bool m_reportedDepthTargetSizes = false;
        RhiBackendCapabilities m_capabilities{};
        CommandQueue m_emptyGraphicsQueue;
        CommandQueue m_emptyComputeQueue;
        SwapChain m_emptySwapChain;
        uint64_t m_nextRhiResourceHandle = 1;
        uint64_t m_nextRhiDescriptorHandle = 1;
        uint64_t m_nextRhiShaderHandle = 1;
        uint64_t m_nextRhiPipelineLayoutHandle = 1;
        uint64_t m_nextRhiPipelineHandle = 1;
        std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11Resource>> m_rhiResources;
        std::unordered_map<uint64_t, RhiShaderModule> m_rhiShaders;
        std::unordered_map<uint64_t, RhiPipelineLayoutState> m_rhiPipelineLayouts;
        std::unordered_map<uint64_t, RhiPipelineStateObjects> m_rhiPipelines;
        std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_rhiSrvs;
        std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> m_rhiUavs;
        std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11RenderTargetView>> m_rhiRtvs;
        std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11DepthStencilView>> m_rhiDsvs;
        std::unordered_map<uint64_t, uint64_t> m_rhiViewResources;

        friend class Dx11RhiCommandEncoder;
    };
#endif
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
