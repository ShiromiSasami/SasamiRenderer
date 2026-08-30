// IblSystem.cpp
// Image-Based Lighting resource management: generation, upload, and SRV publication.
#include "Renderer/Scene/IblSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Utilities/RendererMathUtility.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "d3dx12.h"

namespace SasamiRenderer
{
    bool IblSystem::Initialize(IRHIDevice& device, const AllocateSrvRangeCallback& allocateSrvRange)
    {
        m_device = &device;

        CpuDescriptorHandle iblCpu{};
        GpuDescriptorHandle iblGpu{};
        if (!allocateSrvRange || !allocateSrvRange(3, iblCpu, iblGpu)) {
            return false;
        }
        m_iblSrvCpu = iblCpu;
        m_iblSrv    = iblGpu;

        const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_iblSpecularSrv = { iblGpu.ptr + static_cast<UINT64>(inc) };
        CpuDescriptorHandle preCpu  = { iblCpu.ptr + static_cast<SIZE_T>(inc) };
        CpuDescriptorHandle brdfCpu = { iblCpu.ptr + static_cast<SIZE_T>(inc) * 2 };
        Resource nullResource;

        D3D12_SHADER_RESOURCE_VIEW_DESC cubeSrvDesc = {};
        cubeSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        cubeSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        cubeSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        cubeSrvDesc.TextureCube.MipLevels = 1;
        cubeSrvDesc.TextureCube.MostDetailedMip = 0;
        cubeSrvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
        m_device->CreateShaderResourceView(nullResource, &cubeSrvDesc, iblCpu);
        m_device->CreateShaderResourceView(nullResource, &cubeSrvDesc, preCpu);

        D3D12_SHADER_RESOURCE_VIEW_DESC lutSrvDesc = {};
        lutSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        lutSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        lutSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        lutSrvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(nullResource, &lutSrvDesc, brdfCpu);

        return true;
    }

    void IblSystem::Shutdown()
    {
        Reset();
        m_device = nullptr;
    }

    void IblSystem::Reset()
    {
        if (m_device && m_iblIrradianceHandle.IsValid()) {
            m_device->DestroyRhiResource(m_iblIrradianceHandle);
        }
        if (m_device && m_iblPrefilterHandle.IsValid()) {
            m_device->DestroyRhiResource(m_iblPrefilterHandle);
        }
        if (m_device && m_iblBrdfLutHandle.IsValid()) {
            m_device->DestroyRhiResource(m_iblBrdfLutHandle);
        }
        m_iblIrradianceHandle = {};
        m_iblIrradianceCompat = nullptr;
        m_iblPrefilterHandle  = {};
        m_iblPrefilterCompat  = nullptr;
        m_iblBrdfLutHandle    = {};
        m_iblBrdfLutCompat    = nullptr;

        m_iblIrradianceTexture.Reset();
        m_iblIrradianceUpload.Reset();
        m_iblPrefilterTexture.Reset();
        m_iblPrefilterUpload.Reset();
        m_iblBrdfLutTexture.Reset();
        m_iblBrdfLutUpload.Reset();

        m_iblUploaded        = false;
        m_iblUploadAttempted = false;
        m_iblEnabled         = false;
        m_iblPrefilterMaxMip = 0.0f;
        m_diffuseShValid     = false;
        std::memset(m_diffuseSh, 0, sizeof(m_diffuseSh));
        m_cpuPrefilterSubresources.clear();
        m_cpuBrdfLutPixels.clear();
        m_cpuPrefilterBaseSize  = 0;
        m_cpuPrefilterMipLevels = 0;
        m_cpuBrdfLutWidth       = 0;
        m_cpuBrdfLutHeight      = 0;
        m_pregenerated.reset();
    }

    void IblSystem::AdoptPregeneratedIblData(GeneratedIblData&& data)
    {
        m_pregenerated = std::make_unique<GeneratedIblData>(std::move(data));
        m_iblUploaded        = false;
        m_iblUploadAttempted = false;
    }

