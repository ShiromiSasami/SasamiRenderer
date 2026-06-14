#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Resources/ShaderCompilationService.h"

#include "Foundation/Tools/DebugOutput.h"

#include <d3dcompiler.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <wrl/client.h>

namespace SasamiRenderer
{
    namespace
    {
        using Microsoft::WRL::ComPtr;

        void LogFail(const char* context, HRESULT hr)
        {
            char text[128] = {};
            std::snprintf(text, sizeof(text), "%s failed. hr=0x%08X\n", context, static_cast<unsigned int>(hr));
            DebugLog(text);
        }

        bool LoadShaderBlob(const wchar_t* relativePath,
                            const char* entry,
                            const char* target,
                            ComPtr<ID3DBlob>& outBlob)
        {
            const std::filesystem::path sourcePath = ShaderCompilationService::ResolveShaderPath(relativePath);
            const std::filesystem::path compiledPath =
                ShaderCompilationService::ResolveCompiledShaderPath(sourcePath, entry, target);
            outBlob.Reset();
            if (ShaderCompilationService::IsCompiledShaderUpToDate(compiledPath, sourcePath) &&
                SUCCEEDED(D3DReadFileToBlob(compiledPath.c_str(), outBlob.GetAddressOf()))) {
                ShaderCompilationService::LogShaderResolveMessage(sourcePath, entry, target,
                                                                  "loaded precompiled shader", compiledPath);
                return true;
            }

            ShaderCompilationService::LogShaderResolveMessage(sourcePath, entry, target,
                                                              "precompiled shader missing or stale, runtime compiling",
                                                              compiledPath);
            if (!ShaderCompilationService::CompileShader(sourcePath, entry, target, outBlob)) {
                return false;
            }

            std::error_code ec;
            std::filesystem::create_directories(compiledPath.parent_path(), ec);
            if (ec) {
                ShaderCompilationService::LogShaderResolveMessage(sourcePath, entry, target,
                                                                  "cache directory creation failed; keeping runtime blob only",
                                                                  compiledPath);
                return true;
            }

            const HRESULT writeHr = D3DWriteBlobToFile(outBlob.Get(), compiledPath.c_str(), TRUE);
            if (FAILED(writeHr)) {
                ShaderCompilationService::LogShaderResolveMessage(sourcePath, entry, target,
                                                                  "compiled shader could not be written",
                                                                  compiledPath);
            } else {
                ShaderCompilationService::LogShaderResolveMessage(sourcePath, entry, target,
                                                                  "runtime compiled shader and updated cache",
                                                                  compiledPath);
            }
            return true;
        }
    }

    bool RenderPipelineStateCache::InitializeScreenSpaceReflectionPipeline(GraphicsDevice& device,
                                                                          const std::string& computeProfile)
    {
        HRESULT hr = S_OK;

        D3D12_DESCRIPTOR_RANGE depthRange{
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        D3D12_DESCRIPTOR_RANGE normalRange{
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        D3D12_DESCRIPTOR_RANGE materialRange{
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        D3D12_DESCRIPTOR_RANGE sceneColorRange{
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        D3D12_DESCRIPTOR_RANGE reflectionUavRange{
            D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_ROOT_PARAMETER params[6] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &depthRange;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &normalRange;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &materialRange;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable.NumDescriptorRanges = 1;
        params[3].DescriptorTable.pDescriptorRanges = &sceneColorRange;
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[4].DescriptorTable.NumDescriptorRanges = 1;
        params[4].DescriptorTable.pDescriptorRanges = &reflectionUavRange;
        params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[5].Descriptor.ShaderRegister = 0;
        params[5].Descriptor.RegisterSpace = 0;
        params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
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

        ComPtr<ID3DBlob> sig;
        ComPtr<ID3DBlob> err;
        hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr)) {
            if (err && err->GetBufferPointer()) {
                DebugLog(static_cast<const char*>(err->GetBufferPointer()));
                DebugLog("\n");
            }
            LogFail("RenderPipelineStateCache: SSR SerializeRootSignature", hr);
            return false;
        }

        hr = device.CreateRootSignature(0,
                                        sig->GetBufferPointer(),
                                        sig->GetBufferSize(),
                                        m_screenSpaceReflectionRootSignature);
        if (FAILED(hr)) {
            LogFail("RenderPipelineStateCache: SSR CreateRootSignature", hr);
            return false;
        }

        ComPtr<ID3DBlob> ssrCS;
        if (!LoadShaderBlob(L"Effects/Reflections/SSR/ScreenSpaceReflection_CS.hlsl",
                            "CSMain",
                            computeProfile.c_str(),
                            ssrCS)) {
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = m_screenSpaceReflectionRootSignature.Get();
        pso.CS = { ssrCS->GetBufferPointer(), ssrCS->GetBufferSize() };
        hr = device.CreateComputePipelineState(pso, m_screenSpaceReflectionPipelineState);
        if (FAILED(hr)) {
            LogFail("RenderPipelineStateCache: SSR CreateComputePipelineState", hr);
            return false;
        }
        return true;
    }
}
