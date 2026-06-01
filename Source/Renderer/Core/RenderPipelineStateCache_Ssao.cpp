// RenderPipelineStateCache_Ssao.cpp
// SSAO (Screen-Space Ambient Occlusion) + SSAO Blur pipeline creation.
// Both passes have their own root signatures and are independent from the PBR root signature.
#include "Renderer/Core/RenderPipelineStateCache.h"
#include "Renderer/Core/ShaderCompilationService.h"

#include <d3dcompiler.h>
#include <filesystem>
#include <string>
#include <windows.h>

#include "Foundation/Tools/DebugOutput.h"
#include "d3dx12.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace SasamiRenderer
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        static std::string FmtHr(HRESULT hr)
        {
            char buf[16] = {};
            std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(hr));
            return buf;
        }

        static void LogFail(const char* ctx, HRESULT hr)
        {
            std::string msg = ctx;
            msg += " failed. hr=";
            msg += FmtHr(hr);
            msg += "\n";
            DebugLog(msg.c_str());
        }

        static bool LoadShaderBlob(const wchar_t* relPath, const char* entry, const char* profile,
                                   ComPtr<ID3DBlob>& outBlob)
        {
            const auto srcPath      = ShaderCompilationService::ResolveShaderPath(relPath);
            const auto compiledPath = ShaderCompilationService::ResolveCompiledShaderPath(srcPath, entry, profile);
            outBlob.Reset();

            if (ShaderCompilationService::IsCompiledShaderUpToDate(compiledPath, srcPath)) {
                if (SUCCEEDED(D3DReadFileToBlob(compiledPath.c_str(), outBlob.GetAddressOf()))) {
                    ShaderCompilationService::LogShaderResolveMessage(srcPath, entry, profile,
                        "loaded precompiled shader", compiledPath);
                    return true;
                }
                ShaderCompilationService::LogShaderResolveMessage(srcPath, entry, profile,
                    "failed to read precompiled shader, falling back to runtime compile", compiledPath);
            } else {
                ShaderCompilationService::LogShaderResolveMessage(srcPath, entry, profile,
                    "precompiled shader missing or stale, runtime compiling", compiledPath);
            }

            if (!ShaderCompilationService::CompileShader(srcPath, entry, profile, outBlob)) {
                std::string msg = "RenderPipelineStateCache: shader compile failed: ";
                msg += srcPath.string();
                DebugLogDialog(msg.c_str(), L"Shader Initialize Error", MB_OK | MB_ICONERROR);
                return false;
            }

            std::error_code ec;
            std::filesystem::create_directories(compiledPath.parent_path(), ec);
            if (!ec)
                D3DWriteBlobToFile(outBlob.Get(), compiledPath.c_str(), TRUE);

            ShaderCompilationService::LogShaderResolveMessage(srcPath, entry, profile,
                "runtime compiled shader and updated cache", compiledPath);
            return true;
        }

    } // anonymous namespace

    bool RenderPipelineStateCache::InitializeSsaoPipelines(GraphicsDevice& device,
                                                            const std::string& vertexProfile,
                                                            const std::string& pixelProfile)
    {
        HRESULT hr = S_OK;

        auto createPso = [&](const char* label, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                              PipelineState& out) -> bool {
            hr = device.CreateGraphicsPipelineState(desc, out);
            if (FAILED(hr)) {
                std::string ctx = "RenderPipelineStateCache::InitializeSsaoPipelines: CreateGraphicsPipelineState(";
                ctx += label;
                ctx += ")";
                LogFail(ctx.c_str(), hr);
                return false;
            }
            return true;
        };

        // -------------------------------------------------------------------------
        // SSAO root signature
        // [0] t0 - depth SRV (PS only, descriptor table)
        // [1] t1 - normal SRV (PS only, descriptor table)
        // [2] b0 - SSAO constant buffer (all stages, root CBV)
        // Static sampler: point-clamp at s0 (PS only)
        // -------------------------------------------------------------------------
        D3D12_DESCRIPTOR_RANGE ssaoDepthRange{
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        D3D12_DESCRIPTOR_RANGE ssaoNormalRange{
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_ROOT_PARAMETER ssaoParams[3] = {};
        ssaoParams[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        ssaoParams[0].DescriptorTable.NumDescriptorRanges = 1;
        ssaoParams[0].DescriptorTable.pDescriptorRanges   = &ssaoDepthRange;
        ssaoParams[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        ssaoParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        ssaoParams[1].DescriptorTable.NumDescriptorRanges = 1;
        ssaoParams[1].DescriptorTable.pDescriptorRanges   = &ssaoNormalRange;
        ssaoParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        ssaoParams[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        ssaoParams[2].Descriptor.ShaderRegister = 0;
        ssaoParams[2].Descriptor.RegisterSpace  = 0;
        ssaoParams[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC ssaoSampler = {};
        ssaoSampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
        ssaoSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ssaoSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ssaoSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ssaoSampler.ShaderRegister   = 0;
        ssaoSampler.RegisterSpace    = 0;
        ssaoSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC ssaoRsDesc = {};
        ssaoRsDesc.NumParameters    = 3;
        ssaoRsDesc.pParameters      = ssaoParams;
        ssaoRsDesc.NumStaticSamplers = 1;
        ssaoRsDesc.pStaticSamplers  = &ssaoSampler;
        ssaoRsDesc.Flags            = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        {
            ComPtr<ID3DBlob> sig, sigErr;
            hr = D3D12SerializeRootSignature(&ssaoRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr);
            if (FAILED(hr)) {
                if (sigErr && sigErr->GetBufferPointer() && sigErr->GetBufferSize() > 0) {
                    DebugLog(static_cast<const char*>(sigErr->GetBufferPointer()));
                    DebugLog("\n");
                }
                LogFail("RenderPipelineStateCache: SSAO SerializeRootSignature", hr);
                return false;
            }
            hr = device.CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), m_ssaoRootSignature);
            if (FAILED(hr)) { LogFail("RenderPipelineStateCache: SSAO CreateRootSignature", hr); return false; }
        }

        ComPtr<ID3DBlob> ssaoVS, ssaoPS, ssaoBlurPS;
        if (!LoadShaderBlob(L"SSAO/SSAO_VS.hlsl",      "VSMain", vertexProfile.c_str(), ssaoVS))      return false;
        if (!LoadShaderBlob(L"SSAO/SSAO_PS.hlsl",      "PSMain", pixelProfile.c_str(),  ssaoPS))      return false;
        if (!LoadShaderBlob(L"SSAO/SSAO_Blur_PS.hlsl", "PSMain", pixelProfile.c_str(),  ssaoBlurPS))  return false;

        // SSAO PSO
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
            pso.pRootSignature = m_ssaoRootSignature.Get();
            pso.VS             = { ssaoVS->GetBufferPointer(),  ssaoVS->GetBufferSize()  };
            pso.PS             = { ssaoPS->GetBufferPointer(),  ssaoPS->GetBufferSize()  };
            pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso.BlendState     = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            pso.DepthStencilState.DepthEnable   = FALSE;
            pso.DepthStencilState.StencilEnable = FALSE;
            pso.InputLayout    = { nullptr, 0 };
            pso.SampleMask     = UINT_MAX;
            pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso.NumRenderTargets = 1;
            pso.RTVFormats[0]  = DXGI_FORMAT_R8G8B8A8_UNORM;
            pso.SampleDesc.Count = 1;
            if (!createPso("SSAO", pso, m_ssaoPipelineState)) return false;
        }

        // -------------------------------------------------------------------------
        // SSAO Blur root signature
        // [0] t0 - raw SSAO SRV (PS only)
        // [1] t1 - depth SRV   (PS only)
        // [2] t2 - normal SRV  (PS only)
        // [3] b0 - SSAO CB     (PS only, root CBV)
        // Static sampler: point-clamp at s0 (PS only)
        // -------------------------------------------------------------------------
        D3D12_DESCRIPTOR_RANGE blurSsaoRange  { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
        D3D12_DESCRIPTOR_RANGE blurDepthRange { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
        D3D12_DESCRIPTOR_RANGE blurNormRange  { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };

        D3D12_ROOT_PARAMETER blurParams[4] = {};
        blurParams[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        blurParams[0].DescriptorTable.NumDescriptorRanges = 1;
        blurParams[0].DescriptorTable.pDescriptorRanges   = &blurSsaoRange;
        blurParams[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        blurParams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        blurParams[1].DescriptorTable.NumDescriptorRanges = 1;
        blurParams[1].DescriptorTable.pDescriptorRanges   = &blurDepthRange;
        blurParams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        blurParams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        blurParams[2].DescriptorTable.NumDescriptorRanges = 1;
        blurParams[2].DescriptorTable.pDescriptorRanges   = &blurNormRange;
        blurParams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        blurParams[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        blurParams[3].Descriptor.ShaderRegister = 0;
        blurParams[3].Descriptor.RegisterSpace  = 0;
        blurParams[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC blurSampler = {};
        blurSampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
        blurSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        blurSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        blurSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        blurSampler.ShaderRegister   = 0;
        blurSampler.RegisterSpace    = 0;
        blurSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC blurRsDesc = {};
        blurRsDesc.NumParameters    = 4;
        blurRsDesc.pParameters      = blurParams;
        blurRsDesc.NumStaticSamplers = 1;
        blurRsDesc.pStaticSamplers  = &blurSampler;
        blurRsDesc.Flags            = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        {
            ComPtr<ID3DBlob> sig, sigErr;
            hr = D3D12SerializeRootSignature(&blurRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr);
            if (FAILED(hr)) {
                if (sigErr && sigErr->GetBufferPointer() && sigErr->GetBufferSize() > 0) {
                    DebugLog(static_cast<const char*>(sigErr->GetBufferPointer()));
                    DebugLog("\n");
                }
                LogFail("RenderPipelineStateCache: SSAO Blur SerializeRootSignature", hr);
                return false;
            }
            hr = device.CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), m_ssaoBlurRootSignature);
            if (FAILED(hr)) { LogFail("RenderPipelineStateCache: SSAO Blur CreateRootSignature", hr); return false; }
        }

        // SSAO Blur PSO
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
            pso.pRootSignature = m_ssaoBlurRootSignature.Get();
            pso.VS             = { ssaoVS->GetBufferPointer(),     ssaoVS->GetBufferSize()     };
            pso.PS             = { ssaoBlurPS->GetBufferPointer(), ssaoBlurPS->GetBufferSize() };
            pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
            pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso.BlendState     = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
            pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            pso.DepthStencilState.DepthEnable   = FALSE;
            pso.DepthStencilState.StencilEnable = FALSE;
            pso.InputLayout    = { nullptr, 0 };
            pso.SampleMask     = UINT_MAX;
            pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso.NumRenderTargets = 1;
            pso.RTVFormats[0]  = DXGI_FORMAT_R8G8B8A8_UNORM;
            pso.SampleDesc.Count = 1;
            if (!createPso("SSAOBlur", pso, m_ssaoBlurPipelineState)) return false;
        }

        return true;
    }

} // namespace SasamiRenderer
