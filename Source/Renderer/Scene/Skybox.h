#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Structures/RendererEnums.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace SasamiRenderer
{
    class Skybox
    {
    public:
        using SkyboxLoadFormat = RendererEnums::SkyboxLoadFormat;

        using AllocateSrvRangeCallback = std::function<bool(UINT count,
                                                            CpuDescriptorHandle& outCpu,
                                                            GpuDescriptorHandle& outGpu)>;
        using PushCameraCbCallback = std::function<D3D12_GPU_VIRTUAL_ADDRESS(const float mvp[16],
                                                                              const float world[16])>;

        bool Initialize(IRHIDevice& device, const AllocateSrvRangeCallback& allocateSrvRange);
        void Shutdown();

        void SetHdrEquirectData(std::vector<float> pixels, UINT width, UINT height);
        void SetLdrEquirectData(std::vector<uint8_t> pixels, UINT width, UINT height);
        void SetLoadFormat(SkyboxLoadFormat format) { m_skyboxLoadFormat = format; }
        SkyboxLoadFormat GetLoadFormat() const { return m_skyboxLoadFormat; }

        void RefreshEnvironmentAssets();
        void EnsureSkyboxTextureUploaded(CommandList* cmdList);
        void EnsureIblTexturesUploaded(CommandList* cmdList);

        void Render(CommandList* cmdList,
                    RenderPipelineStateCache& pipelineStateCache,
                    DescriptorHeap& srvHeap,
                    const Viewport& viewport,
                    const Rect& scissorRect,
                    const float cameraPV[16],
                    const float cameraPos[3],
                    const PushCameraCbCallback& pushCameraCb) const;

        bool IsSkyboxTextureUploaded() const { return m_skyboxTextureUploaded; }
        bool HasSkyboxUploadAttempted() const { return m_skyboxUploadAttempted; }
        bool IsIblUploaded() const { return m_iblUploaded; }
        GpuDescriptorHandle GetSkyboxSrvTable() const { return m_skyboxSrv; }
        bool HasIblUploadAttempted() const { return m_iblUploadAttempted; }

        bool IsIblEnabled() const { return m_iblEnabled; }
        float GetIblPrefilterMaxMip() const { return m_iblPrefilterMaxMip; }
        GpuDescriptorHandle GetIblSrvTable() const { return m_iblSrv; }
        bool HasDiffuseShCoefficients() const { return m_diffuseShValid; }
        const float (*GetDiffuseShCoefficients() const)[3] { return m_diffuseSh; }

    private:
        struct GeneratedIblData
        {
            std::vector<std::vector<float>> irradianceFaces;
            std::vector<std::vector<float>> prefilterSubresources;
            std::vector<float> brdfLutPixels;
            float diffuseShCoefficients[9][3] = {};
            UINT prefilterMipLevels = 0;
        };

        enum class SourceType
        {
            None = 0,
            HdrRgbFloat = 1,
            LdrRgba8 = 2,
        };

        bool InitializeGeometry();
        bool EnsureHdrEnvironmentLoaded();
        void ResetSkyboxResources();
        void ResetIblResources();
        bool UploadHdrSkyboxTexture(CommandList* cmdList);
        bool UploadFallbackSkyboxTexture(CommandList* cmdList);
        void PublishSkyboxSrv(DXGI_FORMAT format);
        bool GenerateHdrIblData(GeneratedIblData& outData) const;
        bool UploadGeneratedIblTextures(CommandList* cmdList, const GeneratedIblData& data);
        bool UploadFallbackIblTextures(CommandList* cmdList);
        void PublishIblSrvs(DXGI_FORMAT cubeFormat, DXGI_FORMAT brdfFormat, UINT prefilterMipLevels);
        void ApplyIblMetadata(bool enabled, float prefilterMaxMip, bool shValid, const float (*diffuseSh)[3]);

        IRHIDevice* m_device = nullptr;

        CpuDescriptorHandle m_skyboxSrvCpu{};
        GpuDescriptorHandle m_skyboxSrv{};
        CpuDescriptorHandle m_iblSrvCpu{};
        GpuDescriptorHandle m_iblSrv{};

        SourceType m_sourceType = SourceType::None;
        UINT m_sourceWidth = 0;
        UINT m_sourceHeight = 0;
        std::vector<float> m_sourceHdrRgb;
        std::vector<uint8_t> m_sourceLdrRgba8;

        Resource m_skyboxTexture;
        Resource m_skyboxTextureUpload;
        bool m_skyboxTextureUploaded = false;
        bool m_skyboxUploadAttempted = false;
        bool m_skyboxTextureIsHdr = false;
        UINT m_skyboxMipLevels = 1;

        Resource m_iblIrradianceTexture;
        Resource m_iblIrradianceUpload;
        Resource m_iblPrefilterTexture;
        Resource m_iblPrefilterUpload;
        Resource m_iblBrdfLutTexture;
        Resource m_iblBrdfLutUpload;
        bool m_iblUploaded = false;
        bool m_iblUploadAttempted = false;
        bool m_iblEnabled = false;
        float m_iblPrefilterMaxMip = 0.0f;
        bool m_diffuseShValid = false;
        float m_diffuseSh[9][3] = {};

        bool m_hdrEquirectLoaded = false;
        bool m_hdrEquirectTried = false;
        UINT m_hdrEquirectWidth = 0;
        UINT m_hdrEquirectHeight = 0;
        std::vector<float> m_hdrEquirectPixels;

        SkyboxLoadFormat m_skyboxLoadFormat = SkyboxLoadFormat::Auto;

        Resource m_skyboxVB;
        VertexBufferView m_skyboxVBV{};
    };
}
