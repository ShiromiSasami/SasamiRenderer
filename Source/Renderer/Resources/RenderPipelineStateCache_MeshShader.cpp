// RenderPipelineStateCache_MeshShader.cpp
// Amplification + Mesh + Pixel shader pipeline (D3D12 Ultimate / SM 6.5+).
// Non-fatal: initialisation is skipped silently when the hardware tier is not supported.
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Resources/ShaderCompilationService.h"

#include <array>
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

        static bool LoadMsShaderBlob(const wchar_t* relPath, const char* entry, const char* profile,
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
                std::string msg = "RenderPipelineStateCache: MeshShader compile failed: ";
                msg += srcPath.string();
                msg += "\n";
                DebugLog(msg.c_str());
                return false;
            }

            std::error_code ec;
            std::filesystem::create_directories(compiledPath.parent_path(), ec);
            if (!ec)
                D3DWriteBlobToFile(outBlob.Get(), compiledPath.c_str(), TRUE);

            return true;
        }

        // Pipeline stream subobject wrappers for CreatePipelineState
        template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE SubobjectType, typename InnerType>
        struct alignas(void*) PsoSubobject
        {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type  = SubobjectType;
            InnerType                           inner = {};
        };

        using PsoRootSignature    = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,    ID3D12RootSignature*>;
        using PsoAS               = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS,                D3D12_SHADER_BYTECODE>;
        using PsoMS               = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS,                D3D12_SHADER_BYTECODE>;
        using PsoPS               = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,                D3D12_SHADER_BYTECODE>;
        using PsoBlend            = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,             D3D12_BLEND_DESC>;
        using PsoRasterizer       = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,        D3D12_RASTERIZER_DESC>;
        using PsoDepthStencil     = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL,     D3D12_DEPTH_STENCIL_DESC>;
        using PsoRenderTargets    = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY>;
        using PsoDepthStencilFmt  = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT>;
        using PsoSampleMask       = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,       UINT>;
        using PsoSampleDesc       = PsoSubobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,       DXGI_SAMPLE_DESC>;

        struct MeshShaderPipelineStream
        {
            PsoRootSignature   rootSignature;
            PsoAS              as;
            PsoMS              ms;
            PsoPS              ps;
            PsoBlend           blend;
            PsoRasterizer      rasterizer;
            PsoDepthStencil    depthStencil;
            PsoRenderTargets   renderTargets;
            PsoDepthStencilFmt dsvFormat;
            PsoSampleMask      sampleMask;
            PsoSampleDesc      sampleDesc;
        };

    } // anonymous namespace

    bool RenderPipelineStateCache::InitializeMeshShaderPipeline(GraphicsDevice& device,
                                                                 const std::string& shaderModel)
    {
        ID3D12Device* nativeDevice = device.GetDevice();
        if (!nativeDevice)
            return false;

        // Feature check: require Mesh Shader Tier 1
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7{};
        if (FAILED(nativeDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
                                                      &opts7, sizeof(opts7))) ||
            opts7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
            DebugLog("RenderPipelineStateCache: Mesh Shader Tier not supported on this GPU. "
                     "MeshShader pipeline will be disabled.\n");
            return true; // non-fatal
        }

        DebugLog("RenderPipelineStateCache: Mesh Shader Tier 1 supported. "
                 "Initialising MeshShader pipeline.\n");

        // Mesh/Amplification shaders require SM 6.5 minimum.
        const std::string meshSmVersion = "6_5";
        const std::string asProfile = "as_" + meshSmVersion;
        const std::string msProfile = "ms_" + meshSmVersion;
        const std::string psProfile = "ps_" + shaderModel;

        ComPtr<ID3DBlob> asBlob, msBlob, psBlob;
        if (!LoadMsShaderBlob(L"MeshShader/MeshShader_AS.hlsl", "AS_Meshlet", asProfile.c_str(), asBlob)) return false;
        if (!LoadMsShaderBlob(L"MeshShader/MeshShader_MS.hlsl", "MS_Meshlet", msProfile.c_str(), msBlob)) return false;
        if (!LoadMsShaderBlob(L"PBR/CookTorranceGGX_PS.hlsl",   "PSMain",     psProfile.c_str(), psBlob)) return false;

        // Root Signature
        // [0] t0 - MeshletDesc SRV   (AS + MS, inline root SRV)
        // [1] t1 - Vertex SRV        (MS, inline root SRV)
        // [2] t2 - MeshletIndex SRV  (MS, inline root SRV)
        // [3] b1 - DrawCB 32-bit constants (AS + MS, 34 uint32)
        // [4] b0 - CameraCB (AS + MS, root CBV)
        D3D12_ROOT_PARAMETER msParams[5] = {};
        msParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        msParams[0].Descriptor.ShaderRegister = 0;
        msParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        msParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        msParams[1].Descriptor.ShaderRegister = 1;
        msParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_MESH;

        msParams[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        msParams[2].Descriptor.ShaderRegister = 2;
        msParams[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_MESH;

        msParams[3].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        msParams[3].Constants.ShaderRegister = 1;
        msParams[3].Constants.RegisterSpace  = 0;
        msParams[3].Constants.Num32BitValues = 34;
        msParams[3].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        msParams[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        msParams[4].Descriptor.ShaderRegister = 0;
        msParams[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC msSampler = {};
        msSampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        msSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        msSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        msSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        msSampler.ShaderRegister   = 0;
        msSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC msRsDesc = {};
        msRsDesc.NumParameters    = 5;
        msRsDesc.pParameters      = msParams;
        msRsDesc.NumStaticSamplers = 1;
        msRsDesc.pStaticSamplers  = &msSampler;
        msRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                         D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS   |
                         D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                         D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        HRESULT hr = S_OK;
        ComPtr<ID3DBlob> msSig, msSigErr;
        hr = D3D12SerializeRootSignature(&msRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &msSig, &msSigErr);
        if (FAILED(hr)) {
            if (msSigErr && msSigErr->GetBufferSize() > 0) {
                DebugLog(static_cast<const char*>(msSigErr->GetBufferPointer()));
                DebugLog("\n");
            }
            LogFail("RenderPipelineStateCache: MeshShader SerializeRootSignature", hr);
            return false;
        }
        hr = device.CreateRootSignature(0, msSig->GetBufferPointer(), msSig->GetBufferSize(),
                                        m_meshShaderRootSignature);
        if (FAILED(hr)) {
            LogFail("RenderPipelineStateCache: MeshShader CreateRootSignature", hr);
            return false;
        }

        // Pipeline State Stream
        MeshShaderPipelineStream stream{};
        stream.rootSignature.inner = m_meshShaderRootSignature.Get();
        stream.as.inner            = { asBlob->GetBufferPointer(), asBlob->GetBufferSize() };
        stream.ms.inner            = { msBlob->GetBufferPointer(), msBlob->GetBufferSize() };
        stream.ps.inner            = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };

        auto blendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        blendState.RenderTarget[0].BlendEnable          = FALSE;
        blendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        stream.blend.inner = blendState;

        auto rastState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        rastState.CullMode = D3D12_CULL_MODE_BACK;
        stream.rasterizer.inner = rastState;

        auto dsState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        dsState.DepthEnable   = TRUE;
        dsState.DepthFunc     = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        dsState.StencilEnable = FALSE;
        stream.depthStencil.inner = dsState;

        D3D12_RT_FORMAT_ARRAY rtFormats{};
        rtFormats.NumRenderTargets = 5;
        rtFormats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtFormats.RTFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtFormats.RTFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rtFormats.RTFormats[3] = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtFormats.RTFormats[4] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        stream.renderTargets.inner = rtFormats;

        stream.dsvFormat.inner   = DXGI_FORMAT_D32_FLOAT;
        stream.sampleMask.inner  = UINT_MAX;
        DXGI_SAMPLE_DESC sd{}; sd.Count = 1; sd.Quality = 0;
        stream.sampleDesc.inner  = sd;

        hr = device.CreatePipelineStateFromStream(&stream, sizeof(MeshShaderPipelineStream),
                                                   m_meshShaderPipelineState);
        if (FAILED(hr)) {
            LogFail("RenderPipelineStateCache: MeshShader CreatePipelineState", hr);
            return false;
        }

        DebugLog("RenderPipelineStateCache: MeshShader pipeline initialised successfully.\n");
        return true;
    }

} // namespace SasamiRenderer
