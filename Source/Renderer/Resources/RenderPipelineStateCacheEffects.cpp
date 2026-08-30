// RenderPipelineStateCacheEffects.cpp
// Fullscreen-triangle effect pipelines: Ray March, Volumetric Cloud.
// Each pipeline has its own root signature (single CBV at b0) and is fully
// independent from the main PBR root signature.
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Resources/RenderPipelineStateCacheLog.h"
#include "Renderer/Resources/ShaderBlobCache.h"
#include "Renderer/Resources/ShaderCompilationService.h"

#include <d3dcompiler.h>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

#include "Foundation/Tools/DebugOutput.h"
#include "d3dx12.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace SasamiRenderer
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        static bool LoadShaderBlob(ShaderBlobCache& cache, const wchar_t* relPath, const char* entry, const char* profile,
                                   ComPtr<ID3DBlob>& outBlob)
        {
            const auto srcPath      = ShaderCompilationService::ResolveShaderPath(relPath);
            const auto compiledPath = ShaderCompilationService::ResolveCompiledShaderPath(srcPath, entry, profile);
            outBlob.Reset();

            outBlob = cache.GetOrResolve(compiledPath.wstring(), [&]() -> ComPtr<ID3DBlob>
            {
                ComPtr<ID3DBlob> blob;
                if (ShaderCompilationService::IsCompiledShaderUpToDate(compiledPath, srcPath)) {
                    if (SUCCEEDED(D3DReadFileToBlob(compiledPath.c_str(), blob.GetAddressOf()))) {
                        ShaderCompilationService::LogShaderResolveMessage(srcPath, entry, profile,
                            "loaded precompiled shader", compiledPath);
                        return blob;
                    }
                    ShaderCompilationService::LogShaderResolveMessage(srcPath, entry, profile,
                        "failed to read precompiled shader, falling back to runtime compile", compiledPath);
                } else {
                    ShaderCompilationService::LogShaderResolveMessage(srcPath, entry, profile,
                        "precompiled shader missing or stale, runtime compiling", compiledPath);
                }

                if (!ShaderCompilationService::CompileShader(srcPath, entry, profile, blob)) {
                    std::string msg = "RenderPipelineStateCache: shader compile failed: ";
                    msg += srcPath.string();
                    DebugLogDialog(msg.c_str(), L"Shader Initialize Error", MB_OK | MB_ICONERROR);
                    return ComPtr<ID3DBlob>();
                }

                std::error_code ec;
                std::filesystem::create_directories(compiledPath.parent_path(), ec);
                if (!ec)
                    D3DWriteBlobToFile(blob.Get(), compiledPath.c_str(), TRUE);

                ShaderCompilationService::LogShaderResolveMessage(srcPath, entry, profile,
                    "runtime compiled shader and updated cache", compiledPath);
                return blob;
            });

            return outBlob.Get() != nullptr;
        }

    } // anonymous namespace

    bool RenderPipelineStateCache::InitializeEffectPipelines(GraphicsDevice& device,
                                                              const std::string& vertexProfile,
                                                              const std::string& pixelProfile)
    {
        HRESULT hr = S_OK;

        auto createPso = [&](const char* label, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                              PipelineState& out) -> bool {
            hr = device.CreateGraphicsPipelineState(desc, out);
            if (FAILED(hr)) {
                std::string ctx = "RenderPipelineStateCache::InitializeEffectPipelines: CreateGraphicsPipelineState(";
                ctx += label;
                ctx += ")";
                LogFail(ctx.c_str(), hr);
                return false;
            }
            return true;
        };

        auto rast = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        rast.CullMode = D3D12_CULL_MODE_NONE;

        auto blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        blendDesc.RenderTarget[0].BlendEnable          = FALSE;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        // -------------------------------------------------------------------------
        // Ray March  Esingle root CBV at b0 (pixel shader only)
        // -------------------------------------------------------------------------
        {
            D3D12_ROOT_PARAMETER param = {};
            param.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            param.Descriptor.ShaderRegister = 0;
            param.Descriptor.RegisterSpace  = 0;
            param.ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
            rsDesc.NumParameters = 1;
            rsDesc.pParameters   = &param;
            rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;

            ComPtr<ID3DBlob> sig, err;
            hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
            if (FAILED(hr)) { LogFail("RenderPipelineStateCache: RayMarch SerializeRootSignature", hr); return false; }
            hr = device.CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), m_rayMarchRootSignature);
            if (FAILED(hr)) { LogFail("RenderPipelineStateCache: RayMarch CreateRootSignature", hr); return false; }
        }

        ComPtr<ID3DBlob> rayMarchVS, rayMarchPS;
        if (!LoadShaderBlob(m_shaderBlobCache, L"Effects/RayMarch/RayMarch_VS.hlsl", "VSMain", vertexProfile.c_str(), rayMarchVS)) return false;
        if (!LoadShaderBlob(m_shaderBlobCache, L"Effects/RayMarch/RayMarch_PS.hlsl", "PSMain", pixelProfile.c_str(),  rayMarchPS)) return false;

        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
            pso.InputLayout           = { nullptr, 0 };
            pso.pRootSignature        = m_rayMarchRootSignature.Get();
            pso.VS                    = { rayMarchVS->GetBufferPointer(), rayMarchVS->GetBufferSize() };
            pso.PS                    = { rayMarchPS->GetBufferPointer(), rayMarchPS->GetBufferSize() };
            pso.RasterizerState       = rast;
            pso.BlendState            = blendDesc;
            pso.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            pso.DepthStencilState.DepthEnable    = FALSE;
            pso.DepthStencilState.StencilEnable  = FALSE;
            ApplyCommonPsoDefaults(pso);
            pso.NumRenderTargets      = 1;
            pso.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
            pso.DSVFormat             = DXGI_FORMAT_UNKNOWN;
            if (!createPso("RayMarch", pso, m_rayMarchPipelineState)) return false;
        }

        // -------------------------------------------------------------------------
        // Volumetric Cloud  Esingle root CBV at b0 (VS + PS visible)
        // -------------------------------------------------------------------------
        {
            D3D12_ROOT_PARAMETER param = {};
            param.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            param.Descriptor.ShaderRegister = 0;
            param.Descriptor.RegisterSpace  = 0;
            param.ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
            rsDesc.NumParameters = 1;
            rsDesc.pParameters   = &param;
            rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

            ComPtr<ID3DBlob> sig, err;
            hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
            if (FAILED(hr)) { LogFail("RenderPipelineStateCache: VolumetricCloud SerializeRootSignature", hr); return false; }
            hr = device.CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                            m_volumetricCloudRootSignature);
            if (FAILED(hr)) { LogFail("RenderPipelineStateCache: VolumetricCloud CreateRootSignature", hr); return false; }
        }

        ComPtr<ID3DBlob> cloudVS, cloudPS;
        if (!LoadShaderBlob(m_shaderBlobCache, L"Effects/Volumetric/Cloud/VolumetricCloud_VS.hlsl", "VSMain", vertexProfile.c_str(), cloudVS)) return false;
        if (!LoadShaderBlob(m_shaderBlobCache, L"Effects/Volumetric/Cloud/VolumetricCloud_PS.hlsl", "PSMain", pixelProfile.c_str(),  cloudPS)) return false;

        {
            D3D12_BLEND_DESC cloudBlend = {};
            cloudBlend.AlphaToCoverageEnable  = FALSE;
            cloudBlend.IndependentBlendEnable = FALSE;
            auto& rt = cloudBlend.RenderTarget[0];
            rt.BlendEnable           = TRUE;
            rt.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOp               = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha         = D3D12_BLEND_ONE;
            rt.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

            auto cloudDS = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
            cloudDS.DepthEnable    = TRUE;
            cloudDS.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            cloudDS.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            cloudDS.StencilEnable  = FALSE;

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
            pso.InputLayout          = { nullptr, 0 };
            pso.pRootSignature       = m_volumetricCloudRootSignature.Get();
            pso.VS                   = { cloudVS->GetBufferPointer(), cloudVS->GetBufferSize() };
            pso.PS                   = { cloudPS->GetBufferPointer(), cloudPS->GetBufferSize() };
            pso.RasterizerState      = rast;
            pso.BlendState           = cloudBlend;
            pso.DepthStencilState    = cloudDS;
            ApplyCommonPsoDefaults(pso);
            pso.NumRenderTargets     = 1;
            pso.RTVFormats[0]        = DXGI_FORMAT_R16G16B16A16_FLOAT;
            pso.DSVFormat            = DXGI_FORMAT_D32_FLOAT;
            if (!createPso("VolumetricCloud", pso, m_volumetricCloudPipelineState)) return false;
        }

        return true;
    }

} // namespace SasamiRenderer