    void IblSystem::EnsureTexturesUploaded(CommandList* cmdList,
                                           bool equirectLoaded,
                                           const std::vector<float>& equirectPixels,
                                           UINT equirectWidth,
                                           UINT equirectHeight)
    {
        if (!m_device || m_iblUploaded || m_iblUploadAttempted) {
            return;
        }
        m_iblUploadAttempted = true;

        if (m_pregenerated) {
            std::unique_ptr<GeneratedIblData> pregenerated = std::move(m_pregenerated);
            m_pregenerated.reset();

            if (UploadGeneratedIblTextures(cmdList, *pregenerated)) {
                const float maxMip = (pregenerated->prefilterMipLevels > 0u)
                    ? static_cast<float>(pregenerated->prefilterMipLevels - 1u)
                    : 0.0f;
                ApplyIblMetadata(true, maxMip, true, pregenerated->diffuseShCoefficients);
                m_iblUploaded = true;
                return;
            }

            Reset();
            m_iblUploadAttempted = true;
            DebugLog("Pregenerated IBL upload failed. Fallback IBL will be used.\n");

            if (!UploadFallbackIblTextures(cmdList)) {
                return;
            }
            ApplyIblMetadata(false, 0.0f, false, nullptr);
            m_iblUploaded = true;
            return;
        }

        if (equirectLoaded) {
            GeneratedIblData generated{};
            if (GenerateHdrIblData(equirectPixels, equirectWidth, equirectHeight, generated) &&
                UploadGeneratedIblTextures(cmdList, generated)) {
                const float maxMip = (generated.prefilterMipLevels > 0u)
                    ? static_cast<float>(generated.prefilterMipLevels - 1u)
                    : 0.0f;
                ApplyIblMetadata(true, maxMip, true, generated.diffuseShCoefficients);
                m_iblUploaded = true;
                return;
            }

            Reset();
            m_iblUploadAttempted = true;
            DebugLog("Runtime equirect->IBL generation failed. Fallback IBL will be used.\n");
        }

        if (!UploadFallbackIblTextures(cmdList)) {
            return;
        }
        ApplyIblMetadata(false, 0.0f, false, nullptr);
        m_iblUploaded = true;
    }

    bool IblSystem::GenerateHdrIblData(const std::vector<float>& equirectPixels,
                                        UINT equirectWidth,
                                        UINT equirectHeight,
                                        GeneratedIblData& outData)
    {
        if (equirectPixels.empty() || equirectWidth == 0 || equirectHeight == 0) {
            return false;
        }

        const UINT irradianceSize    = 16;
        const UINT prefilterSize     = 64;
        const UINT prefilterMipLevels = 4;
        const UINT brdfLutSize       = 64;

        outData = {};
        outData.prefilterMipLevels = prefilterMipLevels;

        RendererMathUtility::GenerateIrradianceCubemapFromEquirect(equirectPixels,
                                                                   equirectWidth,
                                                                   equirectHeight,
                                                                   irradianceSize,
                                                                   outData.irradianceFaces);
        RendererMathUtility::GeneratePrefilterCubemapFromEquirect(equirectPixels,
                                                                  equirectWidth,
                                                                  equirectHeight,
                                                                  prefilterSize,
                                                                  prefilterMipLevels,
                                                                  outData.prefilterSubresources);
        RendererMathUtility::GenerateBrdfLut(brdfLutSize, brdfLutSize, outData.brdfLutPixels);
        RendererMathUtility::GenerateDiffuseShCoefficientsFromEquirect(equirectPixels,
                                                                        equirectWidth,
                                                                        equirectHeight,
                                                                        outData.diffuseShCoefficients);

        return outData.irradianceFaces.size() == 6u &&
               outData.prefilterSubresources.size() == static_cast<size_t>(prefilterMipLevels) * 6u &&
               !outData.brdfLutPixels.empty();
    }

