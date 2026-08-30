#include "Renderer/RayTracing/DxrRayTracerUtility.h"

#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Resources/ShaderCompilationService.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"
#include "d3dx12.h"

namespace SasamiRenderer
{
    namespace DxrRayTracerUtility
    {
        using Microsoft::WRL::ComPtr;

        std::filesystem::path GetExecutableDir()
        {
            wchar_t exePath[MAX_PATH] = {};
            const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (len == 0 || len == MAX_PATH) {
                return std::filesystem::current_path();
            }
            return std::filesystem::path(exePath).parent_path();
        }

        std::filesystem::path FindProjectRootWithShaders(const std::filesystem::path& startDir)
        {
            std::filesystem::path dir = startDir;
            for (;;) {
                const std::filesystem::path shaderDir = dir / L"Shaders";
                const std::filesystem::path legacyShaderDir = dir / L"Source" / L"Renderer" / L"Shaders";
                std::error_code ec;
                if (std::filesystem::is_directory(shaderDir, ec) ||
                    std::filesystem::is_directory(legacyShaderDir, ec)) {
                    return dir;
                }
                const std::filesystem::path parent = dir.parent_path();
                if (parent.empty() || parent == dir) {
                    break;
                }
                dir = parent;
            }
            return std::filesystem::path();
        }

        const std::filesystem::path& GetShaderSourceRoot()
        {
            static const std::filesystem::path shaderRoot = []() -> std::filesystem::path {
                const std::filesystem::path projectRoot = FindProjectRootWithShaders(GetExecutableDir());
                if (!projectRoot.empty()) {
                    const std::filesystem::path shaderDir = projectRoot / L"Shaders";
                    std::error_code ec;
                    if (std::filesystem::exists(shaderDir, ec) &&
                        std::filesystem::is_directory(shaderDir, ec)) {
                        return shaderDir;
                    }
                    return projectRoot / L"Source" / L"Renderer" / L"Shaders";
                }
                return std::filesystem::path(L"Shaders");
            }();
            return shaderRoot;
        }

        std::filesystem::path GetBundledDxcRoot()
        {
            const std::filesystem::path projectRoot = FindProjectRootWithShaders(GetExecutableDir());
            if (!projectRoot.empty()) {
                return projectRoot / L"Tools" / L"DXC" / L"bin" / L"x64";
            }
            return GetExecutableDir() / L"Tools" / L"DXC" / L"bin" / L"x64";
        }

        HRESULT CreateDxcInstance(REFCLSID clsid, REFIID iid, void** outObject)
        {
            static HMODULE dxcompilerModule = []() -> HMODULE {
                const std::filesystem::path bundledDllPath = GetBundledDxcRoot() / L"dxcompiler.dll";
                if (HMODULE module = LoadLibraryW(bundledDllPath.c_str())) {
                    return module;
                }
                return LoadLibraryW(L"dxcompiler.dll");
            }();

            static auto dxcCreateInstance = dxcompilerModule
                ? reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>(
                    GetProcAddress(dxcompilerModule, "DxcCreateInstance"))
                : nullptr;

            if (!dxcCreateInstance) {
                return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
            }

            return dxcCreateInstance(clsid, iid, outObject);
        }

        bool CompileShaderLibrary(const std::filesystem::path& shaderPath, ComPtr<IDxcBlob>& outLibraryBlob)
        {
            ComPtr<IDxcUtils> utils;
            if (FAILED(CreateDxcInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) {
                DebugLog("DxrRayTracer: failed to create DXC utils.\n");
                return false;
            }

            std::error_code ec;
            std::filesystem::path relPath = std::filesystem::relative(shaderPath, GetShaderSourceRoot(), ec);
            if (ec || relPath.empty()) {
                relPath = shaderPath.filename();
            }

            std::vector<uint8_t> bytecode;
            if (!SasamiRenderer::ShaderCompilationService::GetOrCompileShaderBytecodeDxc(
                    "DxrRayTracer", relPath.c_str(), "", "lib_6_6", bytecode)) {
                DebugLog("DxrRayTracer: failed to compile ray tracing shader library.\n");
                return false;
            }

            ComPtr<IDxcBlobEncoding> blobEncoding;
            if (FAILED(utils->CreateBlob(bytecode.data(), static_cast<UINT32>(bytecode.size()), DXC_CP_ACP, &blobEncoding)) ||
                !blobEncoding) {
                DebugLog("DxrRayTracer: failed to wrap compiled shader library bytecode.\n");
                return false;
            }

            outLibraryBlob = blobEncoding;
            return true;
        }

        UINT AlignUp(UINT value, UINT alignment)
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }

