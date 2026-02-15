#include "Renderer/Core/RenderLayerConfigurator.h"

#include <d3dcompiler.h>
#include <debugapi.h>
#include <filesystem>
#include <string>
#include <windows.h>

#include "Foundation/Diagnostics/DebugOutput.h"

#pragma comment(lib, "d3dcompiler.lib")

namespace SasamiRenderer
{
    using Microsoft::WRL::ComPtr;

    namespace
    {
        static std::filesystem::path GetExecutableDir()
        {
            wchar_t exePath[MAX_PATH] = {};
            const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (len == 0 || len == MAX_PATH) {
                return std::filesystem::current_path();
            }
            return std::filesystem::path(exePath).parent_path();
        }

        static std::filesystem::path FindProjectRootWithShaders(const std::filesystem::path& startDir)
        {
            std::error_code ec;
            std::filesystem::path dir = std::filesystem::absolute(startDir, ec);
            if (ec) {
                dir = startDir;
            }

            for (;;) {
                const std::filesystem::path shaderDir = dir / L"Source" / L"Renderer" / L"Shaders";
                if (std::filesystem::exists(shaderDir, ec) &&
                    std::filesystem::is_directory(shaderDir, ec)) {
                    return dir;
                }

                const std::filesystem::path parent = dir.parent_path();
                if (parent.empty() || parent == dir) {
                    break;
                }
                dir = parent;
            }

            return {};
        }

        static std::filesystem::path ResolveShaderPath(const wchar_t* fileName)
        {
            // Fixed shader directory by project convention.
            const std::filesystem::path projectRoot = FindProjectRootWithShaders(GetExecutableDir());
            if (!projectRoot.empty()) {
                return projectRoot / L"Source" / L"Renderer" / L"Shaders" / fileName;
            }
            return std::filesystem::path(L"Source/Renderer/Shaders") / fileName;
        }
    }