    bool IblSystem::UploadGeneratedIblTextures(CommandList* cmdList, const GeneratedIblData& data)
    {
        if (!m_device || !cmdList) {
            return false;
        }

        const UINT irradianceSize = 16;
        const UINT prefilterSize  = 64;
        const UINT brdfLutSize    = 64;

        const bool supportsRhi = m_device->GetCapabilities().supportsRhiResourceCreation;

        // Irradiance cubemap (R16G16B16A16_FLOAT, 16×16, 1 mip, 6 faces)
        Resource* irrPreCreated = nullptr;
        if (supportsRhi) {
            RhiTextureDesc rhiDesc{};
            rhiDesc.extent       = { irradianceSize, irradianceSize, 1 };
            rhiDesc.arrayLayers  = 6;
            rhiDesc.mipLevels    = 1;
            rhiDesc.format       = RhiFormat::R16G16B16A16Float;
            rhiDesc.usage        = RhiTextureUsageFlags::ShaderResource;
            rhiDesc.memoryUsage  = RhiMemoryUsage::GpuOnly;
            rhiDesc.initialState = RhiResourceState::CopyDest;
            m_iblIrradianceHandle = m_device->CreateRhiTexture(rhiDesc);
            m_iblIrradianceCompat = m_device->GetD3D12CompatibilityResource(m_iblIrradianceHandle);
            if (!m_iblIrradianceCompat && m_iblIrradianceHandle.IsValid()) {
                m_device->DestroyRhiResource(m_iblIrradianceHandle);
                m_iblIrradianceHandle = {};
            }
            irrPreCreated = m_iblIrradianceCompat;
        }
        const bool irrOk = ResourceUploadUtility::CreateTextureCubeFromFloatFacesWithMips(*m_device,
                                                                    cmdList,
                                                                    data.irradianceFaces,
                                                                    irradianceSize,
                                                                    1,
                                                                    m_iblIrradianceTexture,
                                                                    m_iblIrradianceUpload,
                                                                    irrPreCreated);

        // Prefilter cubemap (R16G16B16A16_FLOAT, 64×64, N mips, 6 faces)
        Resource* prePreCreated = nullptr;
        if (supportsRhi) {
            RhiTextureDesc rhiDesc{};
            rhiDesc.extent       = { prefilterSize, prefilterSize, 1 };
            rhiDesc.arrayLayers  = 6;
            rhiDesc.mipLevels    = data.prefilterMipLevels;
            rhiDesc.format       = RhiFormat::R16G16B16A16Float;
            rhiDesc.usage        = RhiTextureUsageFlags::ShaderResource;
            rhiDesc.memoryUsage  = RhiMemoryUsage::GpuOnly;
            rhiDesc.initialState = RhiResourceState::CopyDest;
            m_iblPrefilterHandle = m_device->CreateRhiTexture(rhiDesc);
            m_iblPrefilterCompat = m_device->GetD3D12CompatibilityResource(m_iblPrefilterHandle);
            if (!m_iblPrefilterCompat && m_iblPrefilterHandle.IsValid()) {
                m_device->DestroyRhiResource(m_iblPrefilterHandle);
                m_iblPrefilterHandle = {};
            }
            prePreCreated = m_iblPrefilterCompat;
        }
        const bool preOk = ResourceUploadUtility::CreateTextureCubeFromFloatFacesWithMips(*m_device,
                                                                    cmdList,
                                                                    data.prefilterSubresources,
                                                                    prefilterSize,
                                                                    data.prefilterMipLevels,
                                                                    m_iblPrefilterTexture,
                                                                    m_iblPrefilterUpload,
                                                                    prePreCreated);

        // BRDF LUT (R16G16B16A16_FLOAT, 64×64, 2D)
        Resource* brdfPreCreated = nullptr;
        if (supportsRhi) {
            RhiTextureDesc rhiDesc{};
            rhiDesc.extent       = { brdfLutSize, brdfLutSize, 1 };
            rhiDesc.arrayLayers  = 1;
            rhiDesc.mipLevels    = 1;
            rhiDesc.format       = RhiFormat::R16G16B16A16Float;
            rhiDesc.usage        = RhiTextureUsageFlags::ShaderResource;
            rhiDesc.memoryUsage  = RhiMemoryUsage::GpuOnly;
            rhiDesc.initialState = RhiResourceState::CopyDest;
            m_iblBrdfLutHandle = m_device->CreateRhiTexture(rhiDesc);
            m_iblBrdfLutCompat = m_device->GetD3D12CompatibilityResource(m_iblBrdfLutHandle);
            if (!m_iblBrdfLutCompat && m_iblBrdfLutHandle.IsValid()) {
                m_device->DestroyRhiResource(m_iblBrdfLutHandle);
                m_iblBrdfLutHandle = {};
            }
            brdfPreCreated = m_iblBrdfLutCompat;
        }
        const bool brdfOk = ResourceUploadUtility::CreateTexture2DFromFloatRgba(*m_device,
                                                          cmdList,
                                                          data.brdfLutPixels,
                                                          brdfLutSize,
                                                          brdfLutSize,
                                                          m_iblBrdfLutTexture,
                                                          m_iblBrdfLutUpload,
                                                          brdfPreCreated);

        if (!irrOk || !preOk || !brdfOk) {
            return false;
        }

        PublishIblSrvs(DXGI_FORMAT_R16G16B16A16_FLOAT,
                       DXGI_FORMAT_R16G16B16A16_FLOAT,
                       data.prefilterMipLevels);
        m_cpuPrefilterSubresources = data.prefilterSubresources;
        m_cpuBrdfLutPixels         = data.brdfLutPixels;
        m_cpuPrefilterBaseSize     = prefilterSize;
        m_cpuPrefilterMipLevels    = data.prefilterMipLevels;
        m_cpuBrdfLutWidth          = brdfLutSize;
        m_cpuBrdfLutHeight         = brdfLutSize;
        return true;
    }

