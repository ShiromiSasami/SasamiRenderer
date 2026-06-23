#pragma once

#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Scene/IblSystem.h"
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

        void Render(IRhiCommandEncoder* enc,
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
        bool IsSkyboxVBValid() const { return m_skyboxVbBinding.buffer.IsValid() || m_skyboxVB.IsValid(); }
        const RhiVertexBufferBinding& GetSkyboxVBBinding() const { return m_skyboxVbBinding; }
        VertexBufferView GetSkyboxVBV() const { return m_skyboxVBV; }
        bool IsIblUploaded() const { return m_iblSystem.IsUploaded(); }
        bool HasIblUploadAttempted() const { return m_iblSystem.HasUploadAttempted(); }

        bool IsIblEnabled() const { return m_iblSystem.IsEnabled(); }
        float GetIblPrefilterMaxMip() const { return m_iblSystem.GetPrefilterMaxMip(); }
        GpuDescriptorHandle GetIblSrvTable() const { return m_iblSystem.GetSrvTable(); }
        ID3D12Resource* GetIblPrefilterResource() const { return m_iblSystem.GetPrefilterResource(); }
        bool HasDiffuseShCoefficients() const { return m_iblSystem.HasDiffuseShCoefficients(); }
        const float (*GetDiffuseShCoefficients() const)[3] { return m_iblSystem.GetDiffuseShCoefficients(); }
        bool HasCpuPrefilterData() const { return m_iblSystem.HasCpuPrefilterData(); }
        const std::vector<std::vector<float>>& GetCpuPrefilterSubresources() const { return m_iblSystem.GetCpuPrefilterSubresources(); }
        UINT GetCpuPrefilterBaseSize() const { return m_iblSystem.GetCpuPrefilterBaseSize(); }
        UINT GetCpuPrefilterMipLevels() const { return m_iblSystem.GetCpuPrefilterMipLevels(); }
        const std::vector<float>& GetCpuBrdfLutPixels() const { return m_iblSystem.GetCpuBrdfLutPixels(); }
        UINT GetCpuBrdfLutWidth() const { return m_iblSystem.GetCpuBrdfLutWidth(); }
        UINT GetCpuBrdfLutHeight() const { return m_iblSystem.GetCpuBrdfLutHeight(); }
        bool IsDirectionalLightMarkerEnabled() const { return m_directionalLightMarkerEnabled; }
        void SetDirectionalLightMarkerEnabled(bool enabled) { m_directionalLightMarkerEnabled = enabled; }
        float GetDirectionalLightMarkerAngularRadius() const { return m_directionalLightMarkerAngularRadius; }
        float GetDirectionalLightMarkerHaloAngularRadius() const { return m_directionalLightMarkerHaloAngularRadius; }
        float GetDirectionalLightMarkerBrightness() const { return m_directionalLightMarkerBrightness; }
        void SetDirectionalLightMarkerAngularRadius(float radians);

    private:
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

        IRHIDevice* m_device = nullptr;

        CpuDescriptorHandle m_skyboxSrvCpu{};
        GpuDescriptorHandle m_skyboxSrv{};
        IblSystem m_iblSystem;

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
        RhiBufferHandle m_skyboxRhiVB{};
        RhiVertexBufferBinding m_skyboxVbBinding{};
    };
}
