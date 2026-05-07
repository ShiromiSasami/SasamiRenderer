#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Scene/RenderLightProxy.h"
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
                                                                              const float world[16],
                                                                              const float extra0[4],
                                                                              const float extra1[4],
                                                                              const float extra2[4])>;

        bool Initialize(IRHIDevice& device, const AllocateSrvRangeCallback& allocateSrvRange);
        void Shutdown();

        void SetHdrEquirectData(std::vector<float> pixels, UINT width, UINT height);
        void SetLdrEquirectData(std::vector<uint8_t> pixels, UINT width, UINT height);
        void SetLdrCubemapFaceData(std::vector<std::vector<uint8_t>> facePixels, UINT width, UINT height);
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
                    const RenderDirectionalLight& directionalLight,
                    const PushCameraCbCallback& pushCameraCb) const;

        bool IsSkyboxTextureUploaded() const { return m_skyboxTextureUploaded; }
        bool HasSkyboxUploadAttempted() const { return m_skyboxUploadAttempted; }
        bool IsSkyboxVBValid() const { return m_skyboxVB.IsValid(); }
        VertexBufferView GetSkyboxVBV() const { return m_skyboxVBV; }
        bool IsIblUploaded() const { return m_iblUploaded; }
        bool HasIblUploadAttempted() const { return m_iblUploadAttempted; }

        bool IsIblEnabled() const { return m_iblEnabled; }
        float GetIblPrefilterMaxMip() const { return m_iblPrefilterMaxMip; }
        GpuDescriptorHandle GetIblSrvTable() const { return m_iblSrv; }
        ID3D12Resource* GetIblPrefilterResource() const { return m_iblPrefilterTexture.Get(); }
        bool HasDiffuseShCoefficients() const { return m_diffuseShValid; }
        const float (*GetDiffuseShCoefficients() const)[3] { return m_diffuseSh; }
        bool HasCpuPrefilterData() const { return !m_cpuPrefilterSubresources.empty(); }
        const std::vector<std::vector<float>>& GetCpuPrefilterSubresources() const { return m_cpuPrefilterSubresources; }
        UINT GetCpuPrefilterBaseSize() const { return m_cpuPrefilterBaseSize; }
        UINT GetCpuPrefilterMipLevels() const { return m_cpuPrefilterMipLevels; }
        const std::vector<float>& GetCpuBrdfLutPixels() const { return m_cpuBrdfLutPixels; }
        UINT GetCpuBrdfLutWidth() const { return m_cpuBrdfLutWidth; }
        UINT GetCpuBrdfLutHeight() const { return m_cpuBrdfLutHeight; }
        bool IsDirectionalLightMarkerEnabled() const { return m_directionalLightMarkerEnabled; }
        void SetDirectionalLightMarkerEnabled(bool enabled) { m_directionalLightMarkerEnabled = enabled; }
        float GetDirectionalLightMarkerAngularRadius() const { return m_directionalLightMarkerAngularRadius; }
        float GetDirectionalLightMarkerHaloAngularRadius() const { return m_directionalLightMarkerHaloAngularRadius; }
        float GetDirectionalLightMarkerBrightness() const { return m_directionalLightMarkerBrightness; }
        void SetDirectionalLightMarkerAngularRadius(float radians);

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
            LdrCubemapFaces = 3,
        };

        bool InitializeGeometry();
        bool EnsureHdrEnvironmentLoaded();
        void ResetSkyboxResources();
        void ResetIblResources();
        bool UploadHdrSkyboxTexture(CommandList* cmdList);
        bool UploadLdrCubemapTexture(CommandList* cmdList);
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
        std::vector<std::vector<uint8_t>> m_sourceCubemapFaceRgba8;

        Resource m_skyboxTexture;
        Resource m_skyboxTextureUpload;
        bool m_skyboxTextureUploaded = false;
        bool m_skyboxUploadAttempted = false;
        bool m_skyboxTextureIsHdr = false;

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
        std::vector<std::vector<float>> m_cpuPrefilterSubresources;
        std::vector<float> m_cpuBrdfLutPixels;
        UINT m_cpuPrefilterBaseSize = 0;
        UINT m_cpuPrefilterMipLevels = 0;
        UINT m_cpuBrdfLutWidth = 0;
        UINT m_cpuBrdfLutHeight = 0;

        bool m_hdrEquirectLoaded = false;
        bool m_hdrEquirectTried = false;
        UINT m_hdrEquirectWidth = 0;
        UINT m_hdrEquirectHeight = 0;
        std::vector<float> m_hdrEquirectPixels;

        SkyboxLoadFormat m_skyboxLoadFormat = SkyboxLoadFormat::Auto;
        bool m_directionalLightMarkerEnabled = true;
        float m_directionalLightMarkerAngularRadius = 0.02f;
        float m_directionalLightMarkerHaloAngularRadius = 0.08f;
        float m_directionalLightMarkerBrightness = 1.0f;

        Resource m_skyboxVB;
        VertexBufferView m_skyboxVBV{};
    };
}