    bool IblSystem::UploadFallbackIblTextures(CommandList* cmdList)
    {
        if (!m_device || !cmdList) {
            return false;
        }

        static const uint8_t blackFace[4] = { 0, 0, 0, 255 };
        static const uint8_t midFace[4]   = { 128, 128, 128, 255 };

        std::vector<std::vector<uint8_t>> irradianceFaces;
        std::vector<std::vector<uint8_t>> prefilterFaces;
        std::vector<uint8_t> brdfPixels;
        irradianceFaces.assign(6, std::vector<uint8_t>(blackFace, blackFace + 4));
        prefilterFaces.assign(6, std::vector<uint8_t>(blackFace, blackFace + 4));
        brdfPixels.assign(midFace, midFace + 4);

        if (!ResourceUploadUtility::CreateTextureCubeFromRgba8Faces(*m_device, cmdList,
                                             irradianceFaces, 1, 1,
                                             m_iblIrradianceTexture, m_iblIrradianceUpload)) {
            return false;
        }
        if (!ResourceUploadUtility::CreateTextureCubeFromRgba8Faces(*m_device, cmdList,
                                             prefilterFaces, 1, 1,
                                             m_iblPrefilterTexture, m_iblPrefilterUpload)) {
            return false;
        }
        if (!ResourceUploadUtility::CreateTexture2DFromRgba8(*m_device, cmdList,
                                                              brdfPixels.data(), 1, 1,
                                                              m_iblBrdfLutTexture,
                                                              m_iblBrdfLutUpload)) {
            return false;
        }

        PublishIblSrvs(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, 1);
        return true;
    }

    void IblSystem::PublishIblSrvs(DXGI_FORMAT cubeFormat, DXGI_FORMAT brdfFormat, UINT prefilterMipLevels)
    {
        if (!m_device) {
            return;
        }

        const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_iblSpecularSrv = { m_iblSrv.ptr + static_cast<UINT64>(inc) };
        CpuDescriptorHandle irrCpu  = m_iblSrvCpu;
        CpuDescriptorHandle preCpu  = { m_iblSrvCpu.ptr + static_cast<SIZE_T>(inc) };
        CpuDescriptorHandle brdfCpu = { m_iblSrvCpu.ptr + static_cast<SIZE_T>(inc) * 2 };

        Resource* irrResource  = m_iblIrradianceCompat ? m_iblIrradianceCompat : &m_iblIrradianceTexture;
        Resource* preResource  = m_iblPrefilterCompat  ? m_iblPrefilterCompat  : &m_iblPrefilterTexture;
        Resource* brdfResource = m_iblBrdfLutCompat    ? m_iblBrdfLutCompat    : &m_iblBrdfLutTexture;

        D3D12_SHADER_RESOURCE_VIEW_DESC cubeSrvDesc = {};
        cubeSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        cubeSrvDesc.Format = cubeFormat;
        cubeSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        cubeSrvDesc.TextureCube.MipLevels = 1;
        cubeSrvDesc.TextureCube.MostDetailedMip = 0;
        cubeSrvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
        m_device->CreateShaderResourceView(*irrResource, &cubeSrvDesc, irrCpu);

        D3D12_SHADER_RESOURCE_VIEW_DESC preCubeSrvDesc = cubeSrvDesc;
        preCubeSrvDesc.TextureCube.MipLevels = prefilterMipLevels;
        m_device->CreateShaderResourceView(*preResource, &preCubeSrvDesc, preCpu);

        D3D12_SHADER_RESOURCE_VIEW_DESC lutSrvDesc = {};
        lutSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        lutSrvDesc.Format = brdfFormat;
        lutSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        lutSrvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(*brdfResource, &lutSrvDesc, brdfCpu);
    }

    void IblSystem::ApplyIblMetadata(bool enabled, float prefilterMaxMip,
                                      bool shValid, const float (*diffuseSh)[3])
    {
        m_iblEnabled         = enabled;
        m_iblPrefilterMaxMip = prefilterMaxMip;
        m_diffuseShValid     = shValid;
        if (shValid && diffuseSh != nullptr) {
            std::memcpy(m_diffuseSh, diffuseSh, sizeof(m_diffuseSh));
        } else {
            std::memset(m_diffuseSh, 0, sizeof(m_diffuseSh));
        }
    }

} // namespace SasamiRenderer
