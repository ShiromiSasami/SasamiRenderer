#pragma once

#include "Renderer/Core/GraphicsDevice.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace SasamiRenderer
{
    //
    // IblSystem
    // Image-Based Lighting resource management: irradiance cubemap, prefilter cubemap,
    // BRDF LUT, and diffuse SH coefficients.
    //
    // Owned by Skybox, which feeds it equirect environment data.
    // Consumers (LightSystem, LightingRenderNode, SWRT) access IBL via Skybox getter shims.
    //
    class IblSystem
    {
    public:
        using AllocateSrvRangeCallback = std::function<bool(UINT count,
                                                             CpuDescriptorHandle& outCpu,
                                                             GpuDescriptorHandle& outGpu)>;

        bool Initialize(IRHIDevice& device, const AllocateSrvRangeCallback& allocateSrvRange);
        void Shutdown();
        void Reset();

        // Called by Skybox once EnsureHdrEnvironmentLoaded() succeeds.
        // Generates IBL textures and uploads them to the GPU on next EnsureTexturesUploaded().
        void EnsureTexturesUploaded(CommandList* cmdList,
                                    bool equirectLoaded,
                                    const std::vector<float>& equirectPixels,
                                    UINT equirectWidth,
                                    UINT equirectHeight);

        // State queries (forwarded through Skybox shims to callers)
        bool IsUploaded() const { return m_iblUploaded; }
        bool HasUploadAttempted() const { return m_iblUploadAttempted; }

        // Lighting accessors
        bool IsEnabled() const { return m_iblEnabled; }
        float GetPrefilterMaxMip() const { return m_iblPrefilterMaxMip; }
        GpuDescriptorHandle GetSrvTable() const { return m_iblSrv; }
        ID3D12Resource* GetPrefilterResource() const { return m_iblPrefilterTexture.Get(); }
        bool HasDiffuseShCoefficients() const { return m_diffuseShValid; }
        const float (*GetDiffuseShCoefficients() const)[3] { return m_diffuseSh; }

        // CPU-side caches for SWRT / DXR
        bool HasCpuPrefilterData() const { return !m_cpuPrefilterSubresources.empty(); }
        const std::vector<std::vector<float>>& GetCpuPrefilterSubresources() const { return m_cpuPrefilterSubresources; }
        UINT GetCpuPrefilterBaseSize() const { return m_cpuPrefilterBaseSize; }
        UINT GetCpuPrefilterMipLevels() const { return m_cpuPrefilterMipLevels; }
        const std::vector<float>& GetCpuBrdfLutPixels() const { return m_cpuBrdfLutPixels; }
        UINT GetCpuBrdfLutWidth() const { return m_cpuBrdfLutWidth; }
        UINT GetCpuBrdfLutHeight() const { return m_cpuBrdfLutHeight; }

    private:
        struct GeneratedIblData
        {
            std::vector<std::vector<float>> irradianceFaces;
            std::vector<std::vector<float>> prefilterSubresources;
            std::vector<float> brdfLutPixels;
            float diffuseShCoefficients[9][3] = {};
            UINT prefilterMipLevels = 0;
        };

        bool GenerateHdrIblData(const std::vector<float>& equirectPixels,
                                 UINT equirectWidth,
                                 UINT equirectHeight,
                                 GeneratedIblData& outData) const;
        bool UploadGeneratedIblTextures(CommandList* cmdList, const GeneratedIblData& data);
        bool UploadFallbackIblTextures(CommandList* cmdList);
        void PublishIblSrvs(DXGI_FORMAT cubeFormat, DXGI_FORMAT brdfFormat, UINT prefilterMipLevels);
        void ApplyIblMetadata(bool enabled, float prefilterMaxMip,
                               bool shValid, const float (*diffuseSh)[3]);

        IRHIDevice* m_device = nullptr;

        CpuDescriptorHandle m_iblSrvCpu{};
        GpuDescriptorHandle m_iblSrv{};

        Resource m_iblIrradianceTexture;
        Resource m_iblIrradianceUpload;
        Resource m_iblPrefilterTexture;
        Resource m_iblPrefilterUpload;
        Resource m_iblBrdfLutTexture;
        Resource m_iblBrdfLutUpload;

        bool  m_iblUploaded        = false;
        bool  m_iblUploadAttempted = false;
        bool  m_iblEnabled         = false;
        float m_iblPrefilterMaxMip = 0.0f;
        bool  m_diffuseShValid     = false;
        float m_diffuseSh[9][3]    = {};

        std::vector<std::vector<float>> m_cpuPrefilterSubresources;
        std::vector<float>              m_cpuBrdfLutPixels;
        UINT m_cpuPrefilterBaseSize  = 0;
        UINT m_cpuPrefilterMipLevels = 0;
        UINT m_cpuBrdfLutWidth       = 0;
        UINT m_cpuBrdfLutHeight      = 0;
    };
}
