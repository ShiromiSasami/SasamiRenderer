#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Resources/RenderPipelineStateCacheLog.h"
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

        D3D12_ROOT_PARAMETER rootParams[17] = {};
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

        // [15] t15 - point light cube shadow map (Texture2DArray, 6 faces), PS only
        D3D12_DESCRIPTOR_RANGE descRangePointShadow{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 15,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        rootParams[15].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[15].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[15].DescriptorTable.pDescriptorRanges = &descRangePointShadow;
        rootParams[15].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // [16] t17 - material normal map; optional, bound per draw with a flat-normal
        // fallback. t5/t6 are NOT free: the IBL range at t4 spans three descriptors
        // (t4-t6), so a range there fails D3D12SerializeRootSignature with E_INVALIDARG.
        D3D12_DESCRIPTOR_RANGE descRangeMaterialNormal{
            .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
            .NumDescriptors = 1,
            .BaseShaderRegister = 17,
            .RegisterSpace = 0,
            .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
        };
        rootParams[16].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[16].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[16].DescriptorTable.pDescriptorRanges = &descRangeMaterialNormal;
        rootParams[16].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // s0 = general linear/wrap sampling, s1 = shadow-map depth comparison (PCF).
        // Shared verbatim by the opaque, deferred and skinned root signatures below.
        const D3D12_STATIC_SAMPLER_DESC staticSamplers[] = {
            MakeLinearWrapSampler(0),
            MakeShadowComparisonSampler(1),
        };

        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.NumParameters = _countof(rootParams);
        rootSigDesc.pParameters = rootParams;
        rootSigDesc.NumStaticSamplers = _countof(staticSamplers);
        rootSigDesc.pStaticSamplers = staticSamplers;
        rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
        if (FAILED(hr)) {
            if (error && error->GetBufferPointer() && error->GetBufferSize() > 0) {
                DebugLog(static_cast<const char*>(error->GetBufferPointer()));
                DebugLog("\n");
            }
            LogFail("RenderPipelineStateCache::Initialize: D3D12SerializeRootSignature", hr);
            return false;
        }

        hr = device.CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), m_rootSignature);
        if (FAILED(hr)) {
            LogFail("RenderPipelineStateCache::Initialize: CreateRootSignature", hr);
            return false;
        }

        // Resolve shader blobs from precompiled CSO first, then compile only stale/missing ones.
        ComPtr<ID3DBlob> vertexShader;
        ComPtr<ID3DBlob> pixelShader;
        ComPtr<ID3DBlob> gbufferPixelShader;
        ComPtr<ID3DBlob> deferredLightingPixelShader;
        ComPtr<ID3DBlob> gbufferDebugPixelShader;
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
        ComPtr<ID3DBlob> fxaaPS;
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

            *spec.output = m_shaderBlobCache.GetOrResolve(compiledPath.wstring(), [&]() -> ComPtr<ID3DBlob>
            {
                ComPtr<ID3DBlob> blob;
                if (ShaderCompilationService::IsCompiledShaderUpToDate(compiledPath, sourcePath)) {
                    const HRESULT readHr = D3DReadFileToBlob(compiledPath.c_str(), blob.GetAddressOf());
                    if (SUCCEEDED(readHr)) {
                        ShaderCompilationService::LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "loaded precompiled shader", compiledPath);
                        return blob;
                    }

                    ShaderCompilationService::LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "failed to read precompiled shader, falling back to runtime compile", compiledPath);
                } else {
                    ShaderCompilationService::LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "precompiled shader missing or stale, runtime compiling", compiledPath);
                }

                ScopedPerfTimer::Timestamp compileStart = ScopedPerfTimer::Now();
                const bool compileSucceeded = ShaderCompilationService::CompileShader(sourcePath, spec.entry, spec.target, blob);
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
                    return ComPtr<ID3DBlob>();
                }

                std::error_code ec;
                std::filesystem::create_directories(compiledPath.parent_path(), ec);
                if (!ec) {
                    const HRESULT writeHr = D3DWriteBlobToFile(blob.Get(), compiledPath.c_str(), TRUE);
                    if (FAILED(writeHr)) {
                        ShaderCompilationService::LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "compiled shader could not be written", compiledPath);
                    } else {
                        ShaderCompilationService::LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "runtime compiled shader and updated cache", compiledPath);
                    }
                } else {
                    ShaderCompilationService::LogShaderResolveMessage(sourcePath, spec.entry, spec.target, "cache directory creation failed; keeping runtime blob only", compiledPath);
                }

                return blob;
            });

            return spec.output->Get() != nullptr;
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
        const std::array<ShaderSpec, 32> shaderSpecs{ {
            { L"Raster/Lighting/PBR/CookTorranceGGX_VS.hlsl", "VSMain", vertexProfile.c_str(), &vertexShader },
            { L"Raster/Lighting/PBR/CookTorranceGGX_PS.hlsl", "PSMain", pixelProfile.c_str(), &pixelShader },
            { L"Raster/Geometry/OpaqueGBuffer/OpaqueGBuffer_PS.hlsl", "PSMain", pixelProfile.c_str(), &gbufferPixelShader },
            { L"Raster/Lighting/Deferred/DeferredLighting_PS.hlsl", "PSMain", pixelProfile.c_str(), &deferredLightingPixelShader },
            { L"Debug/GBuffer/GBufferDebug_PS.hlsl", "PSMain", pixelProfile.c_str(), &gbufferDebugPixelShader },
            { L"Effects/Sky/Skybox/Skybox_VS.hlsl", "VSMain", vertexProfile.c_str(), &skyboxVS },
            { L"Effects/Sky/Skybox/Skybox_HDR_PS.hlsl", "PSMain", pixelProfile.c_str(), &skyboxHdrPS },
            { L"Effects/Sky/Skybox/Skybox_PS.hlsl", "PSMain", pixelProfile.c_str(), &skyboxLdrPS },
            { L"Raster/Geometry/Tessellation/Tessellation_VS.hlsl", "VSMain", vertexProfile.c_str(), &tessVS },
            { L"Raster/Geometry/Tessellation/Tessellation_HS.hlsl", "HSMain", hullProfile.c_str(), &tessHS },
            { L"Raster/Geometry/Tessellation/Tessellation_DS.hlsl", "DSMain", domainProfile.c_str(), &tessDS },
            { L"Raster/Geometry/Tessellation/Tessellation_GS.hlsl", "GSMain", geometryProfile.c_str(), &tessGS },
            { L"Raster/Geometry/Tessellation/Tessellation_Debug_DS.hlsl", "DSMain", domainProfile.c_str(), &tessDebugDS },
            { L"Raster/Geometry/Tessellation/Tessellation_Debug_PS.hlsl", "PSMain", pixelProfile.c_str(), &tessDebugPS },
            { L"Debug/Meshlet/MeshletDebug_PS.hlsl", "PSMain", pixelProfile.c_str(), &meshletDebugPS },
            { L"Effects/AmbientOcclusion/SSAO/SSAO_VS.hlsl", "VSMain", vertexProfile.c_str(), &ssaoVS },
            { L"Effects/AmbientOcclusion/SSAO/SSAO_PS.hlsl", "PSMain", pixelProfile.c_str(), &ssaoPS },
            { L"Effects/AmbientOcclusion/SSAO/SSAO_Blur_PS.hlsl", "PSMain", pixelProfile.c_str(), &ssaoBlurPS },
            { L"Effects/Sky/ProceduralSky/ProceduralSky_PS.hlsl", "PSMain", pixelProfile.c_str(), &proceduralSkyPS },
            { L"Effects/Volumetric/Cloud/VolumetricCloud_VS.hlsl", "VSMain", vertexProfile.c_str(), &volumetricCloudVS },
            { L"Effects/Volumetric/Cloud/VolumetricCloud_PS.hlsl", "PSMain", pixelProfile.c_str(), &volumetricCloudPS },
            { L"RayTracing/SWRT/SWRT_ReflectionComposite_PS.hlsl", "PSMain", pixelProfile.c_str(), &swrtReflectionCompositePS },
            { L"Effects/PostProcess/ToneMap_PS.hlsl", "PSMain", pixelProfile.c_str(), &toneMapPS },
            { L"Effects/PostProcess/FXAA_PS.hlsl", "PSMain", pixelProfile.c_str(), &fxaaPS },
            { L"Effects/RayMarch/RayMarch_VS.hlsl", "VSMain", vertexProfile.c_str(), &rayMarchVS },
            { L"Effects/RayMarch/RayMarch_PS.hlsl", "PSMain", pixelProfile.c_str(), &rayMarchPS },
            { L"Raster/Transparency/TransparentBackfaceDistance_PS.hlsl", "PSMain", pixelProfile.c_str(), &transparentBackfaceDistancePS },
            { L"Raster/Transparency/TransparentOIT_PS.hlsl", "PSMain", pixelProfile.c_str(), &transparentOitPS },
            { L"Raster/Transparency/TransparentOITComposite_PS.hlsl", "PSMain", pixelProfile.c_str(), &transparentOitCompositePS },
            { L"Raster/Lighting/Shadow/ShadowVSM_PS.hlsl", "PS_ShadowVSM", pixelProfile.c_str(), &shadowVsmPS },
            { L"Raster/Lighting/Shadow/ShadowVSM_GaussBlur_CS.hlsl", "CS_BlurH", computeProfile.c_str(), &shadowVsmBlurHCS },
            { L"Raster/Lighting/Shadow/ShadowVSM_GaussBlur_CS.hlsl", "CS_BlurV", computeProfile.c_str(), &shadowVsmBlurVCS },
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

        // D16_UNORM の point/spot cube shadow 専用バイアス。D16 の DepthBias は固定NDC加算
        // (D32_FLOAT の指数スケーリングと異なり z=0 近傍でも縮小されない)なので、
        // 90度FOVの近距離パースペクティブでは shadowRast のカスケード向け値だと
        // オクルーダー深度が受け手深度を追い越し、遮蔽影が消える(peter-panning)。
        auto pointSpotShadowRast = rast;
        pointSpotShadowRast.DepthBias = 24;
        pointSpotShadowRast.DepthBiasClamp = 0.01f;
        pointSpotShadowRast.SlopeScaledDepthBias = 0.5f;

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
        ApplyCommonPsoDefaults(psoDesc);
        psoDesc.NumRenderTargets = 5;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT; // SceneColor HDR
        psoDesc.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;    // GBufferAlbedo
        psoDesc.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT; // GBufferNormal
        psoDesc.RTVFormats[3] = DXGI_FORMAT_R8G8B8A8_UNORM;    // GBufferMaterial
        psoDesc.RTVFormats[4] = DXGI_FORMAT_R16G16B16A16_FLOAT; // GBufferEmissive
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        auto drainD3D12InfoQueue = [&]()
        {
            DrainD3D12InfoQueue(device.GetDevice());
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
                LogFail(context.c_str(), hr);
                drainD3D12InfoQueue();
                return false;
            }

            return true;
        };

        if (!createPipelineState("CookTorranceGGX", psoDesc, m_pipelineState)) { return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoGBuffer = psoDesc;
        psoGBuffer.PS = { gbufferPixelShader->GetBufferPointer(), gbufferPixelShader->GetBufferSize() };
        psoGBuffer.NumRenderTargets = 5;
        psoGBuffer.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;     // GBufferAlbedo
        psoGBuffer.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT; // GBufferNormal
        psoGBuffer.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;     // GBufferMaterial
        psoGBuffer.RTVFormats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT; // GBufferEmissive
        psoGBuffer.RTVFormats[4] = DXGI_FORMAT_R8G8B8A8_UNORM;     // GBufferSpecularWorkflow
        for (UINT rtIndex = 5; rtIndex < _countof(psoGBuffer.RTVFormats); ++rtIndex) {
            psoGBuffer.RTVFormats[rtIndex] = DXGI_FORMAT_UNKNOWN;
        }
        if (!createPipelineState("OpaqueGBuffer", psoGBuffer, m_gbufferPipelineState)) { return false; }

        {
            D3D12_DESCRIPTOR_RANGE deferredRanges[14] = {};
            const UINT deferredBaseRegisters[14] = {
                0,  // GBufferAlbedo
                1,  // GBufferNormal
                2,  // GBufferMaterial
                3,  // GBufferEmissive
                4,  // ShadowMap
                5,  // SceneDepth
                6,  // Point/spot light buffers, range t6-t7
                8,  // Irradiance cubemap
                9,  // Runtime AO
                11, // Reflection
                12, // Spot shadow map
                13, // VSM shadow array
                14, // Transparent backface distance
                15, // GBufferSpecularWorkflow
            };
            for (UINT i = 0; i < 14; ++i) {
                deferredRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                deferredRanges[i].NumDescriptors = 1;
                deferredRanges[i].BaseShaderRegister = deferredBaseRegisters[i];
                deferredRanges[i].RegisterSpace = 0;
                deferredRanges[i].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            }
            deferredRanges[6].NumDescriptors = 2; // t6-t7 point/spot light buffers

            D3D12_ROOT_PARAMETER deferredParams[19] = {};
            for (UINT i = 0; i < 14; ++i) {
                deferredParams[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                deferredParams[i].DescriptorTable.NumDescriptorRanges = 1;
                deferredParams[i].DescriptorTable.pDescriptorRanges = &deferredRanges[i];
                deferredParams[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            }
            deferredParams[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            deferredParams[14].Descriptor.ShaderRegister = 1;
            deferredParams[14].Descriptor.RegisterSpace = 0;
            deferredParams[14].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            deferredParams[15].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            deferredParams[15].Descriptor.ShaderRegister = 2;
            deferredParams[15].Descriptor.RegisterSpace = 0;
            deferredParams[15].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            deferredParams[16].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            deferredParams[16].Descriptor.ShaderRegister = 10;
            deferredParams[16].Descriptor.RegisterSpace = 0;
            deferredParams[16].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            // [17] t16 - point light cube shadow map (Texture2DArray, 6 faces), PS only
            D3D12_DESCRIPTOR_RANGE descRangePointShadowDeferred{
                .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                .NumDescriptors = 1,
                .BaseShaderRegister = 16,
                .RegisterSpace = 0,
                .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
            };
            deferredParams[17].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            deferredParams[17].DescriptorTable.NumDescriptorRanges = 1;
            deferredParams[17].DescriptorTable.pDescriptorRanges = &descRangePointShadowDeferred;
            deferredParams[17].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            // [18] t17-t18 - prefiltered environment cube + BRDF LUT for split-sum specular IBL;
            // the irradiance cube stays at t8 (param 7).
            D3D12_DESCRIPTOR_RANGE descRangeIblSpecularDeferred{
                .RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                .NumDescriptors = 2,
                .BaseShaderRegister = 17,
                .RegisterSpace = 0,
                .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
            };
            deferredParams[18].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            deferredParams[18].DescriptorTable.NumDescriptorRanges = 1;
            deferredParams[18].DescriptorTable.pDescriptorRanges = &descRangeIblSpecularDeferred;
            deferredParams[18].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_ROOT_SIGNATURE_DESC deferredRootDesc = {};
            deferredRootDesc.NumParameters = _countof(deferredParams);
            deferredRootDesc.pParameters = deferredParams;
            deferredRootDesc.NumStaticSamplers = _countof(staticSamplers);
            deferredRootDesc.pStaticSamplers = staticSamplers;
            deferredRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

            ComPtr<ID3DBlob> deferredSignature;
            ComPtr<ID3DBlob> deferredError;
            hr = D3D12SerializeRootSignature(&deferredRootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &deferredSignature, &deferredError);
            if (FAILED(hr)) {
                if (deferredError && deferredError->GetBufferPointer() && deferredError->GetBufferSize() > 0) {
                    DebugLog(static_cast<const char*>(deferredError->GetBufferPointer()));
                    DebugLog("\n");
                }
                LogFail("RenderPipelineStateCache::Initialize: DeferredLighting root signature", hr);
                return false;
            }
            hr = device.CreateRootSignature(0,
                                            deferredSignature->GetBufferPointer(),
                                            deferredSignature->GetBufferSize(),
                                            m_deferredLightingRootSignature);
            if (FAILED(hr)) {
                LogFail("RenderPipelineStateCache::Initialize: DeferredLighting CreateRootSignature", hr);
                return false;
            }

            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDeferredLighting = psoDesc;
            psoDeferredLighting.InputLayout = { nullptr, 0 };
            psoDeferredLighting.pRootSignature = m_deferredLightingRootSignature.Get();
            psoDeferredLighting.VS = { ssaoVS->GetBufferPointer(), ssaoVS->GetBufferSize() };
            psoDeferredLighting.PS = { deferredLightingPixelShader->GetBufferPointer(), deferredLightingPixelShader->GetBufferSize() };
            psoDeferredLighting.DepthStencilState.DepthEnable = FALSE;
            psoDeferredLighting.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            psoDeferredLighting.DepthStencilState.StencilEnable = FALSE;
            psoDeferredLighting.NumRenderTargets = 1;
            psoDeferredLighting.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            for (UINT rtIndex = 1; rtIndex < _countof(psoDeferredLighting.RTVFormats); ++rtIndex) {
                psoDeferredLighting.RTVFormats[rtIndex] = DXGI_FORMAT_UNKNOWN;
            }
            psoDeferredLighting.DSVFormat = DXGI_FORMAT_UNKNOWN;
            if (!createPipelineState("DeferredLighting", psoDeferredLighting, m_deferredLightingPipelineState)) { return false; }

            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoGBufferDebug = psoDeferredLighting;
            psoGBufferDebug.PS = { gbufferDebugPixelShader->GetBufferPointer(), gbufferDebugPixelShader->GetBufferSize() };
            if (!createPipelineState("GBufferDebug", psoGBufferDebug, m_gbufferDebugPipelineState)) { return false; }
        }

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

        // Shared base desc for the fullscreen composite/tonemap passes below Esingle RTV only
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoBasic = psoDesc;
        psoBasic.NumRenderTargets = 1;
        psoBasic.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
        psoBasic.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
        psoBasic.RTVFormats[3] = DXGI_FORMAT_UNKNOWN;
        psoBasic.RTVFormats[4] = DXGI_FORMAT_UNKNOWN;

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

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoFxaa = psoBasic;
        psoFxaa.InputLayout = { nullptr, 0 };
        psoFxaa.VS = { ssaoVS->GetBufferPointer(), ssaoVS->GetBufferSize() };
        psoFxaa.PS = { fxaaPS->GetBufferPointer(), fxaaPS->GetBufferSize() };
        psoFxaa.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoFxaa.DepthStencilState.DepthEnable = FALSE;
        psoFxaa.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoFxaa.DepthStencilState.StencilEnable = FALSE;
        psoFxaa.NumRenderTargets = 1;
        psoFxaa.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        for (UINT rtIndex = 1; rtIndex < _countof(psoFxaa.RTVFormats); ++rtIndex) {
            psoFxaa.RTVFormats[rtIndex] = DXGI_FORMAT_UNKNOWN;
        }
        psoFxaa.DSVFormat = DXGI_FORMAT_UNKNOWN;
        if (!createPipelineState("Fxaa", psoFxaa, m_fxaaPipelineState)) { return false; }

        // Shadow pipeline (depth-only, reuse VS; no PS)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoShadow = {};
        psoShadow.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoShadow.pRootSignature = m_rootSignature.Get();
        psoShadow.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
        psoShadow.PS = { nullptr, 0 };
        psoShadow.RasterizerState = shadowRast;
        psoShadow.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoShadow.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        ApplyCommonPsoDefaults(psoShadow);
        psoShadow.NumRenderTargets = 0;
        psoShadow.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        if (!createPipelineState("Shadow", psoShadow, m_shadowPipelineState)) { return false; }

        // D16_UNORM DSV版(スポット/ポイントライトの512x512シャドウマップ用)。
        // それ以外は psoShadow と同一(DSVFormat の不一致は GPU-Based Validation の
        // "depth stencil format does not match" エラー、悪化するとTDRの一因になる)。
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoShadowD16 = psoShadow;
        psoShadowD16.DSVFormat = DXGI_FORMAT_D16_UNORM;
        psoShadowD16.RasterizerState = pointSpotShadowRast;
        if (!createPipelineState("ShadowD16", psoShadowD16, m_shadowD16PipelineState)) { return false; }

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
                LogFail("RenderPipelineStateCache::Initialize: VSM blur root signature serialization", hr);
                return false;
            }
            hr = device.CreateRootSignature(0, blurRsSig->GetBufferPointer(), blurRsSig->GetBufferSize(), m_vsmBlurRootSignature);
            if (FAILED(hr)) { LogFail("RenderPipelineStateCache::Initialize: VSM blur CreateRootSignature", hr); return false; }

            D3D12_COMPUTE_PIPELINE_STATE_DESC blurPsoH = {};
            blurPsoH.pRootSignature = m_vsmBlurRootSignature.Get();
            blurPsoH.CS = { shadowVsmBlurHCS->GetBufferPointer(), shadowVsmBlurHCS->GetBufferSize() };
            hr = device.CreateComputePipelineState(blurPsoH, m_vsmBlurHPso);
            if (FAILED(hr)) { LogFail("RenderPipelineStateCache::Initialize: VSM blur H PSO", hr); return false; }

            D3D12_COMPUTE_PIPELINE_STATE_DESC blurPsoV = {};
            blurPsoV.pRootSignature = m_vsmBlurRootSignature.Get();
            blurPsoV.CS = { shadowVsmBlurVCS->GetBufferPointer(), shadowVsmBlurVCS->GetBufferSize() };
            hr = device.CreateComputePipelineState(blurPsoV, m_vsmBlurVPso);
            if (FAILED(hr)) { LogFail("RenderPipelineStateCache::Initialize: VSM blur V PSO", hr); return false; }
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
        ApplyCommonPsoDefaults(psoTess, D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH);
        psoTess.NumRenderTargets = 5;
        psoTess.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT; // SceneColor HDR
        psoTess.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;    // GBufferAlbedo
        psoTess.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT; // GBufferNormal
        psoTess.RTVFormats[3] = DXGI_FORMAT_R8G8B8A8_UNORM;    // GBufferMaterial
        psoTess.RTVFormats[4] = DXGI_FORMAT_R16G16B16A16_FLOAT; // GBufferEmissive
        psoTess.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        if (!createPipelineState("Tessellation", psoTess, m_tessPipelineState)) { return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTessGBuffer = psoTess;
        psoTessGBuffer.PS = { gbufferPixelShader->GetBufferPointer(), gbufferPixelShader->GetBufferSize() };
        psoTessGBuffer.NumRenderTargets = 5;
        psoTessGBuffer.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoTessGBuffer.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoTessGBuffer.RTVFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoTessGBuffer.RTVFormats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoTessGBuffer.RTVFormats[4] = DXGI_FORMAT_R8G8B8A8_UNORM;
        for (UINT rtIndex = 5; rtIndex < _countof(psoTessGBuffer.RTVFormats); ++rtIndex) {
            psoTessGBuffer.RTVFormats[rtIndex] = DXGI_FORMAT_UNKNOWN;
        }
        if (!createPipelineState("TessellationGBuffer", psoTessGBuffer, m_tessGBufferPipelineState)) { return false; }

        // Tessellation wireframe pipeline  Esame as tessellation but FillMode = WIREFRAME.
        // Lets the user visualize the polygon mesh formed by the tessellation stage.
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTessWF = psoTess;
            auto rastTessWF = rastTess;
            rastTessWF.FillMode = D3D12_FILL_MODE_WIREFRAME;
            psoTessWF.RasterizerState = rastTessWF;
            if (!createPipelineState("TessellationWireframe", psoTessWF, m_tessWireframePipelineState)) { return false; }

            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTessGBufferWF = psoTessGBuffer;
            psoTessGBufferWF.RasterizerState = rastTessWF;
            if (!createPipelineState("TessellationGBufferWireframe", psoTessGBufferWF, m_tessGBufferWireframePipelineState)) { return false; }
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
        // Uses SV_PrimitiveID / kMaxTrianglesPerMeshlet to derive the meshlet index
        // (exact for sequential meshlet builds) so each meshlet group gets a unique
        // color.  The divisor lives in Shaders/Shared/Common/MeshletConstants.hlsli so
        // it tracks MeshletBuffer::kMaxTrianglesPerMeshlet.  Works with the standard
        // DrawIndexedInstanced path; no mesh shader dispatch required, which is why the
        // PS must NOT read a per-primitive attribute here - a VS cannot supply one.
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
        ApplyCommonPsoDefaults(psoTessShadow, D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH);
        psoTessShadow.NumRenderTargets = 0;
        psoTessShadow.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        if (!createPipelineState("TessellationShadow", psoTessShadow, m_tessShadowPipelineState)) { return false; }

        // D16_UNORM DSV版(スポット/ポイントライトの512x512シャドウマップ用、テッセレーションパス有効時)。
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTessShadowD16 = psoTessShadow;
        psoTessShadowD16.DSVFormat = DXGI_FORMAT_D16_UNORM;
        psoTessShadowD16.RasterizerState = pointSpotShadowRast;
        if (!createPipelineState("TessellationShadowD16", psoTessShadowD16, m_tessShadowD16PipelineState)) { return false; }

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
        ApplyCommonPsoDefaults(psoSkybox);
        psoSkybox.NumRenderTargets = 1;
        psoSkybox.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
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
        if (!InitializeScreenSpaceReflectionPipeline(device, computeProfile)) { return false; }
        // Mesh shader pipeline  Eoptional, requires D3D12 Mesh Shader Tier 1
        InitializeMeshShaderPipeline(device, shaderModel);

        SkinnedPipelineInputs skinnedInputs{
            .rootParams = rootParams,
            .staticSamplers = staticSamplers,
            .vertexProfile = vertexProfile,
            .psoDesc = psoDesc,
            .psoGBuffer = psoGBuffer,
            .blendDesc = blendDesc,
            .transparentBlend = transparentBlend,
            .oitBlend = oitBlend,
            .transparentOitPS = transparentOitPS,
            .transparentBackfaceDistancePS = transparentBackfaceDistancePS,
            .backfaceDistanceRast = backfaceDistanceRast,
            .shadowRast = shadowRast,
            .pointSpotShadowRast = pointSpotShadowRast,
        };
        if (!InitializeSkinnedPipelines(device, skinnedInputs)) {
            return false;
        }

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
