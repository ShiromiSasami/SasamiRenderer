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

namespace
{
    // Upload a float-face cubemap (R16G16B16A16_FLOAT) to an existing or newly created GPU texture.
    // If preCreated is non-null the caller already allocated the GPU texture (via RHI); creation is skipped.
    static bool CreateTextureCubeFromFloatFacesWithMips(SasamiRenderer::IRHIDevice& device,
                                                        SasamiRenderer::CommandList* cmdList,
                                                        const std::vector<std::vector<float>>& subresourcesRgba,
                                                        UINT baseFaceSize,
                                                        UINT mipLevels,
                                                        SasamiRenderer::Resource& outTexture,
                                                        SasamiRenderer::Resource& outUpload,
                                                        SasamiRenderer::Resource* preCreated = nullptr)
    {
        if (!cmdList || baseFaceSize == 0 || mipLevels == 0 ||
            subresourcesRgba.size() != static_cast<size_t>(mipLevels) * 6u) {
            return false;
        }

        SasamiRenderer::Resource* gpuTexture = preCreated;
        if (!gpuTexture) {
            D3D12_RESOURCE_DESC texDesc = {};
            texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texDesc.Width = baseFaceSize;
            texDesc.Height = baseFaceSize;
            texDesc.DepthOrArraySize = 6;
            texDesc.MipLevels = static_cast<UINT16>(mipLevels);
            texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            texDesc.SampleDesc.Count = 1;
            texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

            CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
            HRESULT hr = device.CreateCommittedResource(&defaultHeapProps,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &texDesc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST,
                                                        nullptr,
                                                        outTexture);
            if (FAILED(hr)) {
                return false;
            }
            gpuTexture = &outTexture;
        }

        const UINT subresourceCount = mipLevels * 6;
        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(gpuTexture->Get(), 0, subresourceCount);
        CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
        HRESULT hr = device.CreateCommittedResource(&uploadHeapProps,
                                                    D3D12_HEAP_FLAG_NONE,
                                                    &bufferDesc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr,
                                                    outUpload);
        if (FAILED(hr)) {
            if (!preCreated) outTexture.Reset();
            return false;
        }

        std::vector<std::vector<uint16_t>> halfData(subresourceCount);
        std::vector<D3D12_SUBRESOURCE_DATA> subresourceDescs(subresourceCount);
        for (UINT face = 0; face < 6; ++face) {
            for (UINT mip = 0; mip < mipLevels; ++mip) {
                const UINT subresourceIndex = mip + face * mipLevels;
                const UINT size = ((baseFaceSize >> mip) > 0) ? (baseFaceSize >> mip) : 1u;
                const auto& src = subresourcesRgba[subresourceIndex];
                if (src.size() != static_cast<size_t>(size) * size * 4u) {
                    return false;
                }

                auto& dst = halfData[subresourceIndex];
                dst.resize(src.size());
                for (size_t i = 0; i < src.size(); ++i) {
                    dst[i] = SasamiRenderer::Math::FloatToHalf(src[i]);
                }

                subresourceDescs[subresourceIndex].pData = dst.data();
                subresourceDescs[subresourceIndex].RowPitch = static_cast<LONG_PTR>(size) * 8;
                subresourceDescs[subresourceIndex].SlicePitch = subresourceDescs[subresourceIndex].RowPitch * size;
            }
        }

        UpdateSubresources(cmdList->Get(), gpuTexture->Get(), outUpload.Get(), 0, 0, subresourceCount, subresourceDescs.data());
        // Readable by both PS (skybox, IBL lighting) and non-PS shaders (SWRT compute IBL fallback).
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(gpuTexture->Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
        return true;
    }

    // Upload float RGBA data as a 2D texture (R16G16B16A16_FLOAT).
    // If preCreated is non-null the caller already allocated the GPU texture (via RHI); creation is skipped.
    static bool CreateTexture2DFromFloatRgba(SasamiRenderer::IRHIDevice& device,
                                             SasamiRenderer::CommandList* cmdList,
                                             const std::vector<float>& pixels,
                                             UINT width,
                                             UINT height,
                                             SasamiRenderer::Resource& outTexture,
                                             SasamiRenderer::Resource& outUpload,
                                             SasamiRenderer::Resource* preCreated = nullptr)
    {
        if (!cmdList || width == 0 || height == 0 ||
            pixels.size() != static_cast<size_t>(width) * height * 4u) {
            return false;
        }

        SasamiRenderer::Resource* gpuTexture = preCreated;
        if (!gpuTexture) {
            D3D12_RESOURCE_DESC texDesc = {};
            texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texDesc.Width = width;
            texDesc.Height = height;
            texDesc.DepthOrArraySize = 1;
            texDesc.MipLevels = 1;
            texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            texDesc.SampleDesc.Count = 1;
            texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

            CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
            HRESULT hr = device.CreateCommittedResource(&defaultHeapProps,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &texDesc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST,
                                                        nullptr,
                                                        outTexture);
            if (FAILED(hr)) {
                return false;
            }
            gpuTexture = &outTexture;
        }

        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(gpuTexture->Get(), 0, 1);
        CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
        HRESULT hr = device.CreateCommittedResource(&uploadHeapProps,
                                                    D3D12_HEAP_FLAG_NONE,
                                                    &bufferDesc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr,
                                                    outUpload);
        if (FAILED(hr)) {
            if (!preCreated) outTexture.Reset();
            return false;
        }

        std::vector<uint16_t> halfPixels(pixels.size());
        for (size_t i = 0; i < pixels.size(); ++i) {
            halfPixels[i] = SasamiRenderer::Math::FloatToHalf(pixels[i]);
        }

        D3D12_SUBRESOURCE_DATA subresource = {};
        subresource.pData = halfPixels.data();
        subresource.RowPitch = static_cast<LONG_PTR>(width) * 8;
        subresource.SlicePitch = subresource.RowPitch * height;
        UpdateSubresources(cmdList->Get(), gpuTexture->Get(), outUpload.Get(), 0, 0, 1, &subresource);

        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(gpuTexture->Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
        return true;
    }

    // Upload RGBA8 face data as a cubemap texture (R8G8B8A8_UNORM). Used for fallback (1×1) textures.
    static bool CreateTextureCubeFromRgba8Faces(SasamiRenderer::IRHIDevice& device,
                                                SasamiRenderer::CommandList* cmdList,
                                                const std::vector<std::vector<uint8_t>>& facePixels,
                                                UINT width,
                                                UINT height,
                                                SasamiRenderer::Resource& outTexture,
                                                SasamiRenderer::Resource& outUpload)
    {
        if (!cmdList || facePixels.size() != 6 || width == 0 || height == 0) {
            return false;
        }

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.DepthOrArraySize = 6;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = device.CreateCommittedResource(&defaultHeapProps,
                                                    D3D12_HEAP_FLAG_NONE,
                                                    &texDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr,
                                                    outTexture);
        if (FAILED(hr)) {
            return false;
        }

        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(outTexture.Get(), 0, 6);
        CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
        hr = device.CreateCommittedResource(&uploadHeapProps,
                                            D3D12_HEAP_FLAG_NONE,
                                            &bufferDesc,
                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr,
                                            outUpload);
        if (FAILED(hr)) {
            outTexture.Reset();
            return false;
        }

        std::vector<D3D12_SUBRESOURCE_DATA> subresources(6);
        for (int i = 0; i < 6; ++i) {
            subresources[i].pData = facePixels[static_cast<size_t>(i)].data();
            subresources[i].RowPitch = static_cast<LONG_PTR>(width) * 4;
            subresources[i].SlicePitch = subresources[i].RowPitch * height;
        }
        UpdateSubresources(cmdList->Get(), outTexture.Get(), outUpload.Get(), 0, 0, 6, subresources.data());

        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(outTexture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
        return true;
    }
}

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
                                        GeneratedIblData& outData) const
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
            irrPreCreated = m_iblIrradianceCompat;
        }
        const bool irrOk = CreateTextureCubeFromFloatFacesWithMips(*m_device,
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
            prePreCreated = m_iblPrefilterCompat;
        }
        const bool preOk = CreateTextureCubeFromFloatFacesWithMips(*m_device,
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
            brdfPreCreated = m_iblBrdfLutCompat;
        }
        const bool brdfOk = CreateTexture2DFromFloatRgba(*m_device,
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

        if (!CreateTextureCubeFromRgba8Faces(*m_device, cmdList,
                                             irradianceFaces, 1, 1,
                                             m_iblIrradianceTexture, m_iblIrradianceUpload)) {
            return false;
        }
        if (!CreateTextureCubeFromRgba8Faces(*m_device, cmdList,
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