    bool RenderLayerConfigurator::Initialize(GraphicsDevice& device)
    {
        // Root signature:
        // [0]=material SRV (t0), [1]=shadow SRV (t1), [2]=CBV (b0), [3]=CBV (b1),
        // [4]=light SRVs (t2-t3), [5]=IBL SRVs (t4-t6)
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

        D3D12_ROOT_PARAMETER rootParams[6] = {};
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

        D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplerDesc.ShaderRegister = 0;
        samplerDesc.RegisterSpace = 0;
        samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.NumParameters = 6;
        rootSigDesc.pParameters = rootParams;
        rootSigDesc.NumStaticSamplers = 1;
        rootSigDesc.pStaticSamplers = &samplerDesc;
        rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
        if (FAILED(hr)) { return false; }

        hr = device.CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), m_rootSignature);
        if (FAILED(hr)) { return false; }

        // Compile shaders (PBR forward)
        ComPtr<ID3DBlob> vertexShader;
        ComPtr<ID3DBlob> pixelShader;
        ComPtr<ID3DBlob> basicVertexShader;
        ComPtr<ID3DBlob> basicPixelShader;
        ComPtr<ID3DBlob> skyboxVS;
        ComPtr<ID3DBlob> skyboxHdrPS;
        ComPtr<ID3DBlob> skyboxLdrPS;
        auto compileShader = [&](const std::filesystem::path& path,
                                 const char* entry,
                                 const char* target,
                                 ComPtr<ID3DBlob>& outShader) -> bool
        {
            hr = D3DCompileFromFile(path.c_str(), nullptr, nullptr, entry, target, 0, 0, &outShader, &error);
            if (FAILED(hr)) {
                std::string msg = "RenderLayerConfigurator::Initialize: shader compile failed: ";
                msg += path.string();
                msg += "\n";
                DebugLog(msg.c_str());
                if (error && error->GetBufferPointer()) {
                    DebugLog(static_cast<const char*>(error->GetBufferPointer()));
                    DebugLog("\n");
                }
                return false;
            }
            return true;
        };

        const auto pbrVsPath = ResolveShaderPath(L"PBR_VS.hlsl");
        if (!compileShader(pbrVsPath, "VSMain", "vs_5_0", vertexShader)) { return false; }
        const auto pbrPsPath = ResolveShaderPath(L"PBR_PS.hlsl");
        if (!compileShader(pbrPsPath, "PSMain", "ps_5_0", pixelShader)) { return false; }
        const auto basicShaderPath = ResolveShaderPath(L"BasicShader.hlsl");
        if (!compileShader(basicShaderPath, "VSMain", "vs_5_0", basicVertexShader)) { return false; }
        if (!compileShader(basicShaderPath, "PSMain", "ps_5_0", basicPixelShader)) { return false; }
        const auto skyVsPath = ResolveShaderPath(L"Skybox_VS.hlsl");
        if (!compileShader(skyVsPath, "VSMain", "vs_5_0", skyboxVS)) { return false; }
        // Skybox PS split:
        // - Skybox_PS.hlsl      : LDR cubemap path (direct sample)
        // - Skybox_HDR_PS.hlsl  : HDR cubemap path (tone map + gamma)
        const auto skyHdrPsPath = ResolveShaderPath(L"Skybox_HDR_PS.hlsl");
        if (!compileShader(skyHdrPsPath, "PSMain", "ps_5_0", skyboxHdrPS)) { return false; }
        const auto skyLdrPsPath = ResolveShaderPath(L"Skybox_PS.hlsl");
        if (!compileShader(skyLdrPsPath, "PSMain", "ps_5_0", skyboxLdrPS)) { return false; }

        // Compile tessellation shader set (VS/HS/DS/GS)
        ComPtr<ID3DBlob> tessVS, tessHS, tessDS, tessGS;
        const auto tessVsPath = ResolveShaderPath(L"Tessellation_VS.hlsl");
        if (!compileShader(tessVsPath, "VSMain", "vs_5_0", tessVS)) { return false; }
        const auto tessHsPath = ResolveShaderPath(L"Tessellation_HS.hlsl");
        if (!compileShader(tessHsPath, "HSMain", "hs_5_0", tessHS)) { return false; }
        const auto tessDsPath = ResolveShaderPath(L"Tessellation_DS.hlsl");
        if (!compileShader(tessDsPath, "DSMain", "ds_5_0", tessDS)) { return false; }
        const auto tessGsPath = ResolveShaderPath(L"Tessellation_GS.hlsl");
        if (!compileShader(tessGsPath, "GSMain", "gs_5_0", tessGS)) { return false; }

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
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        hr = device.CreateGraphicsPipelineState(psoDesc, m_pipelineState);
        if (FAILED(hr)) { return false; }

        // Basic raster pipeline (provisional baseline for mesh-shader comparison)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoBasic = psoDesc;
        psoBasic.VS = { basicVertexShader->GetBufferPointer(), basicVertexShader->GetBufferSize() };
        psoBasic.PS = { basicPixelShader->GetBufferPointer(), basicPixelShader->GetBufferSize() };
        hr = device.CreateGraphicsPipelineState(psoBasic, m_basicPipelineState);
        if (FAILED(hr)) { return false; }

        // Shadow pipeline (depth-only, reuse VS; no PS)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoShadow = {};
        psoShadow.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoShadow.pRootSignature = m_rootSignature.Get();
        psoShadow.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
        psoShadow.PS = { nullptr, 0 };
        psoShadow.RasterizerState = rast;
        psoShadow.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoShadow.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoShadow.SampleMask = UINT_MAX;
        psoShadow.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoShadow.NumRenderTargets = 0;
        psoShadow.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoShadow.SampleDesc.Count = 1;

        hr = device.CreateGraphicsPipelineState(psoShadow, m_shadowPipelineState);
        if (FAILED(hr)) { return false; }

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
        rastTess.CullMode = D3D12_CULL_MODE_BACK; // optimize overfill
        psoTess.RasterizerState = rastTess;
        psoTess.BlendState = blendDesc;
        psoTess.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoTess.DepthStencilState.DepthEnable = TRUE;
        psoTess.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        psoTess.DepthStencilState.StencilEnable = FALSE;
        psoTess.SampleMask = UINT_MAX;
        psoTess.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        psoTess.NumRenderTargets = 1;
        psoTess.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoTess.SampleDesc.Count = 1;
        psoTess.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        hr = device.CreateGraphicsPipelineState(psoTess, m_tessPipelineState);
        if (FAILED(hr)) { return false; }

        // Tessellation shadow pipeline (VS+HS+DS; depth-only)
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoTessShadow = {};
        psoTessShadow.InputLayout = { inputElementDescs, _countof(inputElementDescs) };
        psoTessShadow.pRootSignature = m_rootSignature.Get();
        psoTessShadow.VS = { tessVS->GetBufferPointer(), tessVS->GetBufferSize() };
        psoTessShadow.HS = { tessHS->GetBufferPointer(), tessHS->GetBufferSize() };
        psoTessShadow.DS = { tessDS->GetBufferPointer(), tessDS->GetBufferSize() };
        psoTessShadow.GS = { nullptr, 0 };
        psoTessShadow.PS = { nullptr, 0 };
        psoTessShadow.RasterizerState = rastTess;
        psoTessShadow.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoTessShadow.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoTessShadow.SampleMask = UINT_MAX;
        psoTessShadow.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        psoTessShadow.NumRenderTargets = 0;
        psoTessShadow.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoTessShadow.SampleDesc.Count = 1;

        hr = device.CreateGraphicsPipelineState(psoTessShadow, m_tessShadowPipelineState);
        if (FAILED(hr)) { return false; }

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
        psoSkybox.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoSkybox.SampleDesc.Count = 1;
        psoSkybox.DSVFormat = DXGI_FORMAT_D32_FLOAT;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkyboxHdr = psoSkybox;
        psoSkyboxHdr.PS = { skyboxHdrPS->GetBufferPointer(), skyboxHdrPS->GetBufferSize() };
        hr = device.CreateGraphicsPipelineState(psoSkyboxHdr, m_skyboxHdrPipelineState);
        if (FAILED(hr)) { return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoSkyboxLdr = psoSkybox;
        psoSkyboxLdr.PS = { skyboxLdrPS->GetBufferPointer(), skyboxLdrPS->GetBufferSize() };
        hr = device.CreateGraphicsPipelineState(psoSkyboxLdr, m_skyboxLdrPipelineState);
        if (FAILED(hr)) { return false; }

        return true;
    }
}
