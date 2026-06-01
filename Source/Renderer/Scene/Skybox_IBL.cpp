// Skybox_IBL.cpp
// IBL (Image-Based Lighting) generation and upload for Skybox.
#include "Renderer/Scene/Skybox.h"

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
    struct SkyboxVertex
    {
        float position[3];
    };

    static const SkyboxVertex kSkyboxCubeVertices[] = {
        { { 1.0f, -1.0f, -1.0f } }, { { 1.0f, -1.0f,  1.0f } }, { { 1.0f,  1.0f,  1.0f } },
        { { 1.0f, -1.0f, -1.0f } }, { { 1.0f,  1.0f,  1.0f } }, { { 1.0f,  1.0f, -1.0f } },
        { { -1.0f, -1.0f,  1.0f } }, { { -1.0f, -1.0f, -1.0f } }, { { -1.0f,  1.0f, -1.0f } },
        { { -1.0f, -1.0f,  1.0f } }, { { -1.0f,  1.0f, -1.0f } }, { { -1.0f,  1.0f,  1.0f } },
        { { -1.0f, 1.0f, -1.0f } }, { { 1.0f, 1.0f, -1.0f } }, { { 1.0f, 1.0f, 1.0f } },
        { { -1.0f, 1.0f, -1.0f } }, { { 1.0f, 1.0f, 1.0f } }, { { -1.0f, 1.0f, 1.0f } },
        { { -1.0f, -1.0f,  1.0f } }, { { 1.0f, -1.0f,  1.0f } }, { { 1.0f, -1.0f, -1.0f } },
        { { -1.0f, -1.0f,  1.0f } }, { { 1.0f, -1.0f, -1.0f } }, { { -1.0f, -1.0f, -1.0f } },
        { { -1.0f, -1.0f, 1.0f } }, { { -1.0f,  1.0f, 1.0f } }, { { 1.0f,  1.0f, 1.0f } },
        { { -1.0f, -1.0f, 1.0f } }, { { 1.0f,  1.0f, 1.0f } }, { { 1.0f, -1.0f, 1.0f } },
        { { 1.0f, -1.0f, -1.0f } }, { { 1.0f,  1.0f, -1.0f } }, { { -1.0f,  1.0f, -1.0f } },
        { { 1.0f, -1.0f, -1.0f } }, { { -1.0f,  1.0f, -1.0f } }, { { -1.0f, -1.0f, -1.0f } },
    };

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

    static bool CreateTextureCubeFromFloatFacesWithMips(SasamiRenderer::IRHIDevice& device,
                                                        SasamiRenderer::CommandList* cmdList,
                                                        const std::vector<std::vector<float>>& subresourcesRgba,
                                                        UINT baseFaceSize,
                                                        UINT mipLevels,
                                                        SasamiRenderer::Resource& outTexture,
                                                        SasamiRenderer::Resource& outUpload)
    {
        if (!cmdList || baseFaceSize == 0 || mipLevels == 0 ||
            subresourcesRgba.size() != static_cast<size_t>(mipLevels) * 6u) {
            return false;
        }

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

        const UINT subresourceCount = mipLevels * 6;
        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(outTexture.Get(), 0, subresourceCount);
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

        UpdateSubresources(cmdList->Get(), outTexture.Get(), outUpload.Get(), 0, 0, subresourceCount, subresourceDescs.data());
        // Use combined state so the cubemap is readable by both PS (skybox, IBL lighting)
        // and non-pixel shaders (SWRT compute shader IBL fallback sampling).
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(outTexture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
        return true;
    }

    static bool CreateTexture2DFromFloatRgba(SasamiRenderer::IRHIDevice& device,
                                             SasamiRenderer::CommandList* cmdList,
                                             const std::vector<float>& pixels,
                                             UINT width,
                                             UINT height,
                                             SasamiRenderer::Resource& outTexture,
                                             SasamiRenderer::Resource& outUpload)
    {
        if (!cmdList || width == 0 || height == 0 ||
            pixels.size() != static_cast<size_t>(width) * height * 4u) {
            return false;
        }

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

        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(outTexture.Get(), 0, 1);
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

        std::vector<uint16_t> halfPixels(pixels.size());
        for (size_t i = 0; i < pixels.size(); ++i) {
            halfPixels[i] = SasamiRenderer::Math::FloatToHalf(pixels[i]);
        }

        D3D12_SUBRESOURCE_DATA subresource = {};
        subresource.pData = halfPixels.data();
        subresource.RowPitch = static_cast<LONG_PTR>(width) * 8;
        subresource.SlicePitch = subresource.RowPitch * height;
        UpdateSubresources(cmdList->Get(), outTexture.Get(), outUpload.Get(), 0, 0, 1, &subresource);

        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(outTexture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
        return true;
    }
}

namespace SasamiRenderer
{
    bool Skybox::GenerateHdrIblData(GeneratedIblData& outData) const
    {
        if (!m_hdrEquirectLoaded) {
            return false;
        }

        const UINT irradianceSize = 16;
        const UINT prefilterSize = 64;
        const UINT prefilterMipLevels = 4;
        const UINT brdfLutSize = 64;

        outData = {};
        outData.prefilterMipLevels = prefilterMipLevels;

        RendererMathUtility::GenerateIrradianceCubemapFromEquirect(m_hdrEquirectPixels,
                                                                   m_hdrEquirectWidth,
                                                                   m_hdrEquirectHeight,
                                                                   irradianceSize,
                                                                   outData.irradianceFaces);
        RendererMathUtility::GeneratePrefilterCubemapFromEquirect(m_hdrEquirectPixels,
                                                                  m_hdrEquirectWidth,
                                                                  m_hdrEquirectHeight,
                                                                  prefilterSize,
                                                                  prefilterMipLevels,
                                                                  outData.prefilterSubresources);
        RendererMathUtility::GenerateBrdfLut(brdfLutSize, brdfLutSize, outData.brdfLutPixels);
        RendererMathUtility::GenerateDiffuseShCoefficientsFromEquirect(m_hdrEquirectPixels,
                                                                        m_hdrEquirectWidth,
                                                                        m_hdrEquirectHeight,
                                                                        outData.diffuseShCoefficients);

        return outData.irradianceFaces.size() == 6u &&
               outData.prefilterSubresources.size() == static_cast<size_t>(prefilterMipLevels) * 6u &&
               !outData.brdfLutPixels.empty();
    }

    void Skybox::PublishIblSrvs(DXGI_FORMAT cubeFormat, DXGI_FORMAT brdfFormat, UINT prefilterMipLevels)
    {
        if (!m_device) {
            return;
        }

        const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        CpuDescriptorHandle irrCpu = m_iblSrvCpu;
        CpuDescriptorHandle preCpu = { m_iblSrvCpu.ptr + static_cast<SIZE_T>(inc) };
        CpuDescriptorHandle brdfCpu = { m_iblSrvCpu.ptr + static_cast<SIZE_T>(inc) * 2 };

        D3D12_SHADER_RESOURCE_VIEW_DESC cubeSrvDesc = {};
        cubeSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        cubeSrvDesc.Format = cubeFormat;
        cubeSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        cubeSrvDesc.TextureCube.MipLevels = 1;
        cubeSrvDesc.TextureCube.MostDetailedMip = 0;
        cubeSrvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
        m_device->CreateShaderResourceView(m_iblIrradianceTexture, &cubeSrvDesc, irrCpu);

        D3D12_SHADER_RESOURCE_VIEW_DESC preCubeSrvDesc = cubeSrvDesc;
        preCubeSrvDesc.TextureCube.MipLevels = prefilterMipLevels;
        m_device->CreateShaderResourceView(m_iblPrefilterTexture, &preCubeSrvDesc, preCpu);

        D3D12_SHADER_RESOURCE_VIEW_DESC lutSrvDesc = {};
        lutSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        lutSrvDesc.Format = brdfFormat;
        lutSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        lutSrvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_iblBrdfLutTexture, &lutSrvDesc, brdfCpu);
    }

    void Skybox::ApplyIblMetadata(bool enabled, float prefilterMaxMip, bool shValid, const float (*diffuseSh)[3])
    {
        m_iblEnabled = enabled;
        m_iblPrefilterMaxMip = prefilterMaxMip;
        m_diffuseShValid = shValid;
        if (shValid && diffuseSh != nullptr) {
            std::memcpy(m_diffuseSh, diffuseSh, sizeof(m_diffuseSh));
        } else {
            std::memset(m_diffuseSh, 0, sizeof(m_diffuseSh));
        }
    }

    bool Skybox::UploadGeneratedIblTextures(CommandList* cmdList, const GeneratedIblData& data)
    {
        if (!m_device || !cmdList) {
            return false;
        }

        const UINT irradianceSize = 16;
        const UINT prefilterSize = 64;
        const UINT brdfLutSize = 64;
        const bool irrOk = CreateTextureCubeFromFloatFacesWithMips(*m_device,
                                                                    cmdList,
                                                                    data.irradianceFaces,
                                                                    irradianceSize,
                                                                    1,
                                                                    m_iblIrradianceTexture,
                                                                    m_iblIrradianceUpload);
        const bool preOk = CreateTextureCubeFromFloatFacesWithMips(*m_device,
                                                                    cmdList,
                                                                    data.prefilterSubresources,
                                                                    prefilterSize,
                                                                    data.prefilterMipLevels,
                                                                    m_iblPrefilterTexture,
                                                                    m_iblPrefilterUpload);
        const bool brdfOk = CreateTexture2DFromFloatRgba(*m_device,
                                                          cmdList,
                                                          data.brdfLutPixels,
                                                          brdfLutSize,
                                                          brdfLutSize,
                                                          m_iblBrdfLutTexture,
                                                          m_iblBrdfLutUpload);
        if (!irrOk || !preOk || !brdfOk) {
            return false;
        }

        PublishIblSrvs(DXGI_FORMAT_R16G16B16A16_FLOAT,
                       DXGI_FORMAT_R16G16B16A16_FLOAT,
                       data.prefilterMipLevels);
        m_cpuPrefilterSubresources = data.prefilterSubresources;
        m_cpuBrdfLutPixels = data.brdfLutPixels;
        m_cpuPrefilterBaseSize = prefilterSize;
        m_cpuPrefilterMipLevels = data.prefilterMipLevels;
        m_cpuBrdfLutWidth = brdfLutSize;
        m_cpuBrdfLutHeight = brdfLutSize;
        return true;
    }

    bool Skybox::UploadFallbackIblTextures(CommandList* cmdList)
    {
        if (!m_device || !cmdList) {
            return false;
        }

        std::vector<std::vector<uint8_t>> irradianceFaces;
        std::vector<std::vector<uint8_t>> prefilterFaces;
        std::vector<uint8_t> brdfPixels;
        static const uint8_t blackFace[4] = { 0, 0, 0, 255 };
        static const uint8_t midFace[4] = { 128, 128, 128, 255 };

        irradianceFaces.assign(6, std::vector<uint8_t>(blackFace, blackFace + 4));
        prefilterFaces.assign(6, std::vector<uint8_t>(blackFace, blackFace + 4));
        brdfPixels.assign(midFace, midFace + 4);

        if (!CreateTextureCubeFromRgba8Faces(*m_device,
                                             cmdList,
                                             irradianceFaces,
                                             1,
                                             1,
                                             m_iblIrradianceTexture,
                                             m_iblIrradianceUpload)) {
            return false;
        }
        if (!CreateTextureCubeFromRgba8Faces(*m_device,
                                             cmdList,
                                             prefilterFaces,
                                             1,
                                             1,
                                             m_iblPrefilterTexture,
                                             m_iblPrefilterUpload)) {
            return false;
        }
        if (!ResourceUploadUtility::CreateTexture2DFromRgba8(*m_device,
                                                             cmdList,
                                                             brdfPixels.data(),
                                                             1,
                                                             1,
                                                             m_iblBrdfLutTexture,
                                                             m_iblBrdfLutUpload)) {
            return false;
        }

        PublishIblSrvs(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, 1);
        return true;
    }

    void Skybox::EnsureIblTexturesUploaded(CommandList* cmdList)
    {
        if (!m_device || m_iblUploaded || m_iblUploadAttempted) {
            return;
        }
        m_iblUploadAttempted = true;

        if (EnsureHdrEnvironmentLoaded()) {
            GeneratedIblData generated{};
            if (GenerateHdrIblData(generated) && UploadGeneratedIblTextures(cmdList, generated)) {
                const float maxMip = (generated.prefilterMipLevels > 0u)
                    ? static_cast<float>(generated.prefilterMipLevels - 1u)
                    : 0.0f;
                ApplyIblMetadata(true, maxMip, true, generated.diffuseShCoefficients);
                m_iblUploaded = true;
                return;
            }

            ResetIblResources();
            m_iblUploadAttempted = true;
            DebugLog("Runtime equirect->IBL generation failed. Fallback IBL will be used.\n");
        }

        if (!UploadFallbackIblTextures(cmdList)) {
            return;
        }
        ApplyIblMetadata(false, 0.0f, false, nullptr);
        m_iblUploaded = true;
    }


} // namespace SasamiRenderer
