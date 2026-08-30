#pragma once

#include "Renderer/RHI/GraphicsDevice.h"

namespace SasamiRenderer
{
    namespace GpuSoftwareRayTracerResourceUtility
    {
        void CreateStructuredSrv(ID3D12Device* dev, Resource& buf,
                                 UINT elemCount, UINT stride,
                                 D3D12_CPU_DESCRIPTOR_HANDLE dest);

        void CreateNullStructuredSrv(ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE dest);

        bool CreateUavBuffer(IRHIDevice& device, UINT64 byteSize,
                             D3D12_RESOURCE_STATES initialState, Resource& out);

        bool CreateUavTexture2D(IRHIDevice& device, UINT w, UINT h, DXGI_FORMAT fmt,
                                D3D12_RESOURCE_STATES initialState, Resource& out);

        D3D12_RESOURCE_BARRIER MakeTransition(ID3D12Resource* res,
                                               D3D12_RESOURCE_STATES from,
                                               D3D12_RESOURCE_STATES to);

        void CreateTex2DSrv(ID3D12Device* dev, ID3D12Resource* tex,
                            DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE dest);

        void CreateNullTex2DSrv(ID3D12Device* dev, DXGI_FORMAT fmt,
                                D3D12_CPU_DESCRIPTOR_HANDLE dest);

        void CreateTex2DUav(ID3D12Device* dev, ID3D12Resource* tex,
                            DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE dest);

        void CreateNullTex2DUav(ID3D12Device* dev, DXGI_FORMAT fmt,
                                D3D12_CPU_DESCRIPTOR_HANDLE dest);
    }
}
