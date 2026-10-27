// RenderPipelineStateCacheMeshShader.cpp
// Amplification + Mesh + Pixel shader pipeline (D3D12 Ultimate / SM 6.5+), plus its depth-only
// shadow sibling. The GBuffer pipeline's pixel shader (MeshShaderGBuffer_PS) writes the same
// five GBuffer targets as the VS-driven OpaqueGBuffer pass, so it can feed the shared deferred
// lighting pass. The shadow pipeline (MS_ShadowCluster, no PS) renders directional cascaded
// shadows and shares the AS blob and root signature with the GBuffer pipeline, and shares its
// rasterizer state with the VS-driven shadow PSO via MakeDirectionalShadowRasterizerDesc().
// Also builds a separate bindless root signature and depth-only MS-only (no AS) PSO
// (MS_ShadowIndirect, ms_6_6, ResourceDescriptorHeap) used by the indirect GPU-driven cluster
// shadow dispatch, whose visible-cluster list is produced by a prior compute cull pass.
// Non-fatal: initialisation is skipped silently when the hardware tier is not supported.
#include "Renderer/Resources/RenderPipelineStateCache.h"
#include "Renderer/Resources/RenderPipelineStateCacheLog.h"
#include "Renderer/Resources/ShaderBlobCache.h"
#include "Renderer/Resources/ShaderCompilationService.h"
#include "Renderer/Resources/ShadowRasterizerState.h"

#include <array>
#include <d3dcompiler.h>
#include <filesystem>
#include <string>
#include <windows.h>

