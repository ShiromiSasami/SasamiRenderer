#include "Renderer/RayTracing/GpuSoftwareRayTracerResourceUtility.h"

namespace SasamiRenderer
{
    namespace GpuSoftwareRayTracerResourceUtility
    {
        void CreateStructuredSrv(ID3D12Device* dev, Resource& buf,
                                 UINT elemCount, UINT stride,
                                 D3D12_CPU_DESCRIPTOR_HANDLE dest)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Format                  = DXGI_FORMAT_UNKNOWN;
            d.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            d.Buffer.FirstElement     = 0;
            d.Buffer.NumElements      = elemCount;
            d.Buffer.StructureByteStride = stride;
            d.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_NONE;
            dev->CreateShaderResourceView(buf.Get(), &d, dest);
        }

        // ---- Create a null SRV (for empty buffers) ----
        void CreateNullStructuredSrv(ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE dest)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Format                  = DXGI_FORMAT_UNKNOWN;
            d.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            d.Buffer.NumElements      = 0;
            d.Buffer.StructureByteStride = 4u;
            dev->CreateShaderResourceView(nullptr, &d, dest);
        }

        // ---- Create a UAV-capable default-heap buffer (no initial data) ----
        bool CreateUavBuffer(IRHIDevice& device, UINT64 byteSize,
                             D3D12_RESOURCE_STATES initialState, Resource& out)
        {
            if (byteSize == 0) { out.Reset(); return false; }
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width     = byteSize;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return SUCCEEDED(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                &desc, initialState, nullptr, out));
        }

        // ---- Create a Texture2D with UAV support ----
        bool CreateUavTexture2D(IRHIDevice& device, UINT w, UINT h, DXGI_FORMAT fmt,
                                D3D12_RESOURCE_STATES initialState, Resource& out)
        {
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width     = w; desc.Height = h;
            desc.DepthOrArraySize = 1; desc.MipLevels = 1;
            desc.Format    = fmt;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return SUCCEEDED(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                &desc, initialState, nullptr, out));
        }

        // ---- Transition barrier shorthand ----
        D3D12_RESOURCE_BARRIER MakeTransition(ID3D12Resource* res,
                                               D3D12_RESOURCE_STATES from,
                                               D3D12_RESOURCE_STATES to)
        {
            return CD3DX12_RESOURCE_BARRIER::Transition(res, from, to);
        }

        // ---- Create a Texture2D SRV into a given CPU handle ----
        void CreateTex2DSrv(ID3D12Device* dev, ID3D12Resource* tex,
                            DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE dest)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Format      = fmt;
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(tex, &d, dest);
        }

        void CreateNullTex2DSrv(ID3D12Device* dev, DXGI_FORMAT fmt,
                                D3D12_CPU_DESCRIPTOR_HANDLE dest)
        {
            CreateTex2DSrv(dev, nullptr, fmt, dest);
        }

        // ---- Create a Texture2D UAV into a given CPU handle ----
        void CreateTex2DUav(ID3D12Device* dev, ID3D12Resource* tex,
                            DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE dest)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
            d.Format        = fmt;
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            dev->CreateUnorderedAccessView(tex, nullptr, &d, dest);
        }

        void CreateNullTex2DUav(ID3D12Device* dev, DXGI_FORMAT fmt,
                                D3D12_CPU_DESCRIPTOR_HANDLE dest)
        {
            CreateTex2DUav(dev, nullptr, fmt, dest);
        }
    }
}
