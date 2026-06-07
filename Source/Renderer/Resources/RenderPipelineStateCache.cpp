#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Resources/ShaderCompilationService.h"

#include <array>
#include <d3dcompiler.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include <windows.h>

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Tools/ScopedPerfTimer.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace SasamiRenderer
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        struct ShaderSpec
        {
            const wchar_t* sourceRelativePath = nullptr;
            const char* entry = nullptr;
            const char* target = nullptr;
            ComPtr<ID3DBlob>* output = nullptr;
        };

        static std::string FormatHResult(HRESULT hr)
        {
            char text[16] = {};
            std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned int>(hr));
            return text;
        }

        static void LogFailureMessage(const char* context, HRESULT hr)
        {
            std::string message = context;
            message += " failed. hr=";
            message += FormatHResult(hr);
            message += "\n";
            DebugLog(message.c_str());
        }
    }

    bool RenderPipelineStateCache::Initialize(GraphicsDevice& device)
    {
        ScopedPerfTimer perfTimer("RenderPipelineStateCache::Initialize");
        const std::string configuredShaderModel = ShaderCompilationService::GetConfiguredShaderModel();
        const std::string shaderModel = ShaderCompilationService::ResolveEffectiveShaderModel(device.GetDevice(), configuredShaderModel);

        // Root signature:
        // [0]=material SRV (t0), [1]=shadow SRV (t1), [2]=CBV (b0), [3]=CBV (b1),
        // [4]=light SRVs (t2-t3), [5]=IBL SRVs (t4-t6), [6]=AO SRV (t7), [7]=reflection SRV (t8),
        // [8]=SSAO SRV (t9), [9]=CBV (b2 GI probe grid), [10]=inline SRV (t10 probe SH data)
        D3D12_DESCRIPTOR_RANGE descRangeMaterial{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 0,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_DESCRIPTOR_RANGE descRangeShadow{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 1,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_DESCRIPTOR_RANGE descRangeLights{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 2,
            .BaseShaderRegister = 2,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_DESCRIPTOR_RANGE descRangeIbl{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 3,
            .BaseShaderRegister = 4,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_DESCRIPTOR_RANGE descRangeOcclusion{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 7,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_DESCRIPTOR_RANGE descRangeReflection{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 8,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_DESCRIPTOR_RANGE descRangeSceneDepth{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 11,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };

        D3D12_ROOT_PARAMETER rootParams[15] = {};
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[0].DescriptorTable.pDescriptorRanges = &descRangeMaterial;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[1].DescriptorTable.pDescriptorRanges = &descRangeShadow;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[2].Descriptor.ShaderRegister = 0; // b0
        rootParams[2].Descriptor.RegisterSpace = 0;
        // Make camera CB visible to all stages (VS/HS/DS/GS)
        rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[3].Descriptor.ShaderRegister = 1; // b1 (used in VS+PS)
        rootParams[3].Descriptor.RegisterSpace = 0;
        rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[4].DescriptorTable.pDescriptorRanges = &descRangeLights;
        rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[5].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[5].DescriptorTable.pDescriptorRanges = &descRangeIbl;
        rootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[6].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[6].DescriptorTable.pDescriptorRanges = &descRangeOcclusion;
        rootParams[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        rootParams[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[7].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[7].DescriptorTable.pDescriptorRanges = &descRangeReflection;
        rootParams[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // [8] t9 - Screen-space AO (SSAO output), PS only
        D3D12_DESCRIPTOR_RANGE descRangeScreenSpaceAO{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 9,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        rootParams[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[8].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[8].DescriptorTable.pDescriptorRanges = &descRangeScreenSpaceAO;
        rootParams[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // [9] b2 - GI probe grid CBV (inline root descriptor, PS only)
        rootParams[9].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[9].Descriptor.ShaderRegister = 2; // b2
        rootParams[9].Descriptor.RegisterSpace  = 0;
        rootParams[9].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        // [10] t10 - GI probe SH data (inline root SRV, PS only)
        rootParams[10].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[10].Descriptor.ShaderRegister = 10; // t10
        rootParams[10].Descriptor.RegisterSpace  = 0;
        rootParams[10].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        // [11] t11 - Scene depth for screen-space contact shadows
        rootParams[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[11].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[11].DescriptorTable.pDescriptorRanges = &descRangeSceneDepth;
        rootParams[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // [12] t12 - Spot light shadow map SRV, PS only
        D3D12_DESCRIPTOR_RANGE descRangeSpotShadow{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 12,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        rootParams[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[12].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[12].DescriptorTable.pDescriptorRanges = &descRangeSpotShadow;
        rootParams[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // [13] t13 - VSM shadow map array (R32G32_FLOAT Texture2DArray), PS only
        D3D12_DESCRIPTOR_RANGE descRangeVsm{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 13,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        rootParams[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[13].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[13].DescriptorTable.pDescriptorRanges = &descRangeVsm;
        rootParams[13].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // [14] t14 - transparent backface camera distance, PS only
        D3D12_DESCRIPTOR_RANGE descRangeTransparentBackfaceDistance{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 14,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        rootParams[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[14].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[14].DescriptorTable.pDescriptorRanges = &descRangeTransparentBackfaceDistance;
        rootParams[14].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.ShaderRegister = 0;
        samplerDesc.RegisterSpace = 0;
        samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.NumParameters = 15;
        rootSigDesc.pParameters = rootParams;
        rootSigDesc.NumStaticSamplers = 1;
        rootSigDesc.pStaticSamplers = &samplerDesc;
        rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
        if (FAILED(hr)) {
            if (error && error->GetBufferPointer() && error->GetBufferSize() > 0) {
                DebugLog(static_cast<const char*>(error->GetBufferPointer()));
                DebugLog("\n");
            }
            LogFailureMessage("RenderPipelineStateCache::Initialize: D3D12SerializeRootSignature", hr);
            return false;
        }

        hr = device.CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), m_rootSignature);
        if (FAILED(hr)) {
            LogFailureMessage("RenderPipelineStateCache::Initialize: CreateRootSignature", hr);
            return false;
        }

        // Resolve shader blobs from precompiled CSO first, then compile only stale/missing ones.
        ComPtr<ID3DBlob> vertexShader;
        ComPtr<ID3DBlob> pixelShader;
        ComPtr<ID3DBlob> basicVertexShader;
        ComPtr<ID3DBlob> basicPixelShader;
        ComPtr<ID3DBlob> skyboxVS;
        ComPtr<ID3DBlob> skyboxHdrPS;
        ComPtr<ID3DBlob> skyboxLdrPS;
        ComPtr<ID3DBlob> tessVS, tessHS, tessDS, tessGS, tessDebugDS;
        ComPtr<ID3DBlob> tessDebugPS;
        ComPtr<ID3DBlob> meshletDebugPS;
        ComPtr<ID3DBlob> ssaoVS;
        ComPtr<ID3DBlob> ssaoPS;
        ComPtr<ID3DBlob> ssaoBlurPS;
        ComPtr<ID3DBlob> proceduralSkyPS;
        ComPtr<ID3DBlob> rayMarchVS;
        ComPtr<ID3DBlob> rayMarchPS;
        ComPtr<ID3DBlob> volumetricCloudVS;
        ComPtr<ID3DBlob> volumetricCloudPS;
        ComPtr<ID3DBlob> swrtReflectionCompositePS;
        ComPtr<ID3DBlob> toneMapPS;
        ComPtr<ID3DBlob> transparentBackfaceDistancePS;
        ComPtr<ID3DBlob> transparentOitPS;
        ComPtr<ID3DBlob> transparentOitCompositePS;
        ComPtr<ID3DBlob> shadowVsmPS;
        ComPtr<ID3DBlob> shadowVsmBlurHCS;
        ComPtr<ID3DBlob> shadowVsmBlurVCS;
        auto loadOrCompileShader = [&](const ShaderSpec& spec) -> bool
        {
            const std::filesystem::path sourcePath = ShaderCompilationService::ResolveShaderPath(spec.sourceRelativePath);
            const std::filesystem::path compiledPath = ShaderCompilationService::ResolveCompiledShaderPath(sourcePath, spec.entry, spec.target);
            spec.output->Reset();

            if (ShaderCompilationService::IsCompiledShaderUpToDate(compiledPath, sourcePath)) {
                hr = D3DReadFileToBlob(compiledPath.c_str(), spec.output->GetAddressOf());
                if (SUCCEEDED(hr)) {
                    ShaderCompilationService::LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "loaded precompiled shader", compiledPath);
                    return true;
                }

                ShaderCompilationService::LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "failed to read precompiled shader, falling back to runtime compile", compiledPath);
            } else {
                ShaderCompilationService::LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "precompiled shader missing or stale, runtime compiling", compiledPath);
            }

            ScopedPerfTimer::Timestamp compileStart = ScopedPerfTimer::Now();
            const bool compileSucceeded = ShaderCompilationService::CompileShader(sourcePath, spec.entry, spec.target, *spec.output);
            const ScopedPerfTimer::Timestamp compileEnd = ScopedPerfTimer::Now();

            std::string perfLabel = "RenderPipelineStateCache::CompileShader ";
            perfLabel += sourcePath.filename().string();
            perfLabel += " (";
            perfLabel += spec.entry;
            perfLabel += "/";
            perfLabel += spec.target;
            perfLabel += ")";
            ScopedPerfTimer::LogMilliseconds(perfLabel.c_str(),
                           ScopedPerfTimer::ElapsedMilliseconds(compileStart, compileEnd));

            if (!compileSucceeded) {
                std::string msg = "RenderPipelineStateCache::Initialize: shader compile failed: ";
                msg += sourcePath.string();
                msg += "\n";
                DebugLogDialog(msg.c_str(), L"Shader Initialize Error", MB_OK | MB_ICONERROR);
                return false;
            }

            std::error_code ec;
            std::filesystem::create_directories(compiledPath.parent_path(), ec);
            if (!ec) {
                const HRESULT writeHr = D3DWriteBlobToFile(spec.output->Get(), compiledPath.c_str(), TRUE);
                if (FAILED(writeHr)) {
                    ShaderCompilationService::LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "compiled shader could not be written", compiledPath);
                } else {
                    ShaderCompilationService::LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "runtime compiled shader and updated cache", compiledPath);
                }
            } else {
                ShaderCompilationService::LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "cache directory creation failed; keeping runtime blob only", compiledPath);
            }

            return true;
        };

        // Skybox PS split:
        // - Skybox_PS.hlsl      : LDR cubemap path (direct sample)
        // - Skybox_HDR_PS.hlsl  : HDR cubemap path (tone map + gamma)
        const std::string vertexProfile = "vs_" + shaderModel;
        const std::string pixelProfile = "ps_" + shaderModel;
        const std::string hullProfile = "hs_" + shaderModel;
        const std::string domainProfile = "ds_" + shaderModel;
        const std::string geometryProfile = "gs_" + shaderModel;
        const std::string computeProfile = "cs_" + shaderModel;
        const std::array<ShaderSpec, 30> shaderSpecs{ {
            { L"PBR/CookTorranceGGX_VS.hlsl", "VSMain", vertexProfile.c_str(), &vertexShader },
            { L"PBR/CookTorranceGGX_PS.hlsl", "PSMain", pixelProfile.c_str(), &pixelShader },
            { L"Opaque/Opaque_VS.hlsl", "VSMain", vertexProfile.c_str(), &basicVertexShader },
            { L"Opaque/Opaque_PS.hlsl", "PSMain", pixelProfile.c_str(), &basicPixelShader },
            { L"Skybox/Skybox_VS.hlsl", "VSMain", vertexProfile.c_str(), &skyboxVS },
            { L"Skybox/Skybox_HDR_PS.hlsl", "PSMain", pixelProfile.c_str(), &skyboxHdrPS },
            { L"Skybox/Skybox_PS.hlsl", "PSMain", pixelProfile.c_str(), &skyboxLdrPS },
            { L"Tessellation/Tessellation_VS.hlsl", "VSMain", vertexProfile.c_str(), &tessVS },
            { L"Tessellation/Tessellation_HS.hlsl", "HSMain", hullProfile.c_str(), &tessHS },
            { L"Tessellation/Tessellation_DS.hlsl", "DSMain", domainProfile.c_str(), &tessDS },
            { L"Tessellation/Tessellation_GS.hlsl", "GSMain", geometryProfile.c_str(), &tessGS },
            { L"Tessellation/Tessellation_Debug_DS.hlsl", "DSMain", domainProfile.c_str(), &tessDebugDS },
            { L"Tessellation/Tessellation_Debug_PS.hlsl", "PSMain", pixelProfile.c_str(), &tessDebugPS },
            { L"Debug/MeshletDebug_PS.hlsl",        "PSMain", pixelProfile.c_str(), &meshletDebugPS },
            { L"SSAO/SSAO_VS.hlsl", "VSMain", vertexProfile.c_str(), &ssaoVS },
            { L"SSAO/SSAO_PS.hlsl", "PSMain", pixelProfile.c_str(), &ssaoPS },
            { L"SSAO/SSAO_Blur_PS.hlsl", "PSMain", pixelProfile.c_str(), &ssaoBlurPS },
            { L"ProceduralSky/ProceduralSky_PS.hlsl", "PSMain", pixelProfile.c_str(), &proceduralSkyPS },
            { L"VolumetricCloud/VolumetricCloud_VS.hlsl", "VSMain", vertexProfile.c_str(), &volumetricCloudVS },
            { L"VolumetricCloud/VolumetricCloud_PS.hlsl", "PSMain", pixelProfile.c_str(), &volumetricCloudPS },
            { L"SWRT/SWRT_ReflectionComposite_PS.hlsl", "PSMain", pixelProfile.c_str(), &swrtReflectionCompositePS },
            { L"PostProcess/ToneMap_PS.hlsl", "PSMain", pixelProfile.c_str(), &toneMapPS },
            { L"RayMarch/RayMarch_VS.hlsl", "VSMain", vertexProfile.c_str(), &rayMarchVS },
            { L"RayMarch/RayMarch_PS.hlsl", "PSMain", pixelProfile.c_str(), &rayMarchPS },
            { L"Transparent/TransparentBackfaceDistance_PS.hlsl", "PSMain", pixelProfile.c_str(), &transparentBackfaceDistancePS },
            { L"Transparent/TransparentOIT_PS.hlsl", "PSMain", pixelProfile.c_str(), &transparentOitPS },
            { L"Transparent/TransparentOITComposite_PS.hlsl", "PSMain", pixelProfile.c_str(), &transparentOitCompositePS },
            { L"Shadow/ShadowVSM_PS.hlsl", "PS_ShadowVSM", pixelProfile.c_str(), &shadowVsmPS },
            { L"Shadow/ShadowVSM_GaussBlur_CS.hlsl", "CS_BlurH", computeProfile.c_str(), &shadowVsmBlurHCS },
            { L"Shadow/ShadowVSM_GaussBlur_CS.hlsl", "CS_BlurV", computeProfile.c_str(), &shadowVsmBlurVCS },
        } };

        for (const ShaderSpec& spec : shaderSpecs) {
            if (!loadOrCompileShader(spec)) {
                return false;
            }
        }

        D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        D3D12_INPUT_ELEMENT_DESC skyboxInputElementDescs[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        auto blendDesc = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        // Opaque pass: disable blending to avoid translucency
        blendDesc.RenderTarget[0].BlendEnable = FALSE;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        auto rast = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        rast.CullMode = D3D12_CULL_MODE_NONE;

        auto shadowRast = rast;
        shadowRast.DepthBias = 1000;
        shadowRast.DepthBiasClamp = 0.0f;
        shadowRast.SlopeScaledDepthBias = 2.0f;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoDesc.pRootSignature = m_rootSignature.Get();
        psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
        psoDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
        psoDesc.RasterizerState = rast;
        psoDesc.BlendState = blendDesc;
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 5;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT; // SceneColor HDR
        psoDesc.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;    // GBufferAlbedo
        psoDesc.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT; // GBufferNormal
        psoDesc.RTVFormats[3] = DXGI_FORMAT_R8G8B8A8_UNORM;    // GBufferMaterial
        psoDesc.RTVFormats[4] = DXGI_FORMAT_R16G16B16A16_FLOAT; // GBufferEmissive
        psoDesc.SampleDesc.Count = 1;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        auto drainD3D12InfoQueue = [&]()
        {
            ID3D12Device* nativeDevice = device.GetDevice();
            if (!nativeDevice) return;
            ComPtr<ID3D12InfoQueue> infoQueue;
            if (FAILED(nativeDevice->QueryInterface(IID_PPV_ARGS(&infoQueue)))) return;
            const UINT64 count = infoQueue->GetNumStoredMessages();
            for (UINT64 i = 0; i < count; ++i) {
                SIZE_T msgLen = 0;
                if (FAILED(infoQueue->GetMessage(i, nullptr, &msgLen)) || msgLen == 0) continue;
                std::vector<char> buf(msgLen);
                auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
                if (SUCCEEDED(infoQueue->GetMessage(i, msg, &msgLen)) && msg->pDescription) {
                    DebugLog("[D3D12] ");
                    DebugLog(msg->pDescription);
                    DebugLog("\n");
                }
            }
            infoQueue->ClearStoredMessages();
        };

        auto createPipelineState = [&](const char* label,
                                       const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                                       PipelineState& outState) -> bool
        {
            hr = device.CreateGraphicsPipelineState(desc, outState);
            if (FAILED(hr)) {
                std::string context = "RenderPipelineStateCache::Initialize: CreateGraphicsPipelineState(";
                context += label;
                context += ")";
                LogFailureMessage(context.c_str(), hr);
                drainD3D12InfoQueue();
                return false;
            }

            return true;
        };

        if (!createPipelineState("CookTorranceGGX", psoDesc, m_pipelineState)) { return false; }

        D3D12_BLEND_DESC transparentBlend = blendDesc;
        for (UINT rtIndex = 0; rtIndex < 5; ++rtIndex) {
            auto& rt = transparentBlend.RenderTarget[rtIndex];
            rt.BlendEnable = (rtIndex == 0) ? TRUE : FALSE;
            rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTransparent = psoDesc;
        psoTransparent.BlendState = transparentBlend;
        psoTransparent.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoTransparent.NumRenderTargets = 1;
        psoTransparent.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
        psoTransparent.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
        psoTransparent.RTVFormats[3] = DXGI_FORMAT_UNKNOWN;
        psoTransparent.RTVFormats[4] = DXGI_FORMAT_UNKNOWN;
        if (!createPipelineState("CookTorranceGGXTransparent", psoTransparent, m_transparentPipelineState)) { return false; }

        D3D12_BLEND_DESC oitBlend = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        oitBlend.IndependentBlendEnable = TRUE;
        oitBlend.RenderTarget[0].BlendEnable = TRUE;
        oitBlend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        oitBlend.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        oitBlend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        oitBlend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        oitBlend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        oitBlend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        oitBlend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        oitBlend.RenderTarget[1].BlendEnable = TRUE;
        oitBlend.RenderTarget[1].SrcBlend = D3D12_BLEND_ZERO;
        oitBlend.RenderTarget[1].DestBlend = D3D12_BLEND_INV_SRC_COLOR;
        oitBlend.RenderTarget[1].BlendOp = D3D12_BLEND_OP_ADD;
        oitBlend.RenderTarget[1].SrcBlendAlpha = D3D12_BLEND_ZERO;
        oitBlend.RenderTarget[1].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        oitBlend.RenderTarget[1].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        oitBlend.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTransparentOit = psoTransparent;
        psoTransparentOit.PS = { transparentOitPS->GetBufferPointer(), transparentOitPS->GetBufferSize() };
        psoTransparentOit.BlendState = oitBlend;
        psoTransparentOit.NumRenderTargets = 2;
        psoTransparentOit.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoTransparentOit.RTVFormats[1] = DXGI_FORMAT_R16_FLOAT;
        psoTransparentOit.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
        psoTransparentOit.RTVFormats[3] = DXGI_FORMAT_UNKNOWN;
        psoTransparentOit.RTVFormats[4] = DXGI_FORMAT_UNKNOWN;
        if (!createPipelineState("CookTorranceGGXTransparentOIT", psoTransparentOit, m_transparentOitPipelineState)) { return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTransparentBackfaceDistance = psoTransparent;
        auto backfaceDistanceRast = rast;
        backfaceDistanceRast.CullMode = D3D12_CULL_MODE_FRONT;
        psoTransparentBackfaceDistance.PS = { transparentBackfaceDistancePS->GetBufferPointer(), transparentBackfaceDistancePS->GetBufferSize() };
        psoTransparentBackfaceDistance.RasterizerState = backfaceDistanceRast;
        psoTransparentBackfaceDistance.BlendState = blendDesc;
        psoTransparentBackfaceDistance.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
        if (!createPipelineState("TransparentBackfaceDistance", psoTransparentBackfaceDistance, m_transparentBackfaceDistancePipelineState)) { return false; }

        // Opaque raster pipeline (unlit opaque draw path)  Esingle RTV only
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoBasic = psoDesc;
        psoBasic.VS = { basicVertexShader->GetBufferPointer(), basicVertexShader->GetBufferSize() };
        psoBasic.PS = { basicPixelShader->GetBufferPointer(), basicPixelShader->GetBufferSize() };
        psoBasic.NumRenderTargets = 1;
        psoBasic.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
        psoBasic.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
        psoBasic.RTVFormats[3] = DXGI_FORMAT_UNKNOWN;
        psoBasic.RTVFormats[4] = DXGI_FORMAT_UNKNOWN;
        if (!createPipelineState("Opaque", psoBasic, m_basicPipelineState)) { return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTransparentBasic = psoBasic;
        psoTransparentBasic.BlendState = transparentBlend;
        psoTransparentBasic.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        if (!createPipelineState("OpaqueTransparent", psoTransparentBasic, m_transparentBasicPipelineState)) { return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTransparentOitComposite = psoBasic;
        psoTransparentOitComposite.InputLayout = { nullptr, 0 };
        psoTransparentOitComposite.VS = { ssaoVS->GetBufferPointer(), ssaoVS->GetBufferSize() };
        psoTransparentOitComposite.PS = { transparentOitCompositePS->GetBufferPointer(), transparentOitCompositePS->GetBufferSize() };
        psoTransparentOitComposite.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoTransparentOitComposite.DepthStencilState.DepthEnable = FALSE;
        psoTransparentOitComposite.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoTransparentOitComposite.DepthStencilState.StencilEnable = FALSE;
        psoTransparentOitComposite.NumRenderTargets = 1;
        psoTransparentOitComposite.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        for (UINT rtIndex = 1; rtIndex < _countof(psoTransparentOitComposite.RTVFormats); ++rtIndex) {
            psoTransparentOitComposite.RTVFormats[rtIndex] = DXGI_FORMAT_UNKNOWN;
        }
        psoTransparentOitComposite.DSVFormat = DXGI_FORMAT_UNKNOWN;
        if (!createPipelineState("TransparentOITComposite", psoTransparentOitComposite, m_transparentOitCompositePipelineState)) { return false; }

        D3D12_BLEND_DESC additiveBlend = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        additiveBlend.RenderTarget[0].BlendEnable = TRUE;
        // FinalLit uses this pass as an additive specular-indirect contribution:
        // SceneColor.rgb = raster.rgb + SWRTComposite.rgb.
        additiveBlend.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        additiveBlend.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        additiveBlend.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        additiveBlend.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
        additiveBlend.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        additiveBlend.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        additiveBlend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSwrtReflectionComposite = psoBasic;
        psoSwrtReflectionComposite.InputLayout = { nullptr, 0 };
        psoSwrtReflectionComposite.VS = { ssaoVS->GetBufferPointer(), ssaoVS->GetBufferSize() };
        psoSwrtReflectionComposite.PS = { swrtReflectionCompositePS->GetBufferPointer(), swrtReflectionCompositePS->GetBufferSize() };
        psoSwrtReflectionComposite.BlendState = additiveBlend;
        psoSwrtReflectionComposite.DepthStencilState.DepthEnable = FALSE;
        psoSwrtReflectionComposite.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoSwrtReflectionComposite.DepthStencilState.StencilEnable = FALSE;
        psoSwrtReflectionComposite.NumRenderTargets = 1;
        for (UINT rtIndex = 1; rtIndex < _countof(psoSwrtReflectionComposite.RTVFormats); ++rtIndex) {
            psoSwrtReflectionComposite.RTVFormats[rtIndex] = DXGI_FORMAT_UNKNOWN;
        }
        psoSwrtReflectionComposite.DSVFormat = DXGI_FORMAT_UNKNOWN;
        if (!createPipelineState("SWRTReflectionComposite", psoSwrtReflectionComposite, m_swrtReflectionCompositePipelineState)) { return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoToneMap = psoBasic;
        psoToneMap.InputLayout = { nullptr, 0 };
        psoToneMap.VS = { ssaoVS->GetBufferPointer(), ssaoVS->GetBufferSize() };
        psoToneMap.PS = { toneMapPS->GetBufferPointer(), toneMapPS->GetBufferSize() };
        psoToneMap.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoToneMap.DepthStencilState.DepthEnable = FALSE;
        psoToneMap.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoToneMap.DepthStencilState.StencilEnable = FALSE;
        psoToneMap.NumRenderTargets = 1;
        psoToneMap.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        for (UINT rtIndex = 1; rtIndex < _countof(psoToneMap.RTVFormats); ++rtIndex) {
            psoToneMap.RTVFormats[rtIndex] = DXGI_FORMAT_UNKNOWN;
        }
        psoToneMap.DSVFormat = DXGI_FORMAT_UNKNOWN;
        if (!createPipelineState("ToneMap", psoToneMap, m_toneMapPipelineState)) { return false; }

        // Shadow pipeline (depth-only, reuse VS; no PS)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoShadow = {};
        psoShadow.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoShadow.pRootSignature = m_rootSignature.Get();
        psoShadow.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
        psoShadow.PS = { nullptr, 0 };
        psoShadow.RasterizerState = shadowRast;
        psoShadow.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoShadow.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoShadow.SampleMask = UINT_MAX;
        psoShadow.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoShadow.NumRenderTargets = 0;
        psoShadow.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoShadow.SampleDesc.Count = 1;

        if (!createPipelineState("Shadow", psoShadow, m_shadowPipelineState)) { return false; }

        // VSM shadow pipeline: same VS, writes (depth, depth²) to R32G32_FLOAT RTV
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoShadowVsm = psoShadow;
        psoShadowVsm.PS = { shadowVsmPS->GetBufferPointer(), shadowVsmPS->GetBufferSize() };
        psoShadowVsm.NumRenderTargets = 1;
        psoShadowVsm.RTVFormats[0] = DXGI_FORMAT_R32G32_FLOAT;
        psoShadowVsm.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        if (!createPipelineState("ShadowVSM", psoShadowVsm, m_shadowVsmPipelineState)) { return false; }

        // VSM Gaussian blur compute root signature and PSOs
        {
            D3D12_DESCRIPTOR_RANGE blurSrvRange{
                D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
            };
            D3D12_DESCRIPTOR_RANGE blurUavRange{
                D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
            };
            D3D12_ROOT_PARAMETER blurParams[3] = {};
            blurParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            blurParams[0].Constants.ShaderRegister = 0;
            blurParams[0].Constants.RegisterSpace = 0;
            blurParams[0].Constants.Num32BitValues = 4; // width, height, sliceIndex, pad
            blurParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            blurParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            blurParams[1].DescriptorTable.NumDescriptorRanges = 1;
            blurParams[1].DescriptorTable.pDescriptorRanges = &blurSrvRange;
            blurParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            blurParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            blurParams[2].DescriptorTable.NumDescriptorRanges = 1;
            blurParams[2].DescriptorTable.pDescriptorRanges = &blurUavRange;
            blurParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC blurRsDesc = {};
            blurRsDesc.NumParameters = 3;
            blurRsDesc.pParameters = blurParams;
            blurRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

            ComPtr<ID3DBlob> blurRsSig, blurRsErr;
            hr = D3D12SerializeRootSignature(&blurRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blurRsSig, &blurRsErr);
            if (FAILED(hr)) {
                if (blurRsErr) { DebugLog(static_cast<const char*>(blurRsErr->GetBufferPointer())); DebugLog("\n"); }
                LogFailureMessage("RenderPipelineStateCache::Initialize: VSM blur root signature serialization", hr);
                return false;
            }
            hr = device.CreateRootSignature(0, blurRsSig->GetBufferPointer(), blurRsSig->GetBufferSize(), m_vsmBlurRootSignature);
            if (FAILED(hr)) { LogFailureMessage("RenderPipelineStateCache::Initialize: VSM blur CreateRootSignature", hr); return false; }

            D3D12_COMPUTE_PIPELINE_STATE_DESC blurPsoH = {};
            blurPsoH.pRootSignature = m_vsmBlurRootSignature.Get();
            blurPsoH.CS = { shadowVsmBlurHCS->GetBufferPointer(), shadowVsmBlurHCS->GetBufferSize() };
            hr = device.CreateComputePipelineState(blurPsoH, m_vsmBlurHPso);
            if (FAILED(hr)) { LogFailureMessage("RenderPipelineStateCache::Initialize: VSM blur H PSO", hr); return false; }

            D3D12_COMPUTE_PIPELINE_STATE_DESC blurPsoV = {};
            blurPsoV.pRootSignature = m_vsmBlurRootSignature.Get();
            blurPsoV.CS = { shadowVsmBlurVCS->GetBufferPointer(), shadowVsmBlurVCS->GetBufferSize() };
            hr = device.CreateComputePipelineState(blurPsoV, m_vsmBlurVPso);
            if (FAILED(hr)) { LogFailureMessage("RenderPipelineStateCache::Initialize: VSM blur V PSO", hr); return false; }
        }

        // Tessellation pipeline (VS+HS+DS+GS+PS)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTess = {};
        psoTess.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoTess.pRootSignature = m_rootSignature.Get();
        psoTess.VS = { tessVS->GetBufferPointer(), tessVS->GetBufferSize() };
        psoTess.HS = { tessHS->GetBufferPointer(), tessHS->GetBufferSize() };
        psoTess.DS = { tessDS->GetBufferPointer(), tessDS->GetBufferSize() };
        psoTess.GS = { tessGS->GetBufferPointer(), tessGS->GetBufferSize() };
        psoTess.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() }; // Reuse PBR PS
        auto rastTess = rast;
        rastTess.CullMode = D3D12_CULL_MODE_NONE; // HS uses triangle_cw; keep NONE to match standard PSO
        psoTess.RasterizerState = rastTess;
        psoTess.BlendState = blendDesc;
        psoTess.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoTess.DepthStencilState.DepthEnable = TRUE;
        psoTess.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        psoTess.DepthStencilState.StencilEnable = FALSE;
        psoTess.SampleMask = UINT_MAX;
        psoTess.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        psoTess.NumRenderTargets = 5;
        psoTess.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT; // SceneColor HDR
        psoTess.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;    // GBufferAlbedo
        psoTess.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT; // GBufferNormal
        psoTess.RTVFormats[3] = DXGI_FORMAT_R8G8B8A8_UNORM;    // GBufferMaterial
        psoTess.RTVFormats[4] = DXGI_FORMAT_R16G16B16A16_FLOAT; // GBufferEmissive
        psoTess.SampleDesc.Count = 1;
        psoTess.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        if (!createPipelineState("Tessellation", psoTess, m_tessPipelineState)) { return false; }

        // Tessellation wireframe pipeline  Esame as tessellation but FillMode = WIREFRAME.
        // Lets the user visualize the polygon mesh formed by the tessellation stage.
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTessWF = psoTess;
            auto rastTessWF = rastTess;
            rastTessWF.FillMode = D3D12_FILL_MODE_WIREFRAME;
            psoTessWF.RasterizerState = rastTessWF;
            if (!createPipelineState("TessellationWireframe", psoTessWF, m_tessWireframePipelineState)) { return false; }
        }

        // Tessellation debug pipeline  Esame stages as tessellation but uses
        // Tessellation_Debug_PS which flat-shades by per-patch color (input.color).
        // This gives a clean meshlet-style patch boundary visualization.
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTessDbg = psoTess;
            psoTessDbg.DS = { tessDebugDS->GetBufferPointer(), tessDebugDS->GetBufferSize() };
            psoTessDbg.PS = { tessDebugPS->GetBufferPointer(), tessDebugPS->GetBufferSize() };
            if (!createPipelineState("TessellationDebug", psoTessDbg, m_tessDebugPipelineState)) { return false; }
        }

        // Meshlet debug pipeline  EVS + MeshletDebug_PS.
        // Uses SV_PrimitiveID / 16 to derive the meshlet index (exact for sequential
        // meshlet builds) so each meshlet group gets a unique color.  Works with the
        // standard DrawIndexedInstanced path; no mesh shader dispatch required.
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoMeshDbg = psoDesc;
            psoMeshDbg.PS = { meshletDebugPS->GetBufferPointer(), meshletDebugPS->GetBufferSize() };
            if (!createPipelineState("MeshletDebug", psoMeshDbg, m_meshletDebugPipelineState)) { return false; }
        }

        // Tessellation shadow pipeline (VS+HS+DS; depth-only)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTessShadow = {};
        psoTessShadow.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoTessShadow.pRootSignature = m_rootSignature.Get();
        psoTessShadow.VS = { tessVS->GetBufferPointer(), tessVS->GetBufferSize() };
        psoTessShadow.HS = { tessHS->GetBufferPointer(), tessHS->GetBufferSize() };
        psoTessShadow.DS = { tessDS->GetBufferPointer(), tessDS->GetBufferSize() };
        psoTessShadow.GS = { nullptr, 0 };
        psoTessShadow.PS = { nullptr, 0 };
        auto shadowRastTess = rastTess;
        shadowRastTess.DepthBias = shadowRast.DepthBias;
        shadowRastTess.DepthBiasClamp = shadowRast.DepthBiasClamp;
        shadowRastTess.SlopeScaledDepthBias = shadowRast.SlopeScaledDepthBias;
        psoTessShadow.RasterizerState = shadowRastTess;
        psoTessShadow.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoTessShadow.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoTessShadow.SampleMask = UINT_MAX;
        psoTessShadow.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        psoTessShadow.NumRenderTargets = 0;
        psoTessShadow.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoTessShadow.SampleDesc.Count = 1;

        if (!createPipelineState("TessellationShadow", psoTessShadow, m_tessShadowPipelineState)) { return false; }

        // Skybox pipeline (cube map)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkybox = {};
        psoSkybox.InputLayout = { skyboxInputElementDescs, _countof(skyboxInputElementDescs) };
        psoSkybox.pRootSignature = m_rootSignature.Get();
        psoSkybox.VS = { skyboxVS->GetBufferPointer(), skyboxVS->GetBufferSize() };
        psoSkybox.RasterizerState = rast;
        psoSkybox.BlendState = blendDesc;
        psoSkybox.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoSkybox.DepthStencilState.DepthEnable = TRUE;
        psoSkybox.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoSkybox.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        psoSkybox.DepthStencilState.StencilEnable = FALSE;
        psoSkybox.SampleMask = UINT_MAX;
        psoSkybox.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoSkybox.NumRenderTargets = 1;
        psoSkybox.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoSkybox.SampleDesc.Count = 1;
        psoSkybox.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkyboxHdr = psoSkybox;
        psoSkyboxHdr.PS = { skyboxHdrPS->GetBufferPointer(), skyboxHdrPS->GetBufferSize() };
        if (!createPipelineState("SkyboxHDR", psoSkyboxHdr, m_skyboxHdrPipelineState)) { return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkyboxLdr = psoSkybox;
        psoSkyboxLdr.PS = { skyboxLdrPS->GetBufferPointer(), skyboxLdrPS->GetBufferSize() };
        if (!createPipelineState("SkyboxLDR", psoSkyboxLdr, m_skyboxLdrPipelineState)) { return false; }

        // Procedural sky pipeline: same cube mesh + depth state as skybox, different PS (no texture)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoProceduralSky = psoSkybox;
        psoProceduralSky.PS = { proceduralSkyPS->GetBufferPointer(), proceduralSkyPS->GetBufferSize() };
        if (!createPipelineState("ProceduralSky", psoProceduralSky, m_proceduralSkyPipelineState)) { return false; }

        // RayMarch, VolumetricCloud ↁEInitializeEffectPipelines
        if (!InitializeEffectPipelines(device, vertexProfile, pixelProfile)) { return false; }

        // SSAO + SSAO Blur ↁEInitializeSsaoPipelines
        if (!InitializeSsaoPipelines(device, vertexProfile, pixelProfile)) { return false; }
        // Mesh shader pipeline  Eoptional, requires D3D12 Mesh Shader Tier 1
        InitializeMeshShaderPipeline(device, shaderModel);

        // -------------------------------------------------------------------------
        // Skinned mesh root signature: same 15 params as m_rootSignature + [15] b3 bone CB (VS-only)
        // -------------------------------------------------------------------------
        D3D12_ROOT_PARAMETER skinnedRootParams[16] = {};
        for (int i = 0; i < 15; ++i) skinnedRootParams[i] = rootParams[i];
        skinnedRootParams[15].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        skinnedRootParams[15].Descriptor.ShaderRegister = 3; // b3
        skinnedRootParams[15].Descriptor.RegisterSpace  = 0;
        skinnedRootParams[15].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC skinnedRootSigDesc = {};
        skinnedRootSigDesc.NumParameters    = 16;
        skinnedRootSigDesc.pParameters      = skinnedRootParams;
        skinnedRootSigDesc.NumStaticSamplers = 1;
        skinnedRootSigDesc.pStaticSamplers  = &samplerDesc;
        skinnedRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> skinnedSig, skinnedSigErr;
        hr = D3D12SerializeRootSignature(&skinnedRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &skinnedSig, &skinnedSigErr);
        if (FAILED(hr)) {
            if (skinnedSigErr) DebugLog(static_cast<const char*>(skinnedSigErr->GetBufferPointer()));
            LogFailureMessage("RenderPipelineStateCache: SkinnedMesh SerializeRootSignature", hr);
            return false;
        }
        hr = device.CreateRootSignature(0, skinnedSig->GetBufferPointer(), skinnedSig->GetBufferSize(), m_skinnedRootSignature);
        if (FAILED(hr)) {
            LogFailureMessage("RenderPipelineStateCache: SkinnedMesh CreateRootSignature", hr);
            return false;
        }

        // Load/compile SkinnedMesh_VS.hlsl
        ComPtr<ID3DBlob> skinnedVS;
        if (!loadOrCompileShader({ L"SkinnedMesh/SkinnedMesh_VS.hlsl", "VSMain", vertexProfile.c_str(), &skinnedVS })) {
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
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkinned = psoDesc;
        psoSkinned.InputLayout    = { skinnedInputLayout, _countof(skinnedInputLayout) };
        psoSkinned.pRootSignature = m_skinnedRootSignature.Get();
        psoSkinned.VS             = { skinnedVS->GetBufferPointer(), skinnedVS->GetBufferSize() };
        if (!createPipelineState("SkinnedOpaque", psoSkinned, m_skinnedPipelineState)) { return false; }

        // Transparent skinned PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkinnedTransparent = psoSkinned;
        psoSkinnedTransparent.BlendState = transparentBlend;
        psoSkinnedTransparent.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoSkinnedTransparent.NumRenderTargets = 1;
        for (UINT rtIndex = 1; rtIndex < 5; ++rtIndex)
            psoSkinnedTransparent.RTVFormats[rtIndex] = DXGI_FORMAT_UNKNOWN;
        if (!createPipelineState("SkinnedTransparent", psoSkinnedTransparent, m_skinnedTransparentPipelineState)) { return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkinnedTransparentOit = psoSkinnedTransparent;
        psoSkinnedTransparentOit.PS = { transparentOitPS->GetBufferPointer(), transparentOitPS->GetBufferSize() };
        psoSkinnedTransparentOit.BlendState = oitBlend;
        psoSkinnedTransparentOit.NumRenderTargets = 2;
        psoSkinnedTransparentOit.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoSkinnedTransparentOit.RTVFormats[1] = DXGI_FORMAT_R16_FLOAT;
        for (UINT rtIndex = 2; rtIndex < 5; ++rtIndex) {
            psoSkinnedTransparentOit.RTVFormats[rtIndex] = DXGI_FORMAT_UNKNOWN;
        }
        if (!createPipelineState("SkinnedTransparentOIT", psoSkinnedTransparentOit, m_skinnedTransparentOitPipelineState)) { return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkinnedTransparentBackfaceDistance = psoSkinnedTransparent;
        psoSkinnedTransparentBackfaceDistance.PS = { transparentBackfaceDistancePS->GetBufferPointer(), transparentBackfaceDistancePS->GetBufferSize() };
        psoSkinnedTransparentBackfaceDistance.RasterizerState = backfaceDistanceRast;
        psoSkinnedTransparentBackfaceDistance.BlendState = blendDesc;
        psoSkinnedTransparentBackfaceDistance.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
        if (!createPipelineState("SkinnedTransparentBackfaceDistance", psoSkinnedTransparentBackfaceDistance, m_skinnedTransparentBackfaceDistancePipelineState)) { return false; }

        // Shadow skinned PSO (depth-only, same shadow rasterizer bias)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkinnedShadow = {};
        psoSkinnedShadow.InputLayout    = { skinnedInputLayout, _countof(skinnedInputLayout) };
        psoSkinnedShadow.pRootSignature = m_skinnedRootSignature.Get();
        psoSkinnedShadow.VS             = { skinnedVS->GetBufferPointer(), skinnedVS->GetBufferSize() };
        psoSkinnedShadow.RasterizerState = shadowRast;
        psoSkinnedShadow.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoSkinnedShadow.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoSkinnedShadow.DepthStencilState.DepthEnable = TRUE;
        psoSkinnedShadow.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        psoSkinnedShadow.SampleMask = UINT_MAX;
        psoSkinnedShadow.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoSkinnedShadow.NumRenderTargets = 0;
        psoSkinnedShadow.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoSkinnedShadow.SampleDesc.Count = 1;
        if (!createPipelineState("SkinnedShadow", psoSkinnedShadow, m_skinnedShadowPipelineState)) { return false; }

        return true;
    }

    // -------------------------------------------------------------------------

    RhiPipelineHandle RenderPipelineStateCache::MakePipelineHandle(const PipelineState& pso)
    {
        return RhiPipelineHandle{ reinterpret_cast<uint64_t>(pso.Get()) };
    }

    RhiPipelineLayoutHandle RenderPipelineStateCache::MakeLayoutHandle(const RootSignature& sig)
    {
        return RhiPipelineLayoutHandle{ reinterpret_cast<uint64_t>(sig.Get()) };
    }

    RhiDescriptorHeapHandle RenderPipelineStateCache::MakeDescriptorHeapHandle(const DescriptorHeap& heap)
    {
        return RhiDescriptorHeapHandle{ reinterpret_cast<uint64_t>(heap.Get()) };
    }
}