#include "Foundation/Tools/DebugOutput.h"
#include "d3dx12.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace SasamiEngine::Render
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        static bool LoadMsShaderBlob(ShaderBlobCache& cache, const wchar_t* relPath, const char* entry, const char* profile,
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
                    std::string msg = "RenderPipelineStateCache: MeshShader compile failed: ";
                    msg += srcPath.string();
                    msg += "\n";
                    DebugLog(msg.c_str());
                    return ComPtr<ID3DBlob>();
                }

                std::error_code ec;
                std::filesystem::create_directories(compiledPath.parent_path(), ec);
                if (!ec)
                    D3DWriteBlobToFile(blob.Get(), compiledPath.c_str(), TRUE);

                return blob;
            });

            return outBlob.Get() != nullptr;
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
        if (!LoadMsShaderBlob(m_shaderBlobCache, L"Raster/Geometry/MeshShader/MeshShader_AS.hlsl", "AS_Meshlet", asProfile.c_str(), asBlob)) return false;
        if (!LoadMsShaderBlob(m_shaderBlobCache, L"Raster/Geometry/MeshShader/MeshShader_MS.hlsl", "MS_Meshlet", msProfile.c_str(), msBlob)) return false;
        if (!LoadMsShaderBlob(m_shaderBlobCache, L"Raster/Geometry/MeshShader/MeshShaderGBuffer_PS.hlsl", "PSMain", psProfile.c_str(), psBlob)) return false;

        // Root Signature (virtual-geometry cluster path)
        // [0]  t0 - ClusterDesc SRV      (AS + MS, inline root SRV)
        // [1]  t1 - Vertex SRV           (MS, inline root SRV)
        // [2]  t2 - ClusterVertexId SRV  (MS, inline root SRV)
        // [3]  b1 - DrawCB 32-bit constants (AS + MS, 36 uint32: model 16 + inverseModel 16 + 4 uints)
        // [4]  b0 - CameraCB             (AS + MS + PS, root CBV)
        // [5]  b2 - MaterialCB           (PS, root CBV, per-draw material block)
        // [6]  t3 - Albedo texture       (PS, descriptor table, 1 SRV range)
        // [7]  t4 - Occlusion texture    (PS, descriptor table, 1 SRV range)
        // [8]  t5 - Normal texture       (PS, descriptor table, 1 SRV range)
        // [9]  t6 - ClusterTriangle SRV  (MS, inline root SRV)
        // [10] t7 - GroupDesc SRV        (AS, inline root SRV)
        // [11] t8 - Emissive texture     (PS, descriptor table, 1 SRV range)
        // [12] t9 - Face-shadow map      (PS, descriptor table, 1 SRV range; ToonFaceShadow.hlsli)
        // Root cost (dwords): 36 (32-bit constants) + 2x2 (root CBVs b0,b2) + 5x2 (root SRVs
        // t0,t1,t2,t6,t7) + 5x1 (descriptor tables t3,t4,t5,t8,t9) = 36 + 4 + 10 + 5 = 55 <= 64
        D3D12_ROOT_PARAMETER msParams[13] = {};
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
        msParams[3].Constants.Num32BitValues = 36;
        msParams[3].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

        msParams[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        msParams[4].Descriptor.ShaderRegister = 0;
        msParams[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        msParams[5].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        msParams[5].Descriptor.ShaderRegister = 2;
        msParams[5].Descriptor.RegisterSpace  = 0;
        msParams[5].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_DESCRIPTOR_RANGE msAlbedoSrvRange = {};
        msAlbedoSrvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        msAlbedoSrvRange.NumDescriptors                    = 1;
        msAlbedoSrvRange.BaseShaderRegister                = 3;
        msAlbedoSrvRange.RegisterSpace                     = 0;
        msAlbedoSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        msParams[6].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        msParams[6].DescriptorTable.NumDescriptorRanges = 1;
        msParams[6].DescriptorTable.pDescriptorRanges   = &msAlbedoSrvRange;
        msParams[6].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_DESCRIPTOR_RANGE msOcclusionSrvRange = {};
        msOcclusionSrvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        msOcclusionSrvRange.NumDescriptors                    = 1;
        msOcclusionSrvRange.BaseShaderRegister                = 4;
        msOcclusionSrvRange.RegisterSpace                     = 0;
        msOcclusionSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        msParams[7].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        msParams[7].DescriptorTable.NumDescriptorRanges = 1;
        msParams[7].DescriptorTable.pDescriptorRanges   = &msOcclusionSrvRange;
        msParams[7].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_DESCRIPTOR_RANGE msNormalSrvRange = {};
        msNormalSrvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        msNormalSrvRange.NumDescriptors                    = 1;
        msNormalSrvRange.BaseShaderRegister                = 5;
        msNormalSrvRange.RegisterSpace                     = 0;
        msNormalSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        msParams[8].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        msParams[8].DescriptorTable.NumDescriptorRanges = 1;
        msParams[8].DescriptorTable.pDescriptorRanges   = &msNormalSrvRange;
        msParams[8].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        msParams[9].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        msParams[9].Descriptor.ShaderRegister = 6;
        msParams[9].ShaderVisibility          = D3D12_SHADER_VISIBILITY_MESH;

        msParams[10].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        msParams[10].Descriptor.ShaderRegister = 7;
        msParams[10].ShaderVisibility          = D3D12_SHADER_VISIBILITY_AMPLIFICATION;

        D3D12_DESCRIPTOR_RANGE msEmissiveSrvRange = {};
        msEmissiveSrvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        msEmissiveSrvRange.NumDescriptors                    = 1;
        msEmissiveSrvRange.BaseShaderRegister                = 8;
        msEmissiveSrvRange.RegisterSpace                     = 0;
        msEmissiveSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        msParams[11].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        msParams[11].DescriptorTable.NumDescriptorRanges = 1;
        msParams[11].DescriptorTable.pDescriptorRanges   = &msEmissiveSrvRange;
        msParams[11].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_DESCRIPTOR_RANGE msFaceShadowSrvRange = msEmissiveSrvRange;
        msFaceShadowSrvRange.BaseShaderRegister = 9;

        msParams[12].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        msParams[12].DescriptorTable.NumDescriptorRanges = 1;
        msParams[12].DescriptorTable.pDescriptorRanges   = &msFaceShadowSrvRange;
        msParams[12].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC msSampler = MakeLinearWrapSampler();

        D3D12_ROOT_SIGNATURE_DESC msRsDesc = {};
        msRsDesc.NumParameters    = _countof(msParams);
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
        // Matches the VS GBuffer path; MeshShaderGBuffer_PS flips the normal on back faces via SV_IsFrontFace.
        rastState.CullMode = D3D12_CULL_MODE_NONE;
        stream.rasterizer.inner = rastState;

        auto dsState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        dsState.DepthEnable   = TRUE;
        // Reverse-Z: near=1.0, far=0.0, so a fragment passes when its depth is >= what's stored.
        dsState.DepthFunc     = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        dsState.StencilEnable = FALSE;
        stream.depthStencil.inner = dsState;

        // Must match psoGBuffer (RenderPipelineStateCache.cpp) — the pass writes the same GBuffer targets.
        D3D12_RT_FORMAT_ARRAY rtFormats{};
        rtFormats.NumRenderTargets = 5;
        rtFormats.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // GBufferAlbedo (PS writes linear; _SRGB view encodes on write/decodes on read to preserve dark precision in 8-bit)
        rtFormats.RTFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;  // GBufferNormal
        rtFormats.RTFormats[2] = DXGI_FORMAT_R8G8B8A8_UNORM;      // GBufferMaterial
        rtFormats.RTFormats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;  // GBufferEmissive
        rtFormats.RTFormats[4] = DXGI_FORMAT_R8G8B8A8_UNORM;      // GBufferSpecularWorkflow
        stream.renderTargets.inner = rtFormats;

        stream.dsvFormat.inner   = DXGI_FORMAT_D32_FLOAT;
        stream.sampleMask.inner  = UINT_MAX;
        DXGI_SAMPLE_DESC sd{}; sd.Count = 1; sd.Quality = 0;
        stream.sampleDesc.inner  = sd;

        hr = device.CreatePipelineStateFromStream(&stream, sizeof(MeshShaderPipelineStream),
                                                   m_meshShaderPipelineState);
        if (FAILED(hr)) {
            LogPipelineStateFailure("RenderPipelineStateCache: MeshShader CreatePipelineState",
                                    hr,
                                    nativeDevice);
            return false;
        }

        DebugLog("RenderPipelineStateCache: MeshShader pipeline initialised successfully.\n");

        // Depth-only mesh-shader PSO for directional cascaded shadows. Reuses the GBuffer
        // pipeline's AS blob and root signature; only the MS (depth-only MS_ShadowCluster) and
        // the pipeline state (no PS, shadow rasterizer/depth-stencil) differ.
        ComPtr<ID3DBlob> shadowMsBlob;
        if (!LoadMsShaderBlob(m_shaderBlobCache, L"Raster/Geometry/MeshShader/MeshShaderShadow_MS.hlsl", "MS_ShadowCluster", msProfile.c_str(), shadowMsBlob)) {
            DebugLog("RenderPipelineStateCache: MeshShaderShadow shader load failed. "
                     "MeshShaderShadow pipeline will be disabled.\n");
            return true;
        }

        MeshShaderPipelineStream shadowStream{};
        shadowStream.rootSignature.inner = m_meshShaderRootSignature.Get();
        shadowStream.as.inner            = { asBlob->GetBufferPointer(), asBlob->GetBufferSize() };
        shadowStream.ms.inner            = { shadowMsBlob->GetBufferPointer(), shadowMsBlob->GetBufferSize() };
        // No pixel shader for this depth-only pass: leaving stream.ps at its zero-initialised
        // { nullptr, 0 } bytecode is equivalent to omitting the PS subobject.

        shadowStream.blend.inner = blendState;

        shadowStream.rasterizer.inner = MakeDirectionalShadowRasterizerDesc();

        auto shadowDsState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        shadowDsState.DepthEnable    = TRUE;
        shadowDsState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        // Reverse-Z shadow depth pass, matches psoShadow (RenderPipelineStateCache.cpp).
        shadowDsState.DepthFunc      = D3D12_COMPARISON_FUNC_GREATER;
        shadowDsState.StencilEnable  = FALSE;
        shadowStream.depthStencil.inner = shadowDsState;

        D3D12_RT_FORMAT_ARRAY shadowRtFormats{};
        shadowRtFormats.NumRenderTargets = 0;
        shadowStream.renderTargets.inner = shadowRtFormats;

        shadowStream.dsvFormat.inner  = DXGI_FORMAT_D32_FLOAT;
        shadowStream.sampleMask.inner = UINT_MAX;
        DXGI_SAMPLE_DESC shadowSd{}; shadowSd.Count = 1; shadowSd.Quality = 0;
        shadowStream.sampleDesc.inner = shadowSd;

        hr = device.CreatePipelineStateFromStream(&shadowStream, sizeof(MeshShaderPipelineStream),
                                                   m_meshShaderShadowPipelineState);
        if (FAILED(hr)) {
            LogPipelineStateFailure("RenderPipelineStateCache: MeshShaderShadow CreatePipelineState",
                                    hr,
                                    nativeDevice);
        } else {
            DebugLog("RenderPipelineStateCache: MeshShaderShadow pipeline initialised successfully.\n");
        }

        // Bindless root signature for the indirect (GPU-driven) cluster shadow dispatch.
        // The mesh shader (MS_ShadowIndirect) indexes the page pool/instances/visible-list/
        // counter through ResourceDescriptorHeap, so no descriptor tables or static samplers
        // are needed; the signature only opts into direct heap indexing.
        // [0] b0 - CameraCB     (root CBV)
        // [1] t0 - Page pool SRV (root SRV, raw ByteAddressBuffer)
        // [2] t1 - Instances SRV (root SRV)
        // [3] t2 - VisibleList SRV (root SRV, uint2)
        // [4] t3 - Counter SRV  (root SRV, raw ByteAddressBuffer)
        D3D12_ROOT_PARAMETER clusterIndirectParams[5] = {};
        clusterIndirectParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        clusterIndirectParams[0].Descriptor.ShaderRegister = 0;
        clusterIndirectParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_MESH;

        clusterIndirectParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        clusterIndirectParams[1].Descriptor.ShaderRegister = 0;
        clusterIndirectParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_MESH;

        clusterIndirectParams[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        clusterIndirectParams[2].Descriptor.ShaderRegister = 1;
        clusterIndirectParams[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_MESH;

        clusterIndirectParams[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        clusterIndirectParams[3].Descriptor.ShaderRegister = 2;
        clusterIndirectParams[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_MESH;

        clusterIndirectParams[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        clusterIndirectParams[4].Descriptor.ShaderRegister = 3;
        clusterIndirectParams[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_MESH;

        D3D12_ROOT_SIGNATURE_DESC clusterIndirectRsDesc = {};
        clusterIndirectRsDesc.NumParameters     = _countof(clusterIndirectParams);
        clusterIndirectRsDesc.pParameters       = clusterIndirectParams;
        clusterIndirectRsDesc.NumStaticSamplers = 0;
        clusterIndirectRsDesc.pStaticSamplers   = nullptr;
        clusterIndirectRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                                       D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS   |
                                       D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS     |
                                       D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS   |
                                       D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
                                       D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

        ComPtr<ID3DBlob> clusterIndirectSig, clusterIndirectSigErr;
        hr = D3D12SerializeRootSignature(&clusterIndirectRsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &clusterIndirectSig, &clusterIndirectSigErr);
        if (FAILED(hr)) {
            if (clusterIndirectSigErr && clusterIndirectSigErr->GetBufferSize() > 0) {
                DebugLog(static_cast<const char*>(clusterIndirectSigErr->GetBufferPointer()));
                DebugLog("\n");
            }
            LogFail("RenderPipelineStateCache: ClusterIndirect SerializeRootSignature", hr);
            return true;
        }
        hr = device.CreateRootSignature(0, clusterIndirectSig->GetBufferPointer(), clusterIndirectSig->GetBufferSize(),
                                        m_clusterIndirectRootSignature);
        if (FAILED(hr)) {
            LogFail("RenderPipelineStateCache: ClusterIndirect CreateRootSignature", hr);
            return true;
        }

        // MS-only PSO (no AS, no PS) for the indirect cluster shadow dispatch. The device
        // supports up to SM 6.8, but this shader specifically requires ms_6_6 for
        // ResourceDescriptorHeap, hence the local profile literal instead of msProfile above.
        const char* indirectMsProfile = "ms_6_6";
        ComPtr<ID3DBlob> indirectMsBlob;
        if (!LoadMsShaderBlob(m_shaderBlobCache, L"Raster/Geometry/MeshShader/MeshShaderShadowIndirect_MS.hlsl", "MS_ShadowIndirect", indirectMsProfile, indirectMsBlob)) {
            DebugLog("RenderPipelineStateCache: ClusterIndirectShadow shader load failed. "
                     "ClusterIndirectShadow pipeline will be disabled.\n");
            return true;
        }

        MeshShaderPipelineStream indirectStream{};
        indirectStream.rootSignature.inner = m_clusterIndirectRootSignature.Get();
        // No amplification shader: the visible-cluster list is produced by a separate compute
        // cull pass, so leaving stream.as at its zero-initialised { nullptr, 0 } bytecode is
        // equivalent to omitting the AS subobject, same trick as the null PS above.
        indirectStream.ms.inner = { indirectMsBlob->GetBufferPointer(), indirectMsBlob->GetBufferSize() };

        indirectStream.blend.inner      = blendState;
        indirectStream.rasterizer.inner = MakeDirectionalShadowRasterizerDesc();

        auto indirectDsState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        indirectDsState.DepthEnable    = TRUE;
        indirectDsState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        // Reverse-Z shadow depth pass, matches psoShadow (RenderPipelineStateCache.cpp).
        indirectDsState.DepthFunc      = D3D12_COMPARISON_FUNC_GREATER;
        indirectDsState.StencilEnable  = FALSE;
        indirectStream.depthStencil.inner = indirectDsState;

        D3D12_RT_FORMAT_ARRAY indirectRtFormats{};
        indirectRtFormats.NumRenderTargets = 0;
        indirectStream.renderTargets.inner = indirectRtFormats;

        indirectStream.dsvFormat.inner  = DXGI_FORMAT_D32_FLOAT;
        indirectStream.sampleMask.inner = UINT_MAX;
        DXGI_SAMPLE_DESC indirectSd{}; indirectSd.Count = 1; indirectSd.Quality = 0;
        indirectStream.sampleDesc.inner = indirectSd;

        hr = device.CreatePipelineStateFromStream(&indirectStream, sizeof(MeshShaderPipelineStream),
                                                   m_clusterIndirectShadowPipelineState);
        if (FAILED(hr)) {
            LogPipelineStateFailure("RenderPipelineStateCache: ClusterIndirectShadow CreatePipelineState",
                                    hr,
                                    nativeDevice);
        } else {
            DebugLog("RenderPipelineStateCache: ClusterIndirectShadow pipeline initialised successfully.\n");
        }

        // Root signature for the indirect (GPU-driven) main-view GBuffer cluster draw. Same
        // select/visible-list contract as the shadow signature above, but the pixel shader is
        // active: it reads CameraCB, the PIXEL-visible material table (t4) and the linear wrap
        // sampler (s0), and samples material textures through ResourceDescriptorHeap.
        // [0] b0 - CameraCB        (ALL, root CBV)
        // [1] t0 - Page pool SRV   (MESH, root SRV, raw ByteAddressBuffer)
        // [2] t1 - Instances SRV   (MESH, root SRV)
        // [3] t2 - VisibleList SRV (MESH, root SRV, uint2)
        // [4] t3 - Counter SRV     (MESH, root SRV, raw ByteAddressBuffer)
        // [5] t4 - Material table  (PIXEL, root SRV, StructuredBuffer<GpuClusterMaterial>)
        D3D12_ROOT_PARAMETER clusterIndirectGBufferParams[6] = {};
        clusterIndirectGBufferParams[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        clusterIndirectGBufferParams[0].Descriptor.ShaderRegister = 0;
        clusterIndirectGBufferParams[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        for (UINT i = 1; i <= 4; ++i) {
            clusterIndirectGBufferParams[i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
            clusterIndirectGBufferParams[i].Descriptor.ShaderRegister = i - 1;
            clusterIndirectGBufferParams[i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_MESH;
        }

        clusterIndirectGBufferParams[5].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        clusterIndirectGBufferParams[5].Descriptor.ShaderRegister = 4;
        clusterIndirectGBufferParams[5].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC clusterIndirectGBufferSampler = MakeLinearWrapSampler(0);

        D3D12_ROOT_SIGNATURE_DESC clusterIndirectGBufferRsDesc = {};
        clusterIndirectGBufferRsDesc.NumParameters     = _countof(clusterIndirectGBufferParams);
        clusterIndirectGBufferRsDesc.pParameters       = clusterIndirectGBufferParams;
        clusterIndirectGBufferRsDesc.NumStaticSamplers = 1;
        clusterIndirectGBufferRsDesc.pStaticSamplers   = &clusterIndirectGBufferSampler;
        clusterIndirectGBufferRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                                              D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS   |
                                              D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS     |
                                              D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS   |
                                              D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ComPtr<ID3DBlob> clusterIndirectGBufferSig, clusterIndirectGBufferSigErr;
        hr = D3D12SerializeRootSignature(&clusterIndirectGBufferRsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &clusterIndirectGBufferSig, &clusterIndirectGBufferSigErr);
        if (FAILED(hr)) {
            if (clusterIndirectGBufferSigErr && clusterIndirectGBufferSigErr->GetBufferSize() > 0) {
                DebugLog(static_cast<const char*>(clusterIndirectGBufferSigErr->GetBufferPointer()));
                DebugLog("\n");
            }
            LogFail("RenderPipelineStateCache: ClusterIndirectGBuffer SerializeRootSignature", hr);
            return true;
        }
        hr = device.CreateRootSignature(0, clusterIndirectGBufferSig->GetBufferPointer(),
                                        clusterIndirectGBufferSig->GetBufferSize(),
                                        m_clusterIndirectGBufferRootSignature);
        if (FAILED(hr)) {
            LogFail("RenderPipelineStateCache: ClusterIndirectGBuffer CreateRootSignature", hr);
            return true;
        }

        // MS + PS PSO (no AS) for the indirect main-view GBuffer draw. ps_6_6 is needed for
        // ResourceDescriptorHeap. Null PSO makes the renderer fall back to the CPU-driven draw.
        const char* indirectGBufferPsProfile = "ps_6_6";
        ComPtr<ID3DBlob> indirectGBufferMsBlob, indirectGBufferPsBlob;
        if (!LoadMsShaderBlob(m_shaderBlobCache, L"Raster/Geometry/MeshShader/MeshShaderGBufferIndirect_MS.hlsl", "MS_GBufferIndirect", indirectMsProfile, indirectGBufferMsBlob) ||
            !LoadMsShaderBlob(m_shaderBlobCache, L"Raster/Geometry/MeshShader/MeshShaderGBufferIndirect_PS.hlsl", "PSMain", indirectGBufferPsProfile, indirectGBufferPsBlob)) {
            DebugLog("RenderPipelineStateCache: ClusterIndirectGBuffer shader load failed. "
                     "ClusterIndirectGBuffer pipeline will be disabled.\n");
            return true;
        }

        MeshShaderPipelineStream indirectGBufferStream{};
        indirectGBufferStream.rootSignature.inner = m_clusterIndirectGBufferRootSignature.Get();
        indirectGBufferStream.ms.inner = { indirectGBufferMsBlob->GetBufferPointer(), indirectGBufferMsBlob->GetBufferSize() };
        indirectGBufferStream.ps.inner = { indirectGBufferPsBlob->GetBufferPointer(), indirectGBufferPsBlob->GetBufferSize() };

        indirectGBufferStream.blend.inner        = blendState;
        indirectGBufferStream.rasterizer.inner   = rastState;
        indirectGBufferStream.depthStencil.inner = dsState;
        indirectGBufferStream.renderTargets.inner = rtFormats;

        indirectGBufferStream.dsvFormat.inner  = DXGI_FORMAT_D32_FLOAT;
        indirectGBufferStream.sampleMask.inner = UINT_MAX;
        indirectGBufferStream.sampleDesc.inner = sd;

        hr = device.CreatePipelineStateFromStream(&indirectGBufferStream, sizeof(MeshShaderPipelineStream),
                                                   m_clusterIndirectGBufferPipelineState);
        if (FAILED(hr)) {
            LogPipelineStateFailure("RenderPipelineStateCache: ClusterIndirectGBuffer CreatePipelineState",
                                    hr,
                                    nativeDevice);
        } else {
            DebugLog("RenderPipelineStateCache: ClusterIndirectGBuffer pipeline initialised successfully.\n");
        }

        return true;
    }

} // namespace SasamiEngine::Render
