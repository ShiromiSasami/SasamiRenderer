#pragma once

#include "Renderer/RHI/GraphicsDevice.h"

#include <wrl.h>

#include <cstdint>
#include <filesystem>

#include <dxcapi.h>

namespace SasamiRenderer
{
    namespace DxrRayTracerUtility
    {
        std::filesystem::path GetExecutableDir();
        std::filesystem::path FindProjectRootWithShaders(const std::filesystem::path& startDir);
        const std::filesystem::path& GetShaderSourceRoot();
        std::filesystem::path GetBundledDxcRoot();

        HRESULT CreateDxcInstance(REFCLSID clsid, REFIID iid, void** outObject);
        bool CompileShaderLibrary(const std::filesystem::path& shaderPath, Microsoft::WRL::ComPtr<IDxcBlob>& outLibraryBlob);

        UINT AlignUp(UINT value, UINT alignment);

        bool CreateBuffer(IRHIDevice& device,
                           UINT64 size,
                           D3D12_RESOURCE_FLAGS flags,
                           D3D12_RESOURCE_STATES initialState,
                           Resource& outResource);

        void WriteShaderIdentifierRecord(uint8_t* destination, ID3D12StateObjectProperties* properties, LPCWSTR exportName);

        void CopyBufferData(IRHIDevice& device,
                             const void* sourceData,
                             UINT64 sourceSize,
                             Resource& destination,
                             D3D12_RESOURCE_STATES destinationState);

        bool CreateTextureUav(ID3D12Device* device, Resource& texture, CpuDescriptorHandle destination);

        void ConvertToDxrTransform(const float rowMajorRowVector[16], float outTransform[3][4]);
    }
}
