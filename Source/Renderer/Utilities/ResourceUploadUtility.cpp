#include "Renderer/Utilities/ResourceUploadUtility.h"

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
                if (FAILED(hr)) {
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
    }
}
