#include "Renderer/Utilities/ResourceUploadUtility.h"

#include "Foundation/Math/MathUtil.h"
#include "d3dx12.h"

namespace SasamiRenderer
{
    namespace ResourceUploadUtility
    {
        bool CreateUploadBuffer(IRHIDevice& device,
                                std::uint64_t size,
                                Resource& outResource,
                                void** outMappedPtr)
        {
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC resDesc = {};
            resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resDesc.Width = size;
            resDesc.Height = 1;
            resDesc.DepthOrArraySize = 1;
            resDesc.MipLevels = 1;
            resDesc.SampleDesc.Count = 1;
            resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            HRESULT hr = device.CreateCommittedResource(&heapProps,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &resDesc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ,
                                                        nullptr,
                                                        outResource);
            if (FAILED(hr)) {
                return false;
            }

            if (outMappedPtr) {
                hr = outResource->Map(0, nullptr, outMappedPtr);
                if (FAILED(hr) || !*outMappedPtr) {
                    outResource.Reset();
                    *outMappedPtr = nullptr;
                    return false;
                }
            }

            return true;
        }

        bool CreateTexture2DFromRgba8(IRHIDevice& device,
                                      CommandList* cmdList,
                                      const std::uint8_t* pixels,
                                      UINT width,
                                      UINT height,
                                      Resource& outTexture,
                                      Resource& outUpload)
        {
            if (!pixels || width == 0 || height == 0 || !cmdList) {
                return false;
            }

            D3D12_RESOURCE_DESC texDesc = {};
            texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texDesc.Width = width;
            texDesc.Height = height;
            texDesc.DepthOrArraySize = 1;
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

            D3D12_SUBRESOURCE_DATA textureData = {};
            textureData.pData = pixels;
            textureData.RowPitch = static_cast<LONG_PTR>(width) * 4;
            textureData.SlicePitch = textureData.RowPitch * height;
            UpdateSubresources(cmdList->Get(), outTexture.Get(), outUpload.Get(), 0, 0, 1, &textureData);

            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(outTexture.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList->ResourceBarrier(1, &barrier);
            return true;
        }

        bool CreateTexture2DFromRgba8WithMips(IRHIDevice& device,
                                              CommandList* cmdList,
                                              const TextureMipChain& chain,
                                              Resource& outTexture,
                                              Resource& outUpload)
        {
            if (!cmdList || !chain.IsValid() ||
                chain.levels[0].width == 0 || chain.levels[0].height == 0) {
                return false;
            }

            const UINT mipCount = static_cast<UINT>(chain.levels.size());

            D3D12_RESOURCE_DESC texDesc = {};
            texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texDesc.Width = chain.levels[0].width;
            texDesc.Height = chain.levels[0].height;
            texDesc.DepthOrArraySize = 1;
            texDesc.MipLevels = static_cast<UINT16>(chain.levels.size());
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

            const UINT64 uploadBufferSize = GetRequiredIntermediateSize(outTexture.Get(), 0, mipCount);
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

            std::vector<D3D12_SUBRESOURCE_DATA> subresources(mipCount);
            for (UINT mip = 0; mip < mipCount; ++mip) {
                const auto& level = chain.levels[mip];
                subresources[mip].pData = chain.data.data() + level.byteOffset;
                subresources[mip].RowPitch = static_cast<LONG_PTR>(level.width) * 4;
                subresources[mip].SlicePitch = subresources[mip].RowPitch * level.height;
            }
            UpdateSubresources(cmdList->Get(), outTexture.Get(), outUpload.Get(), 0, 0, mipCount, subresources.data());

            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(outTexture.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList->ResourceBarrier(1, &barrier);
            return true;
        }

        bool CreateTextureCubeFromRgba8Faces(IRHIDevice& device,
                                             CommandList* cmdList,
                                             const std::vector<std::vector<std::uint8_t>>& facePixels,
                                             UINT width,
                                             UINT height,
                                             Resource& outTexture,
                                             Resource& outUpload)
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

        bool CreateTextureCubeFromFloatFacesWithMips(IRHIDevice& device,
                                                     CommandList* cmdList,
                                                     const std::vector<std::vector<float>>& subresourcesRgba,
                                                     UINT baseFaceSize,
                                                     UINT mipLevels,
                                                     Resource& outTexture,
                                                     Resource& outUpload,
                                                     Resource* preCreated)
        {
            if (!cmdList || baseFaceSize == 0 || mipLevels == 0 ||
                subresourcesRgba.size() != static_cast<size_t>(mipLevels) * 6u) {
                return false;
            }

            Resource* gpuTexture = preCreated;
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
                        if (!preCreated) outTexture.Reset();
                        outUpload.Reset();
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
            // Use combined state so the cubemap is readable by both PS (skybox, IBL lighting)
            // and non-pixel shaders (SWRT compute shader IBL fallback sampling).
            auto cubeBarrier = CD3DX12_RESOURCE_BARRIER::Transition(gpuTexture->Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList->ResourceBarrier(1, &cubeBarrier);
            return true;
        }

        bool CreateTexture2DFromFloatRgba(IRHIDevice& device,
                                          CommandList* cmdList,
                                          const std::vector<float>& pixels,
                                          UINT width,
                                          UINT height,
                                          Resource& outTexture,
                                          Resource& outUpload,
                                          Resource* preCreated)
        {
            if (!cmdList || width == 0 || height == 0 ||
                pixels.size() != static_cast<size_t>(width) * height * 4u) {
                return false;
            }

            Resource* gpuTexture = preCreated;
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

            auto barrier2d = CD3DX12_RESOURCE_BARRIER::Transition(gpuTexture->Get(),
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cmdList->ResourceBarrier(1, &barrier2d);
            return true;
        }
    }
}
