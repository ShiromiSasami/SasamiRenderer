// RenderPipelineStateCacheSkinned.cpp
// GPU-skinned mesh pipelines: opaque, GBuffer, transparent (+OIT, +backface-distance)
// and shadow (D32/D16) variants. Split out of Initialize() purely to keep that
// function from growing without bound; behaviour is identical to the inline code
// it replaced.
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Resources/RenderPipelineStateCacheLog.h"
#include "Renderer/Resources/ShaderBlobCache.h"
#include "Renderer/Resources/ShaderCompilationService.h"

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

    bool RenderPipelineStateCache::InitializeSkinnedPipelines(GraphicsDevice& device, const SkinnedPipelineInputs& inputs)
    {
        auto createPipelineState = [&](const char* label,
                                       const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                                       PipelineState& outState) -> bool
        {
            HRESULT hr = device.CreateGraphicsPipelineState(desc, outState);
            if (FAILED(hr)) {
                std::string context = "RenderPipelineStateCache::Initialize: CreateGraphicsPipelineState(";
                context += label;
                context += ")";
                LogPipelineStateFailure(context.c_str(), hr, device.GetDevice());
                return false;
            }

            return true;
        };

        // -------------------------------------------------------------------------
        // Skinned mesh root signature: same first 16 params as m_rootSignature, then
        // [16] b3 bone CB (VS-only) and [17] the material normal map table copied from
        // m_rootSignature[16]. The bone CB keeps index 16 so the skinned draw path's
        // existing root-parameter indices stay valid; the normal table is appended after
        // it because OpaqueGBuffer_PS (shared with the skinned GBuffer PSO) declares t17.
        // -------------------------------------------------------------------------
        D3D12_ROOT_PARAMETER skinnedRootParams[18] = {};
        for (int i = 0; i < 16; ++i) skinnedRootParams[i] = inputs.rootParams[i];
        skinnedRootParams[17] = inputs.rootParams[16];
        skinnedRootParams[16].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        skinnedRootParams[16].Descriptor.ShaderRegister = 3; // b3
        skinnedRootParams[16].Descriptor.RegisterSpace  = 0;
        skinnedRootParams[16].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC skinnedRootSigDesc = {};
        skinnedRootSigDesc.NumParameters    = _countof(skinnedRootParams);
        skinnedRootSigDesc.pParameters      = skinnedRootParams;
        skinnedRootSigDesc.NumStaticSamplers = _countof(inputs.staticSamplers);
        skinnedRootSigDesc.pStaticSamplers  = inputs.staticSamplers;
        skinnedRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> skinnedSig, skinnedSigErr;
        HRESULT hr = D3D12SerializeRootSignature(&skinnedRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &skinnedSig, &skinnedSigErr);
        if (FAILED(hr)) {
            if (skinnedSigErr) DebugLog(static_cast<const char*>(skinnedSigErr->GetBufferPointer()));
            LogFail("RenderPipelineStateCache: SkinnedMesh SerializeRootSignature", hr);
            return false;
        }
        hr = device.CreateRootSignature(0, skinnedSig->GetBufferPointer(), skinnedSig->GetBufferSize(), m_skinnedRootSignature);
        if (FAILED(hr)) {
            LogFail("RenderPipelineStateCache: SkinnedMesh CreateRootSignature", hr);
            return false;
        }

        // Load/compile SkinnedMesh_VS.hlsl
        ComPtr<ID3DBlob> skinnedVS;
        if (!LoadShaderBlob(m_shaderBlobCache, L"Raster/Geometry/SkinnedMesh/SkinnedMesh_VS.hlsl", "VSMain",
                            inputs.vertexProfile.c_str(), skinnedVS)) {
            return false;
        }

        // Skinned vertex input layout (68 bytes): POSITION/NORMAL/COLOR/TEXCOORD/JOINTS_0/WEIGHTS_0
        D3D12_INPUT_ELEMENT_DESC skinnedInputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "JOINTS_",  0, DXGI_FORMAT_R8G8B8A8_UINT,      0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "WEIGHTS_", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 52, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        // Opaque skinned PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkinned = inputs.psoDesc;
        psoSkinned.InputLayout    = { skinnedInputLayout, _countof(skinnedInputLayout) };
        psoSkinned.pRootSignature = m_skinnedRootSignature.Get();
        psoSkinned.VS             = { skinnedVS->GetBufferPointer(), skinnedVS->GetBufferSize() };
        if (!createPipelineState("SkinnedOpaque", psoSkinned, m_skinnedPipelineState)) { return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkinnedGBuffer = inputs.psoGBuffer;
        psoSkinnedGBuffer.InputLayout = { skinnedInputLayout, _countof(skinnedInputLayout) };
        psoSkinnedGBuffer.pRootSignature = m_skinnedRootSignature.Get();
        psoSkinnedGBuffer.VS = { skinnedVS->GetBufferPointer(), skinnedVS->GetBufferSize() };
        if (!createPipelineState("SkinnedOpaqueGBuffer", psoSkinnedGBuffer, m_skinnedGBufferPipelineState)) { return false; }

        // Transparent skinned PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkinnedTransparent = psoSkinned;
        psoSkinnedTransparent.BlendState = inputs.transparentBlend;
        psoSkinnedTransparent.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoSkinnedTransparent.NumRenderTargets = 1;
        for (UINT rtIndex = 1; rtIndex < 5; ++rtIndex)
            psoSkinnedTransparent.RTVFormats[rtIndex] = DXGI_FORMAT_UNKNOWN;
        if (!createPipelineState("SkinnedTransparent", psoSkinnedTransparent, m_skinnedTransparentPipelineState)) { return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkinnedTransparentOit = psoSkinnedTransparent;
        psoSkinnedTransparentOit.PS = { inputs.transparentOitPS->GetBufferPointer(), inputs.transparentOitPS->GetBufferSize() };
        psoSkinnedTransparentOit.BlendState = inputs.oitBlend;
        psoSkinnedTransparentOit.NumRenderTargets = 2;
        psoSkinnedTransparentOit.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoSkinnedTransparentOit.RTVFormats[1] = DXGI_FORMAT_R16_FLOAT;
        for (UINT rtIndex = 2; rtIndex < 5; ++rtIndex) {
            psoSkinnedTransparentOit.RTVFormats[rtIndex] = DXGI_FORMAT_UNKNOWN;
        }
        if (!createPipelineState("SkinnedTransparentOIT", psoSkinnedTransparentOit, m_skinnedTransparentOitPipelineState)) { return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkinnedTransparentBackfaceDistance = psoSkinnedTransparent;
        psoSkinnedTransparentBackfaceDistance.PS = { inputs.transparentBackfaceDistancePS->GetBufferPointer(), inputs.transparentBackfaceDistancePS->GetBufferSize() };
        psoSkinnedTransparentBackfaceDistance.RasterizerState = inputs.backfaceDistanceRast;
        psoSkinnedTransparentBackfaceDistance.BlendState = inputs.blendDesc;
        psoSkinnedTransparentBackfaceDistance.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
        if (!createPipelineState("SkinnedTransparentBackfaceDistance", psoSkinnedTransparentBackfaceDistance, m_skinnedTransparentBackfaceDistancePipelineState)) { return false; }

        // Shadow skinned PSO (depth-only, same shadow rasterizer bias)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkinnedShadow = {};
        psoSkinnedShadow.InputLayout    = { skinnedInputLayout, _countof(skinnedInputLayout) };
        psoSkinnedShadow.pRootSignature = m_skinnedRootSignature.Get();
        psoSkinnedShadow.VS             = { skinnedVS->GetBufferPointer(), skinnedVS->GetBufferSize() };
        psoSkinnedShadow.RasterizerState = inputs.shadowRast;
        psoSkinnedShadow.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoSkinnedShadow.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoSkinnedShadow.DepthStencilState.DepthEnable = TRUE;
        psoSkinnedShadow.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        ApplyCommonPsoDefaults(psoSkinnedShadow);
        psoSkinnedShadow.NumRenderTargets = 0;
        psoSkinnedShadow.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        if (!createPipelineState("SkinnedShadow", psoSkinnedShadow, m_skinnedShadowPipelineState)) { return false; }

        // D16_UNORM DSV版(スポット/ポイントライトの512x512シャドウマップ用)。
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkinnedShadowD16 = psoSkinnedShadow;
        psoSkinnedShadowD16.DSVFormat = DXGI_FORMAT_D16_UNORM;
        psoSkinnedShadowD16.RasterizerState = inputs.pointSpotShadowRast;
        if (!createPipelineState("SkinnedShadowD16", psoSkinnedShadowD16, m_skinnedShadowD16PipelineState)) { return false; }

        return true;
    }

} // namespace SasamiRenderer
