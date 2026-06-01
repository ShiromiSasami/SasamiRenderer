#define NOMINMAX
#include "Renderer/Passes/DebugProbeGridRenderNode.h"

#include "Renderer/Core/RenderGraph.h"
#include "Foundation/Math/MathUtil.h"
#include "Foundation/Tools/DebugOutput.h"
#include "d3dx12.h"

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

namespace SasamiRenderer
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;

        // ---- Minimal DXC helper (mirrors GpuSoftwareRayTracer) ----
        HRESULT CreateDxcInstance(REFCLSID clsid, REFIID iid, LPVOID* out)
        {
            static HMODULE mod = LoadLibraryW(L"dxcompiler.dll");
            static auto fn = mod
                ? reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>(
                      GetProcAddress(mod, "DxcCreateInstance"))
                : nullptr;
            return fn ? fn(clsid, iid, out) : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        }

        std::filesystem::path GetExeDir()
        {
            wchar_t buf[MAX_PATH]{};
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            return std::filesystem::path(buf).parent_path();
        }

        std::filesystem::path FindProjectRoot(std::filesystem::path dir)
        {
            for (int depth = 0; depth < 16; ++depth) {
                if (std::filesystem::exists(dir / "Source" / "Renderer" / "Shaders")) return dir;
                auto p = dir.parent_path();
                if (p.empty() || p == dir) break;
                dir = p;
            }
            return {};
        }

        std::filesystem::path GetShaderRoot()
        {
            static const std::filesystem::path root = []() {
                auto pr = FindProjectRoot(GetExeDir());
                return pr.empty()
                    ? std::filesystem::path(L"Source/Renderer/Shaders")
                    : pr / L"Source" / L"Renderer" / L"Shaders";
            }();
            return root;
        }

        bool CompileShader(const wchar_t* relPath,
                           const char* entry,
                           const char* profile,
                           ComPtr<ID3DBlob>& outBlob)
        {
            const std::filesystem::path srcPath = GetShaderRoot() / relPath;
            const std::filesystem::path incPath = GetShaderRoot();

            ComPtr<IDxcUtils>     utils;
            ComPtr<IDxcCompiler3> compiler;
            if (FAILED(CreateDxcInstance(CLSID_DxcUtils,     IID_PPV_ARGS(&utils))))    return false;
            if (FAILED(CreateDxcInstance(CLSID_DxcCompiler,  IID_PPV_ARGS(&compiler)))) return false;

            ComPtr<IDxcBlobEncoding> src;
            if (FAILED(utils->LoadFile(srcPath.c_str(), nullptr, &src))) {
                OutputDebugStringA(("DebugProbeGrid: failed to load shader: " + srcPath.string() + "\n").c_str());
                return false;
            }

            ComPtr<IDxcIncludeHandler> incHandler;
            utils->CreateDefaultIncludeHandler(&incHandler);

            const std::wstring srcW = srcPath.native();
            const std::wstring incW = incPath.native();
            auto toW = [](const char* s) {
                std::wstring w; for (const char* p = s; *p; ++p) w += (wchar_t)*p; return w;
            };
            const std::wstring entW  = toW(entry);
            const std::wstring profW = toW(profile);

            std::vector<LPCWSTR> args{
                srcW.c_str(), L"-E", entW.c_str(),
                L"-T", profW.c_str(),
                L"-I", incW.c_str(),
                L"-HV", L"2021",
                L"-WX",
            };
#if defined(_DEBUG)
            args.push_back(L"-Zi"); args.push_back(L"-Od");
#else
            args.push_back(L"-O3");
#endif

            DxcBuffer buf{ src->GetBufferPointer(), src->GetBufferSize(), DXC_CP_ACP };
            ComPtr<IDxcResult> result;
            if (FAILED(compiler->Compile(&buf, args.data(), (UINT32)args.size(), incHandler.Get(), IID_PPV_ARGS(&result))))
                return false;

            ComPtr<IDxcBlobUtf8> errors;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
                errors && errors->GetStringLength() > 0) {
                OutputDebugStringA(errors->GetStringPointer());
            }

            HRESULT hr = S_OK;
            result->GetStatus(&hr);
            if (FAILED(hr)) {
                OutputDebugStringA(("DebugProbeGrid: shader compilation failed: " + srcPath.string() + "\n").c_str());
                return false;
            }

            ComPtr<IDxcBlob> code;
            result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&code), nullptr);
            if (!code) return false;

            ID3DBlob* blob = nullptr;
            D3DCreateBlob(code->GetBufferSize(), &blob);
            std::memcpy(blob->GetBufferPointer(), code->GetBufferPointer(), code->GetBufferSize());
            outBlob.Attach(blob);
            return true;
        }

        // UV sphere geometry.  Vertices are unit-radius (scale applied in VS via probeRadius).
        struct SphereVertex { float pos[3]; float normal[3]; };

        std::vector<SphereVertex> GenerateUVSphere(int rings, int segments)
        {
            std::vector<SphereVertex> verts;
            verts.reserve(static_cast<size_t>(segments * 3 * 2 + (rings - 2) * segments * 6));

            auto mkVert = [](float phi, float theta) -> SphereVertex {
                const float sp = std::sin(phi), cp = std::cos(phi);
                const float st = std::sin(theta), ct = std::cos(theta);
                SphereVertex sv{};
                sv.normal[0] = sp * ct; sv.normal[1] = cp; sv.normal[2] = sp * st;
                sv.pos[0] = sv.normal[0]; sv.pos[1] = sv.normal[1]; sv.pos[2] = sv.normal[2];
                return sv;
            };

            // Top cap
            const float phi1 = kPi / static_cast<float>(rings);
            for (int j = 0; j < segments; ++j) {
                const float t0 = (static_cast<float>(j)     / segments) * 2.0f * kPi;
                const float t1 = (static_cast<float>(j + 1) / segments) * 2.0f * kPi;
                verts.push_back(mkVert(0.0f, 0.0f));
                verts.push_back(mkVert(phi1, t0));
                verts.push_back(mkVert(phi1, t1));
            }

            // Middle bands
            for (int i = 1; i < rings - 1; ++i) {
                const float p0 = static_cast<float>(i)     / rings * kPi;
                const float p1 = static_cast<float>(i + 1) / rings * kPi;
                for (int j = 0; j < segments; ++j) {
                    const float t0 = (static_cast<float>(j)     / segments) * 2.0f * kPi;
                    const float t1 = (static_cast<float>(j + 1) / segments) * 2.0f * kPi;
                    // Upper-left triangle
                    verts.push_back(mkVert(p0, t0));
                    verts.push_back(mkVert(p1, t1));
                    verts.push_back(mkVert(p0, t1));
                    // Lower-right triangle
                    verts.push_back(mkVert(p0, t0));
                    verts.push_back(mkVert(p1, t0));
                    verts.push_back(mkVert(p1, t1));
                }
            }

            // Bottom cap
            const float botPhi = static_cast<float>(rings - 1) / rings * kPi;
            for (int j = 0; j < segments; ++j) {
                const float t0 = (static_cast<float>(j)     / segments) * 2.0f * kPi;
                const float t1 = (static_cast<float>(j + 1) / segments) * 2.0f * kPi;
                verts.push_back(mkVert(kPi, 0.0f));
                verts.push_back(mkVert(botPhi, t1));
                verts.push_back(mkVert(botPhi, t0));
            }

            return verts;
        }

    } // anonymous namespace

    // =========================================================================
    // IRenderNode interface
    // =========================================================================

    void DebugProbeGridRenderNode::BuildRequirements(RenderNodeRequirementBuilder& builder) const
    {
        builder.RequireRhiGraphicsBase();
        builder.RequireCameraPV();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
    }

    void DebugProbeGridRenderNode::Setup(RenderGraphBuilder& builder) const
    {
        builder.Write("SceneColor");
        builder.UseColorTarget("SceneColor");
        // Depth-test against the scene so probes embed into opaque geometry while remaining unlit.
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool DebugProbeGridRenderNode::Execute(const RenderNodeContextView& context) const
    {
        if (!m_enabled || !m_initialized || !m_probeGrid) {
            return true;
        }

        // Always flush the probe grid CB so VS/PS have the current origin/spacing/count
        // regardless of initialization order.  m_cbMapped is mutable so this is safe from
        // a const Execute().
        m_probeGrid->FlushGridCB();
        if (!context.IsSatisfied()) {
            DebugLog("DebugProbeGridRenderNode::Execute: runtime context is invalid.\n");
            return false;
        }
        if (!m_probeGrid->IsInitialized()) {
            return true;
        }
        const uint32_t probeCount = m_probeGrid->GetTotalProbeCount();
        if (probeCount == 0 || m_sphereVertexCount == 0) {
            return true;
        }

        const RenderNodeFrameInputs& inputs = context.Inputs();
        auto* enc = inputs.execution.commandEncoder;

        // --- Root signature / PSO (ComPtr raw pointers converted to opaque handles) ---
        enc->SetGraphicsPipelineLayout(RhiPipelineLayoutHandle{ reinterpret_cast<uint64_t>(m_rootSig.Get()) });
        enc->SetGraphicsPipeline(RhiPipelineHandle{ reinterpret_cast<uint64_t>(m_pso.Get()) });

        // --- Viewport / scissor ---
        enc->SetViewports(reinterpret_cast<const RhiViewport*>(inputs.execution.viewport), 1);
        enc->SetScissors(reinterpret_cast<const RhiRect*>(inputs.execution.scissorRect), 1);
        enc->SetPrimitiveTopology(RhiPrimitiveTopology::TriangleList);

        // --- [0] Camera CB (b0): viewProj + probeRadius in extra0.x ---
        const float* vp = inputs.camera.pv;
        const float identity[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
        const float extra0[4] = { m_probeRadius, 0.0f, 0.0f, 0.0f };
        const float extra1[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float extra2[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        const RhiGpuAddress cameraCbGpu =
            inputs.execution.frameCoordinator->PushCameraCB(*inputs.execution.frame,
                                                            vp,
                                                   identity,
                                                   extra0,
                                                   extra1,
                                                   extra2);
        if (cameraCbGpu == 0) {
            DebugLog("DebugProbeGridRenderNode::Execute: PushCameraCB returned 0.\n");
            return false;
        }
        enc->SetGraphicsConstantBufferView(0, cameraCbGpu);

        // --- [1] GI Probe Grid CB (b2) ---
        const RhiGpuAddress probeCbGpu = m_probeGrid->GetProbeGridCbGpuAddress();
        if (probeCbGpu == 0) return true;
        enc->SetGraphicsConstantBufferView(1, probeCbGpu);

        // --- [2] Probe SH data inline SRV (t10) ---
        const RhiGpuAddress probeVA = m_probeGrid->GetProbeDataGpuVA();
        if (probeVA == 0) return true;
        enc->SetGraphicsShaderResourceView(2, probeVA);

        // --- Vertex buffer + instanced draw ---
        const RhiVertexBufferView rhiVbv{ m_sphereVBV.BufferLocation, m_sphereVBV.StrideInBytes, m_sphereVBV.SizeInBytes };
        enc->SetVertexBuffers(0, 1, &rhiVbv);
        enc->Draw({ m_sphereVertexCount, probeCount, 0u, 0u });

        return true;
    }

    // =========================================================================
    // Initialize
    // =========================================================================

    bool DebugProbeGridRenderNode::Initialize(IRHIDevice& device)
    {
        if (m_initialized) return true;

        if (!CreatePipeline(device)) {
            DebugLog("DebugProbeGridRenderNode::Initialize: CreatePipeline failed.\n");
            return false;
        }
        if (!CreateSphereMesh(device)) {
            DebugLog("DebugProbeGridRenderNode::Initialize: CreateSphereMesh failed.\n");
            return false;
        }

        m_initialized = true;
        return true;
    }

    // =========================================================================
    // CreatePipeline
    // =========================================================================

    bool DebugProbeGridRenderNode::CreatePipeline(IRHIDevice& device)
    {
        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // --- Root signature ---
        // [0] Root CBV b0 : Camera CB (viewProj + probeRadius) — ALL
        // [1] Root CBV b2 : GI Probe Grid CB                  — ALL
        // [2] Root SRV t10: Probe SH data                     — PIXEL

        D3D12_ROOT_PARAMETER params[3]{};

        params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0; // b0
        params[0].Descriptor.RegisterSpace  = 0;
        params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor.ShaderRegister = 2; // b2 (matches GI_Common.hlsli)
        params[1].Descriptor.RegisterSpace  = 0;
        params[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        params[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor.ShaderRegister = 10; // t10 (matches GI_Common.hlsli)
        params[2].Descriptor.RegisterSpace  = 0;
        params[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 3;
        rsDesc.pParameters   = params;
        rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> rsBlob, rsErr;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  rsBlob.GetAddressOf(), rsErr.GetAddressOf());
        if (FAILED(hr)) {
            if (rsErr) OutputDebugStringA(static_cast<const char*>(rsErr->GetBufferPointer()));
            DebugLog("DebugProbeGridRenderNode: D3D12SerializeRootSignature failed.\n");
            return false;
        }
        hr = dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                       IID_PPV_ARGS(m_rootSig.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            DebugLog("DebugProbeGridRenderNode: CreateRootSignature failed.\n");
            return false;
        }

        // --- Compile VS / PS ---
        ComPtr<ID3DBlob> vsBlob, psBlob;
        if (!CompileShader(L"Debug/DebugProbeGrid_VS.hlsl", "VSMain", "vs_6_6", vsBlob)) {
            DebugLog("DebugProbeGridRenderNode: VS compilation failed.\n");
            return false;
        }
        if (!CompileShader(L"Debug/DebugProbeGrid_PS.hlsl", "PSMain", "ps_6_6", psBlob)) {
            DebugLog("DebugProbeGridRenderNode: PS compilation failed.\n");
            return false;
        }

        // --- Input layout: POSITION (R32G32B32), NORMAL (R32G32B32) ---
        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        // --- PSO ---
        auto rast = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        rast.CullMode = D3D12_CULL_MODE_BACK;

        auto depth = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        depth.DepthEnable    = TRUE;
        depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        depth.StencilEnable  = FALSE;

        auto blend = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        blend.RenderTarget[0].BlendEnable           = FALSE;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.InputLayout             = { inputLayout, _countof(inputLayout) };
        psoDesc.pRootSignature          = m_rootSig.Get();
        psoDesc.VS                      = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        psoDesc.PS                      = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        psoDesc.RasterizerState         = rast;
        psoDesc.BlendState              = blend;
        psoDesc.DepthStencilState       = depth;
        psoDesc.SampleMask              = UINT_MAX;
        psoDesc.PrimitiveTopologyType   = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets        = 1;
        psoDesc.RTVFormats[0]           = DXGI_FORMAT_R8G8B8A8_UNORM; // SceneColor
        psoDesc.DSVFormat               = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc.Count        = 1;

        hr = dev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_pso.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            DebugLog("DebugProbeGridRenderNode: CreateGraphicsPipelineState failed.\n");
            return false;
        }

        return true;
    }

    // =========================================================================
    // CreateSphereMesh
    // =========================================================================

    bool DebugProbeGridRenderNode::CreateSphereMesh(IRHIDevice& device)
    {
        const std::vector<SphereVertex> verts = GenerateUVSphere(8, 12);
        if (verts.empty()) return false;

        const UINT64 vbBytes = static_cast<UINT64>(verts.size() * sizeof(SphereVertex));

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC vbDesc{};
        vbDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        vbDesc.Width            = vbBytes;
        vbDesc.Height           = 1;
        vbDesc.DepthOrArraySize = 1;
        vbDesc.MipLevels        = 1;
        vbDesc.SampleDesc.Count = 1;
        vbDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = device.CreateCommittedResource(&heapProps,
                                                     D3D12_HEAP_FLAG_NONE,
                                                     &vbDesc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ,
                                                     nullptr,
                                                     m_sphereVB);
        if (FAILED(hr)) {
            DebugLog("DebugProbeGridRenderNode: CreateCommittedResource for sphere VB failed.\n");
            return false;
        }

        void* mapped = nullptr;
        hr = m_sphereVB->Map(0, nullptr, &mapped);
        if (FAILED(hr) || !mapped) {
            m_sphereVB.Reset();
            DebugLog("DebugProbeGridRenderNode: sphere VB Map failed.\n");
            return false;
        }
        std::memcpy(mapped, verts.data(), static_cast<size_t>(vbBytes));
        m_sphereVB->Unmap(0, nullptr);

        m_sphereVBV.BufferLocation = m_sphereVB->GetGPUVirtualAddress();
        m_sphereVBV.StrideInBytes  = sizeof(SphereVertex);
        m_sphereVBV.SizeInBytes    = static_cast<UINT>(vbBytes);
        m_sphereVertexCount        = static_cast<uint32_t>(verts.size());

        return true;
    }

} // namespace SasamiRenderer
