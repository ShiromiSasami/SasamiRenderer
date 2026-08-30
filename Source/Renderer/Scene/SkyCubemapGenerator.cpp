#include "Renderer/Scene/SkyCubemapGenerator.h"

#include "Renderer/Resources/RenderPipelineStateCacheLog.h"
#include "Renderer/Resources/ShaderCompilationService.h"

#include "Foundation/Tools/DebugOutput.h"

#include "d3dx12.h"

#include <DirectXPackedVector.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace SasamiRenderer
{
    namespace
    {
        // Same file-local shader loader used by RenderPipelineStateCacheSsr.cpp
        // (each PSO-building translation unit keeps its own copy rather than
        // sharing one across .cpp files).
        bool LoadShaderBlob(ShaderBlobCache& cache,
                            const wchar_t* relativePath,
                            const char* entry,
                            const char* target,
                            Microsoft::WRL::ComPtr<ID3DBlob>& outBlob)
        {
            const std::filesystem::path sourcePath = ShaderCompilationService::ResolveShaderPath(relativePath);
            const std::filesystem::path compiledPath =
                ShaderCompilationService::ResolveCompiledShaderPath(sourcePath, entry, target);
            outBlob.Reset();

            outBlob = cache.GetOrResolve(compiledPath.wstring(), [&]() -> Microsoft::WRL::ComPtr<ID3DBlob>
            {
                Microsoft::WRL::ComPtr<ID3DBlob> blob;
                if (ShaderCompilationService::IsCompiledShaderUpToDate(compiledPath, sourcePath) &&
                    SUCCEEDED(D3DReadFileToBlob(compiledPath.c_str(), blob.GetAddressOf()))) {
                    ShaderCompilationService::LogShaderResolveMessage(sourcePath, entry, target,
                                                                      "loaded precompiled shader", compiledPath);
                    return blob;
                }

                ShaderCompilationService::LogShaderResolveMessage(sourcePath, entry, target,
                                                                  "precompiled shader missing or stale, runtime compiling",
                                                                  compiledPath);
                if (!ShaderCompilationService::CompileShader(sourcePath, entry, target, blob)) {
                    return Microsoft::WRL::ComPtr<ID3DBlob>();
                }

                std::error_code ec;
                std::filesystem::create_directories(compiledPath.parent_path(), ec);
                if (ec) {
                    ShaderCompilationService::LogShaderResolveMessage(sourcePath, entry, target,
                                                                      "cache directory creation failed; keeping runtime blob only",
                                                                      compiledPath);
                    return blob;
                }

                const HRESULT writeHr = D3DWriteBlobToFile(blob.Get(), compiledPath.c_str(), TRUE);
                if (FAILED(writeHr)) {
                    ShaderCompilationService::LogShaderResolveMessage(sourcePath, entry, target,
                                                                      "compiled shader could not be written",
                                                                      compiledPath);
                } else {
                    ShaderCompilationService::LogShaderResolveMessage(sourcePath, entry, target,
                                                                      "runtime compiled shader and updated cache",
                                                                      compiledPath);
                }
                return blob;
            });

            return outBlob.Get() != nullptr;
        }
    }

    bool SkyCubemapGenerator::Initialize(IRHIDevice& device, const std::string& computeProfile, SrvAllocFn srvAlloc, DescriptorHeap* srvHeap)
    {
        m_device = &device;
        m_srvAlloc = std::move(srvAlloc);
        m_srvHeap = srvHeap;
        m_ready = false;

        if (!CreateFromEquirectPipeline(device, computeProfile)) {
            DebugLog("SkyCubemapGenerator::Initialize: FromEquirect pipeline creation failed.\n");
            return false;
        }
        if (!CreateDownsamplePipeline(device, computeProfile)) {
            DebugLog("SkyCubemapGenerator::Initialize: Downsample pipeline creation failed.\n");
            return false;
        }

        m_ready = true;
        return true;
    }

    bool SkyCubemapGenerator::CreateFromEquirectPipeline(IRHIDevice& device, const std::string& computeProfile)
    {
        D3D12_DESCRIPTOR_RANGE equirectRange{
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        D3D12_DESCRIPTOR_RANGE cubeMip0Range{
            D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &equirectRange;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &cubeMip0Range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants.ShaderRegister = 0;
        params[2].Constants.RegisterSpace = 0;
        params[2].Constants.Num32BitValues = 4;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Wrap-X / clamp-Y bilinear sampler: reproduces the CPU version's manual
        // bilinear tap, which wrapped the equirect's horizontal (longitude) seam
        // and clamped the vertical (latitude) poles. See SkyCubemapFromEquirect_CS.hlsl.
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister = 0;
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = _countof(params);
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &sampler;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> sig;
        Microsoft::WRL::ComPtr<ID3DBlob> err;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr)) {
            if (err && err->GetBufferPointer()) {
                DebugLog(static_cast<const char*>(err->GetBufferPointer()));
                DebugLog("\n");
            }
            LogFail("SkyCubemapGenerator: FromEquirect SerializeRootSignature", hr);
            return false;
        }

        hr = device.CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), m_fromEquirectRootSignature);
        if (FAILED(hr)) {
            LogFail("SkyCubemapGenerator: FromEquirect CreateRootSignature", hr);
            return false;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> cs;
        if (!LoadShaderBlob(m_shaderBlobCache,
                            L"Compute/Sky/SkyCubemapFromEquirect_CS.hlsl",
                            "CSMain",
                            computeProfile.c_str(),
                            cs)) {
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_fromEquirectRootSignature.Get();
        pso.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        hr = device.CreateComputePipelineState(pso, m_fromEquirectPipelineState);
        if (FAILED(hr)) {
            LogFail("SkyCubemapGenerator: FromEquirect CreateComputePipelineState", hr);
            return false;
        }
        return true;
    }

    bool SkyCubemapGenerator::CreateDownsamplePipeline(IRHIDevice& device, const std::string& computeProfile)
    {
        D3D12_DESCRIPTOR_RANGE srcMipRange{
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        D3D12_DESCRIPTOR_RANGE dstMipRange{
            D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &srcMipRange;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &dstMipRange;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants.ShaderRegister = 0;
        params[2].Constants.RegisterSpace = 0;
        params[2].Constants.Num32BitValues = 4;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = _countof(params);
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 0;
        rsDesc.pStaticSamplers = nullptr;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> sig;
        Microsoft::WRL::ComPtr<ID3DBlob> err;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr)) {
            if (err && err->GetBufferPointer()) {
                DebugLog(static_cast<const char*>(err->GetBufferPointer()));
                DebugLog("\n");
            }
            LogFail("SkyCubemapGenerator: Downsample SerializeRootSignature", hr);
            return false;
        }

        hr = device.CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), m_downsampleRootSignature);
        if (FAILED(hr)) {
            LogFail("SkyCubemapGenerator: Downsample CreateRootSignature", hr);
            return false;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> cs;
        if (!LoadShaderBlob(m_shaderBlobCache,
                            L"Compute/Sky/SkyCubemapDownsample_CS.hlsl",
                            "CSMain",
                            computeProfile.c_str(),
                            cs)) {
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_downsampleRootSignature.Get();
        pso.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        hr = device.CreateComputePipelineState(pso, m_downsamplePipelineState);
        if (FAILED(hr)) {
            LogFail("SkyCubemapGenerator: Downsample CreateComputePipelineState", hr);
            return false;
        }
        return true;
    }

    bool SkyCubemapGenerator::UploadEquirectTexture(CommandList* cmdList,
                                                    const std::vector<float>& equirectRgb,
                                                    UINT srcWidth,
                                                    UINT srcHeight)
    {
        if (!m_device || !cmdList) {
            return false;
        }

        // Expand the tightly-packed RGB float source to RGBA16F: the shader binds
        // the equirect as a Texture2D<float4>, and there is no RGB16F DXGI format.
        const size_t texelCount = static_cast<size_t>(srcWidth) * static_cast<size_t>(srcHeight);
        std::vector<DirectX::PackedVector::HALF> halfPixels(texelCount * 4u);
        const DirectX::PackedVector::HALF alphaOne = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
        for (size_t i = 0; i < texelCount; ++i) {
            const size_t srcIdx = i * 3u;
            const size_t dstIdx = i * 4u;
            halfPixels[dstIdx + 0] = DirectX::PackedVector::XMConvertFloatToHalf(equirectRgb[srcIdx + 0]);
            halfPixels[dstIdx + 1] = DirectX::PackedVector::XMConvertFloatToHalf(equirectRgb[srcIdx + 1]);
            halfPixels[dstIdx + 2] = DirectX::PackedVector::XMConvertFloatToHalf(equirectRgb[srcIdx + 2]);
            halfPixels[dstIdx + 3] = alphaOne;
        }

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = srcWidth;
        texDesc.Height = srcHeight;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
        m_equirectTexture.Reset();
        HRESULT hr = m_device->CreateCommittedResource(&defaultHeapProps,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &texDesc,
                                                        D3D12_RESOURCE_STATE_COPY_DEST,
                                                        nullptr,
                                                        m_equirectTexture);
        if (FAILED(hr)) {
            return false;
        }

        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(m_equirectTexture.Get(), 0, 1);
        CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
        m_equirectUpload.Reset();
        hr = m_device->CreateCommittedResource(&uploadHeapProps,
                                                D3D12_HEAP_FLAG_NONE,
                                                &bufferDesc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr,
                                                m_equirectUpload);
        if (FAILED(hr)) {
            m_equirectTexture.Reset();
            return false;
        }

        D3D12_SUBRESOURCE_DATA subresource = {};
        subresource.pData = halfPixels.data();
        subresource.RowPitch = static_cast<LONG_PTR>(srcWidth) * 8;
        subresource.SlicePitch = subresource.RowPitch * srcHeight;
        UpdateSubresources(cmdList->Get(), m_equirectTexture.Get(), m_equirectUpload.Get(), 0, 0, 1, &subresource);

        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_equirectTexture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cmdList->ResourceBarrier(1, &barrier);
        return true;
    }

    bool SkyCubemapGenerator::EnsureDescriptors(UINT faceSize, UINT mipLevels)
    {
        if (m_equirectSrvCpu.ptr == 0) {
            CpuDescriptorHandle cpu{};
            GpuDescriptorHandle gpu{};
            if (!m_srvAlloc || !m_srvAlloc(1, cpu, gpu)) {
                return false;
            }
            m_equirectSrvCpu = cpu;
            m_equirectSrvGpu = gpu;
        }

        if (faceSize == m_cachedFaceSize && mipLevels == m_cachedMipLevels &&
            m_mipUavGpu.size() == mipLevels && m_mipSrvGpu.size() == mipLevels - 1) {
            return true;
        }

        m_mipUavCpu.clear();
        m_mipUavGpu.clear();
        m_mipSrvCpu.clear();
        m_mipSrvGpu.clear();
        m_mipUavCpu.reserve(mipLevels);
        m_mipUavGpu.reserve(mipLevels);
        m_mipSrvCpu.reserve(mipLevels > 0 ? mipLevels - 1 : 0);
        m_mipSrvGpu.reserve(mipLevels > 0 ? mipLevels - 1 : 0);

        for (UINT m = 0; m < mipLevels; ++m) {
            CpuDescriptorHandle cpu{};
            GpuDescriptorHandle gpu{};
            if (!m_srvAlloc || !m_srvAlloc(1, cpu, gpu)) {
                return false;
            }
            m_mipUavCpu.push_back(cpu);
            m_mipUavGpu.push_back(gpu);
        }
        for (UINT m = 0; m + 1 < mipLevels; ++m) {
            CpuDescriptorHandle cpu{};
            GpuDescriptorHandle gpu{};
            if (!m_srvAlloc || !m_srvAlloc(1, cpu, gpu)) {
                return false;
            }
            m_mipSrvCpu.push_back(cpu);
            m_mipSrvGpu.push_back(gpu);
        }

        m_cachedFaceSize = faceSize;
        m_cachedMipLevels = mipLevels;
        return true;
    }

    bool SkyCubemapGenerator::Generate(CommandList* cmdList,
                                       const std::vector<float>& equirectRgb,
                                       UINT srcWidth,
                                       UINT srcHeight,
                                       UINT faceSize,
                                       Resource& outCube)
    {
        if (!IsReady() || !cmdList || faceSize == 0 || srcWidth == 0 || srcHeight == 0 ||
            equirectRgb.size() < static_cast<size_t>(srcWidth) * static_cast<size_t>(srcHeight) * 3u) {
            return false;
        }

        UINT mipLevels = 1;
        for (UINT s = faceSize; s > 1; s >>= 1) {
            ++mipLevels;
        }

        if (!UploadEquirectTexture(cmdList, equirectRgb, srcWidth, srcHeight)) {
            DebugLog("SkyCubemapGenerator::Generate: equirect upload failed.\n");
            return false;
        }

        D3D12_RESOURCE_DESC cubeDesc = {};
        cubeDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        cubeDesc.Width = faceSize;
        cubeDesc.Height = faceSize;
        cubeDesc.DepthOrArraySize = 6;
        cubeDesc.MipLevels = static_cast<UINT16>(mipLevels);
        cubeDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        cubeDesc.SampleDesc.Count = 1;
        cubeDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        cubeDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        CD3DX12_HEAP_PROPERTIES defaultHeapProps(D3D12_HEAP_TYPE_DEFAULT);
        outCube.Reset();
        HRESULT hr = m_device->CreateCommittedResource(&defaultHeapProps,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &cubeDesc,
                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                        nullptr,
                                                        outCube);
        if (FAILED(hr)) {
            DebugLog("SkyCubemapGenerator::Generate: cube texture creation failed.\n");
            return false;
        }

        if (!EnsureDescriptors(faceSize, mipLevels)) {
            DebugLog("SkyCubemapGenerator::Generate: descriptor allocation failed.\n");
            return false;
        }

        ID3D12Device* nativeDevice = m_device->GetDevice();
        if (!nativeDevice) {
            return false;
        }

        // The descriptor slots above are cached/reused across calls, but the views
        // bound to them must be re-pointed at this call's freshly created outCube
        // (and equirect texture, which may have changed size) every time.
        D3D12_SHADER_RESOURCE_VIEW_DESC equirectSrvDesc = {};
        equirectSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        equirectSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        equirectSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        equirectSrvDesc.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_equirectTexture, &equirectSrvDesc, m_equirectSrvCpu);

        for (UINT m = 0; m < mipLevels; ++m) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uavDesc.Texture2DArray.MipSlice = m;
            uavDesc.Texture2DArray.FirstArraySlice = 0;
            uavDesc.Texture2DArray.ArraySize = 6;
            nativeDevice->CreateUnorderedAccessView(outCube.Get(), nullptr, &uavDesc, m_mipUavCpu[m]);
        }
        for (UINT m = 0; m + 1 < mipLevels; ++m) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.MostDetailedMip = m;
            srvDesc.Texture2DArray.MipLevels = 1;
            srvDesc.Texture2DArray.FirstArraySlice = 0;
            srvDesc.Texture2DArray.ArraySize = 6;
            m_device->CreateShaderResourceView(outCube, &srvDesc, m_mipSrvCpu[m]);
        }

        if (m_srvHeap) {
            DescriptorHeap* heaps[] = { m_srvHeap };
            cmdList->SetDescriptorHeaps(1, heaps);
        }

        cmdList->SetComputeRootSignature(m_fromEquirectRootSignature);
        cmdList->SetPipelineState(m_fromEquirectPipelineState);
        cmdList->SetComputeRootDescriptorTable(0, m_equirectSrvGpu);
        cmdList->SetComputeRootDescriptorTable(1, m_mipUavGpu[0]);
        const UINT mip0Constants[4] = { faceSize, 0, 0, 0 };
        cmdList->SetComputeRoot32BitConstants(2, 4, mip0Constants, 0);
        cmdList->Dispatch((faceSize + 7u) / 8u, (faceSize + 7u) / 8u, 6u);

        for (UINT m = 1; m < mipLevels; ++m) {
            const UINT srcSize = (std::max)(1u, faceSize >> (m - 1));
            const UINT dstSize = (std::max)(1u, faceSize >> m);

            // Subresource index for a Texture2DArray mip/face pair is
            // mipSlice + arraySlice * mipLevels (matches the ResourceUploadUtility
            // cubemap-with-mips convention noted in Skybox::UploadHdrSkyboxTexture).
            std::vector<D3D12_RESOURCE_BARRIER> toSrvBarriers;
            toSrvBarriers.reserve(7);
            toSrvBarriers.push_back(CD3DX12_RESOURCE_BARRIER::UAV(outCube.Get()));
            for (UINT face = 0; face < 6; ++face) {
                const UINT subresource = (m - 1) + face * mipLevels;
                toSrvBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(outCube.Get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    subresource));
            }
            cmdList->ResourceBarrier(static_cast<UINT>(toSrvBarriers.size()), toSrvBarriers.data());

            cmdList->SetComputeRootSignature(m_downsampleRootSignature);
            cmdList->SetPipelineState(m_downsamplePipelineState);
            cmdList->SetComputeRootDescriptorTable(0, m_mipSrvGpu[m - 1]);
            cmdList->SetComputeRootDescriptorTable(1, m_mipUavGpu[m]);
            const UINT downsampleConstants[4] = { dstSize, dstSize, srcSize, srcSize };
            cmdList->SetComputeRoot32BitConstants(2, 4, downsampleConstants, 0);
            cmdList->Dispatch((dstSize + 7u) / 8u, (dstSize + 7u) / 8u, 6u);
        }

        // Every mip except the last was left in NON_PIXEL_SHADER_RESOURCE by the
        // downsample loop above; the last mip is still UNORDERED_ACCESS (mip0's
        // initial dispatch target if mipLevels == 1, or the final downsample target
        // otherwise). Move everything to PIXEL_SHADER_RESOURCE for sampling.
        std::vector<D3D12_RESOURCE_BARRIER> finalBarriers;
        finalBarriers.reserve(static_cast<size_t>(mipLevels) * 6u);
        for (UINT face = 0; face < 6; ++face) {
            for (UINT m = 0; m < mipLevels; ++m) {
                const UINT subresource = m + face * mipLevels;
                const D3D12_RESOURCE_STATES before = (m + 1 == mipLevels)
                    ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                    : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                finalBarriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(outCube.Get(),
                    before, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, subresource));
            }
        }
        cmdList->ResourceBarrier(static_cast<UINT>(finalBarriers.size()), finalBarriers.data());

        return true;
    }
}
