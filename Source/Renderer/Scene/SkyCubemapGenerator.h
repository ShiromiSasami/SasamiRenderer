#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/ShaderBlobCache.h"

#include <functional>
#include <string>
#include <vector>

namespace SasamiRenderer
{
    // GPU port of RendererMathUtility::GenerateSkyCubemapFromEquirect and its mip
    // chain builder (see Skybox::UploadHdrSkyboxTexture). Runs two compute shaders --
    // equirect->cube mip0, then a per-mip box-filter downsample -- instead of the
    // ~9.93s CPU path measured for a 2048^2 x 6 face cube.
    class SkyCubemapGenerator
    {
    public:
        using SrvAllocFn = std::function<bool(UINT count, CpuDescriptorHandle&, GpuDescriptorHandle&)>;

        // srvHeap should be the descriptor heap backing srvAlloc so Generate() can bind
        // it on the command list before dispatching. If omitted, the caller must ensure
        // that heap is already bound on cmdList before calling Generate() (D3D12's
        // SetDescriptorHeaps state persists on the command list until changed).
        bool Initialize(IRHIDevice& device, const std::string& computeProfile, SrvAllocFn srvAlloc, DescriptorHeap* srvHeap = nullptr);
        bool IsReady() const { return m_ready; }

        // Records the whole generation into cmdList. Creates outCube itself.
        // On success outCube has: 6 array slices, `faceSize` base, mip chain down to 1x1,
        // DXGI_FORMAT_R16G16B16A16_FLOAT, ALLOW_UNORDERED_ACCESS, and is left in
        // D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE.
        // equirectRgb is tightly packed 3 floats per pixel (RGB).
        bool Generate(CommandList* cmdList,
                      const std::vector<float>& equirectRgb,
                      UINT srcWidth,
                      UINT srcHeight,
                      UINT faceSize,
                      Resource& outCube);

    private:
        bool CreateFromEquirectPipeline(IRHIDevice& device, const std::string& computeProfile);
        bool CreateDownsamplePipeline(IRHIDevice& device, const std::string& computeProfile);
        bool UploadEquirectTexture(CommandList* cmdList, const std::vector<float>& equirectRgb, UINT srcWidth, UINT srcHeight);
        bool EnsureDescriptors(UINT faceSize, UINT mipLevels);

        IRHIDevice* m_device = nullptr;
        SrvAllocFn m_srvAlloc;
        DescriptorHeap* m_srvHeap = nullptr;
        bool m_ready = false;

        RootSignature m_fromEquirectRootSignature;
        PipelineState m_fromEquirectPipelineState;
        RootSignature m_downsampleRootSignature;
        PipelineState m_downsamplePipelineState;

        // Memoizes shader blobs across both pipeline-init functions above.
        ShaderBlobCache m_shaderBlobCache;

        // Source equirect texture + its upload heap. Both must be members so they
        // outlive the GPU-side UpdateSubresources copy recorded into cmdList (same
        // reason Skybox keeps m_skyboxTextureUpload alive).
        Resource m_equirectTexture;
        Resource m_equirectUpload;

        CpuDescriptorHandle m_equirectSrvCpu{};
        GpuDescriptorHandle m_equirectSrvGpu{};

        // Descriptor slots for the cube mip chain, indexed by mip level. Allocated
        // once per distinct faceSize and reused across Generate() calls; the views
        // bound to these slots are re-pointed at the current call's outCube every
        // time since a new cube resource is created each Generate() call.
        UINT m_cachedFaceSize = 0;
        UINT m_cachedMipLevels = 0;
        std::vector<CpuDescriptorHandle> m_mipUavCpu;
        std::vector<GpuDescriptorHandle> m_mipUavGpu;
        std::vector<CpuDescriptorHandle> m_mipSrvCpu;
        std::vector<GpuDescriptorHandle> m_mipSrvGpu;
    };
}