        bool CreateBuffer(IRHIDevice& device,
                           UINT64 size,
                           D3D12_RESOURCE_FLAGS flags,
                           D3D12_RESOURCE_STATES initialState,
                           Resource& outResource)
        {
            D3D12_HEAP_PROPERTIES heapProperties{};
            heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC resourceDesc{};
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            resourceDesc.Width = size;
            resourceDesc.Height = 1;
            resourceDesc.DepthOrArraySize = 1;
            resourceDesc.MipLevels = 1;
            resourceDesc.SampleDesc.Count = 1;
            resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            resourceDesc.Flags = flags;

            return SUCCEEDED(device.CreateCommittedResource(&heapProperties,
                                                            D3D12_HEAP_FLAG_NONE,
                                                            &resourceDesc,
                                                            initialState,
                                                            nullptr,
                                                            outResource));
        }

        void WriteShaderIdentifierRecord(uint8_t* destination, ID3D12StateObjectProperties* properties, LPCWSTR exportName)
        {
            const void* shaderIdentifier = properties ? properties->GetShaderIdentifier(exportName) : nullptr;
            if (shaderIdentifier) {
                std::memcpy(destination, shaderIdentifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
            } else {
                std::memset(destination, 0, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
            }
        }

        void CopyBufferData(IRHIDevice& device,
                             const void* sourceData,
                             UINT64 sourceSize,
                             Resource& destination,
                             D3D12_RESOURCE_STATES destinationState)
        {
            if (!sourceData || sourceSize == 0u || !destination.IsValid()) {
                return;
            }

            CommandAllocator allocator;
            CommandList commandList;
            if (FAILED(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, allocator)) ||
                FAILED(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, commandList))) {
                return;
            }

            Resource uploadBuffer;
            void* mappedPtr = nullptr;
            if (!ResourceUploadUtility::CreateUploadBuffer(device, sourceSize, uploadBuffer, &mappedPtr)) {
                return;
            }
            std::memcpy(mappedPtr, sourceData, static_cast<size_t>(sourceSize));
            uploadBuffer->Unmap(0, nullptr);

            commandList.CopyBufferRegion(destination, 0u, uploadBuffer, 0u, sourceSize);
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(destination.Get(),
                                                                      D3D12_RESOURCE_STATE_COPY_DEST,
                                                                      destinationState);
            commandList.ResourceBarrier(1u, &barrier);
            commandList.Close();
            ID3D12CommandList* commandLists[] = { commandList.Get() };
            device.GetCommandQueue()->ExecuteCommandLists(1u, commandLists);
            device.WaitForGPU();
        }

        bool CreateTextureUav(ID3D12Device* device, Resource& texture, CpuDescriptorHandle destination)
        {
            if (!device || !texture.IsValid()) {
                return false;
            }

            D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(texture.Get(), nullptr, &desc, destination);
            return true;
        }

        void ConvertToDxrTransform(const float rowMajorRowVector[16], float outTransform[3][4])
        {
            // Engine matrices are row-major with row-vector convention.
            // DXR instance desc expects a row-major 3x4 matrix in standard affine form,
            // so transpose and keep the upper 3 rows.
            const float transposed[16] = {
                rowMajorRowVector[0], rowMajorRowVector[4], rowMajorRowVector[8],  rowMajorRowVector[12],
                rowMajorRowVector[1], rowMajorRowVector[5], rowMajorRowVector[9],  rowMajorRowVector[13],
                rowMajorRowVector[2], rowMajorRowVector[6], rowMajorRowVector[10], rowMajorRowVector[14],
                rowMajorRowVector[3], rowMajorRowVector[7], rowMajorRowVector[11], rowMajorRowVector[15],
            };

            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 4; ++column) {
                    outTransform[row][column] = transposed[row * 4 + column];
                }
            }
        }
    }
}
