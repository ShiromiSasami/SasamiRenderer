// GpuSoftwarePipelineCache.cpp
// D3D12 compute pipeline (PSO / root signature) creation for GpuSoftwareRayTracer.
// Separated from the main translation unit to keep PSO authoring readable.
#define NOMINMAX
#include "Renderer/RayTracing/GpuSoftwareRayTracer.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl.h>
#include "d3dx12.h"

using Microsoft::WRL::ComPtr;

namespace SasamiRenderer
{
    namespace
    {
        // ---- DXC runtime loader ----
        HRESULT CreateDxcInstance(REFCLSID clsid, REFIID iid, LPVOID* out)
        {
            static HMODULE mod = []() -> HMODULE { return LoadLibraryW(L"dxcompiler.dll"); }();
            static auto fn = mod ? reinterpret_cast<HRESULT(WINAPI*)(REFCLSID,REFIID,LPVOID*)>(
                GetProcAddress(mod,"DxcCreateInstance")) : nullptr;
            return fn ? fn(clsid,iid,out) : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        }

        // ---- Project root / shader path resolution ----
        std::filesystem::path GetExeDir()
        {
            wchar_t buf[MAX_PATH]{};
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            return std::filesystem::path(buf).parent_path();
        }

        std::filesystem::path FindProjectRoot(std::filesystem::path dir)
        {
            for (int depth=0; depth<16; ++depth) {
                if (std::filesystem::exists(dir/"Source"/"Renderer"/"Shaders")) return dir;
                auto p = dir.parent_path();
                if (p.empty()||p==dir) break;
                dir = p;
            }
            return {};
        }

        std::filesystem::path GetShaderRoot()
        {
            static const std::filesystem::path root = [](){
                auto pr = FindProjectRoot(GetExeDir());
                return pr.empty() ? std::filesystem::path(L"Source/Renderer/Shaders")
                                  : pr / L"Source" / L"Renderer" / L"Shaders";
            }();
            return root;
        }

        // ---- Compile a compute shader via DXC ----
        bool CompileComputeShader(const wchar_t* relPath, const char* entry,
                                  ComPtr<ID3DBlob>& outBlob)
        {
            const std::filesystem::path srcPath = GetShaderRoot() / relPath;
            const std::filesystem::path incPath = GetShaderRoot();

            ComPtr<IDxcUtils> utils; ComPtr<IDxcCompiler3> compiler;
            if (FAILED(CreateDxcInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) return false;
            if (FAILED(CreateDxcInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) return false;

            ComPtr<IDxcBlobEncoding> src;
            if (FAILED(utils->LoadFile(srcPath.c_str(), nullptr, &src))) {
                OutputDebugStringA(("GpuSWRT: failed to load shader: " + srcPath.string() + "\n").c_str());
                return false;
            }

            ComPtr<IDxcIncludeHandler> incHandler;
            utils->CreateDefaultIncludeHandler(&incHandler);

            const std::wstring srcW  = srcPath.native();
            const std::wstring entW  = [&](){ std::wstring w; for (char c:std::string(entry)) w+=c; return w; }();
            const std::wstring incW  = incPath.native();

            std::vector<LPCWSTR> args{
                srcW.c_str(),
                L"-E", entW.c_str(),
                L"-T", L"cs_6_6",
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
            if (FAILED(compiler->Compile(&buf, args.data(), (UINT32)args.size(), incHandler.Get(), IID_PPV_ARGS(&result)))) {
                return false;
            }
            ComPtr<IDxcBlobUtf8> errors;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
                errors && errors->GetStringLength()>0) {
                OutputDebugStringA(errors->GetStringPointer());
            }
            HRESULT status=S_OK;
            if (FAILED(result->GetStatus(&status))||FAILED(status)) return false;

            ComPtr<IDxcBlob> obj;
            if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr))||!obj) return false;

            if (FAILED(D3DCreateBlob(obj->GetBufferSize(), outBlob.ReleaseAndGetAddressOf()))) return false;
            memcpy(outBlob->GetBufferPointer(), obj->GetBufferPointer(), obj->GetBufferSize());
            return true;
        }

    } // anonymous namespace

    // =========================================================================
    // CreatePipelines — legacy shadow / reflection / AO + temporal + A-Trous
    // =========================================================================

    bool GpuSoftwareRayTracer::CreatePipelines(IRHIDevice& device)
    {
        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // ---- Root signature ----
        // [0]: Root CBV  (b0) inline
        // [1]: Descriptor table: 6 SRVs  (t0-t5) – BVH data
        // [2]: Descriptor table: 1 UAV   (u0)     – output texture
        // [3]: Descriptor table: 3 SRVs  (t6-t8)  – G-Buffer (Normal, Material, Albedo)
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors                    = kSrvCount;
        srvRange.BaseShaderRegister                = 0;
        srvRange.RegisterSpace                     = 0;
        srvRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors                    = 1;
        uavRange.BaseShaderRegister                = 0;
        uavRange.RegisterSpace                     = 0;
        uavRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_DESCRIPTOR_RANGE gbufferRange{};
        gbufferRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        gbufferRange.NumDescriptors                    = kGBufferSrvCount;
        gbufferRange.BaseShaderRegister                = kSrvCount; // t6
        gbufferRange.RegisterSpace                     = 0;
        gbufferRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[6]{};
        // [0] Root CBV
        params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace  = 0;
        params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        // [1] SRV table (BVH)
        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &srvRange;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        // [2] UAV table
        params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges   = &uavRange;
        params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        // [3] G-Buffer SRV table (t6-t9)
        params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable.NumDescriptorRanges = 1;
        params[3].DescriptorTable.pDescriptorRanges   = &gbufferRange;
        params[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        // [4] Point light structured buffer (t12), used by legacy reflection NEE.
        params[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[4].Descriptor.ShaderRegister = 12;
        params[4].Descriptor.RegisterSpace  = 0;
        params[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        // [5] Spot light structured buffer (t13), used by legacy reflection NEE.
        params[5].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[5].Descriptor.ShaderRegister = 13;
        params[5].Descriptor.RegisterSpace  = 0;
        params[5].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC staticSampler{};
        staticSampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSampler.ShaderRegister   = 0;
        staticSampler.RegisterSpace    = 0;
        staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        staticSampler.MaxLOD           = D3D12_FLOAT32_MAX;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters     = 6;
        rsDesc.pParameters       = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers   = &staticSampler;
        rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> rsBlob, rsError;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                               rsBlob.ReleaseAndGetAddressOf(),
                                               rsError.ReleaseAndGetAddressOf()))) {
            if (rsError) OutputDebugStringA((char*)rsError->GetBufferPointer());
            return false;
        }
        if (FAILED(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                            IID_PPV_ARGS(m_rootSignature.ReleaseAndGetAddressOf())))) {
            return false;
        }

        // ---- Shadow PSO ----
        {
            ComPtr<ID3DBlob> cs;
            if (!CompileComputeShader(L"SWRT/SWRT_Shadow_CS.hlsl", "CS_Shadow", cs)) {
                OutputDebugStringA("GpuSWRT: failed to compile SWRT_Shadow_CS.hlsl\n");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = m_rootSignature.Get();
            pso.CS             = { cs->GetBufferPointer(), cs->GetBufferSize() };
            if (FAILED(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(m_shadowPso.ReleaseAndGetAddressOf())))) {
                return false;
            }
        }

        // ---- Reflection PSO ----
        {
            ComPtr<ID3DBlob> cs;
            if (!CompileComputeShader(L"SWRT/SWRT_Reflection_CS.hlsl", "CS_Reflection", cs)) {
                OutputDebugStringA("GpuSWRT: failed to compile SWRT_Reflection_CS.hlsl\n");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = m_rootSignature.Get();
            pso.CS             = { cs->GetBufferPointer(), cs->GetBufferSize() };
            if (FAILED(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(m_reflectionPso.ReleaseAndGetAddressOf())))) {
                return false;
            }
        }

        // ---- AO PSO ----
        {
            ComPtr<ID3DBlob> cs;
            if (!CompileComputeShader(L"SWRT/SWRT_AO_CS.hlsl", "CS_AO", cs)) {
                OutputDebugStringA("GpuSWRT: failed to compile SWRT_AO_CS.hlsl\n");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
            pso.pRootSignature = m_rootSignature.Get();
            pso.CS             = { cs->GetBufferPointer(), cs->GetBufferSize() };
            if (FAILED(dev->CreateComputePipelineState(&pso, IID_PPV_ARGS(m_aoPso.ReleaseAndGetAddressOf())))) {
                return false;
            }
        }

        // ---- Temporal EMA root signature ----
        // Root[0]: 7 root constants  b0  (alpha_bits, size, validation flags, gbuffer size)
        // Root[1]: Descriptor table  t0-t5  (current/history reflection + surface/material metadata)
        // Root[2]: Descriptor table  u0-u2  (history + surface/material metadata write UAVs)
        // Root[3]: Reprojection CBV  b1     (current invVP + previous VP)
        {
            D3D12_DESCRIPTOR_RANGE srvRange2{};
            srvRange2.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRange2.NumDescriptors                    = 6;
            srvRange2.BaseShaderRegister                = 0;
            srvRange2.RegisterSpace                     = 0;
            srvRange2.OffsetInDescriptorsFromTableStart = 0;

            D3D12_DESCRIPTOR_RANGE uavRange1{};
            uavRange1.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            uavRange1.NumDescriptors                    = 3;
            uavRange1.BaseShaderRegister                = 0;
            uavRange1.RegisterSpace                     = 0;
            uavRange1.OffsetInDescriptorsFromTableStart = 0;

            D3D12_ROOT_PARAMETER tparams[4]{};
            tparams[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            tparams[0].Constants.ShaderRegister = 0;
            tparams[0].Constants.RegisterSpace  = 0;
            tparams[0].Constants.Num32BitValues = 7;
            tparams[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

            tparams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            tparams[1].DescriptorTable.NumDescriptorRanges = 1;
            tparams[1].DescriptorTable.pDescriptorRanges   = &srvRange2;
            tparams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

            tparams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            tparams[2].DescriptorTable.NumDescriptorRanges = 1;
            tparams[2].DescriptorTable.pDescriptorRanges   = &uavRange1;
            tparams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

            tparams[3].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
            tparams[3].Descriptor.ShaderRegister = 1;
            tparams[3].Descriptor.RegisterSpace  = 0;
            tparams[3].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC trsDesc{};
            trsDesc.NumParameters = 4;
            trsDesc.pParameters   = tparams;
            trsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

            ComPtr<ID3DBlob> trsBlob, trsError;
            if (FAILED(D3D12SerializeRootSignature(&trsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                   trsBlob.ReleaseAndGetAddressOf(),
                                                   trsError.ReleaseAndGetAddressOf()))) {
                if (trsError) OutputDebugStringA((char*)trsError->GetBufferPointer());
                return false;
            }
            if (FAILED(dev->CreateRootSignature(0,
                    trsBlob->GetBufferPointer(), trsBlob->GetBufferSize(),
                    IID_PPV_ARGS(m_reflTemporalRootSignature.ReleaseAndGetAddressOf())))) {
                return false;
            }

            // ---- Temporal EMA PSO ----
            ComPtr<ID3DBlob> tcs;
            if (!CompileComputeShader(L"SWRT/SWRT_Reflection_Temporal_CS.hlsl",
                                      "CS_ReflectionTemporal", tcs)) {
                OutputDebugStringA("GpuSWRT: failed to compile SWRT_Reflection_Temporal_CS.hlsl\n");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC tpso{};
            tpso.pRootSignature = m_reflTemporalRootSignature.Get();
            tpso.CS             = { tcs->GetBufferPointer(), tcs->GetBufferSize() };
            if (FAILED(dev->CreateComputePipelineState(
                    &tpso, IID_PPV_ARGS(m_reflTemporalPso.ReleaseAndGetAddressOf())))) {
                return false;
            }
        }

        // ---- A-Trous root signature ----
        // Root[0]: 6 root constants b0  (step width, reflection size, phi depth, GBuffer size)
        // Root[1]: Descriptor table t0-t2 (source reflection + normal/depth + material)
        // Root[2]: Descriptor table u0    (filtered reflection output)
        {
            D3D12_DESCRIPTOR_RANGE atrousSrvRange{};
            atrousSrvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            atrousSrvRange.NumDescriptors                    = 3;
            atrousSrvRange.BaseShaderRegister                = 0;
            atrousSrvRange.RegisterSpace                     = 0;
            atrousSrvRange.OffsetInDescriptorsFromTableStart = 0;

            D3D12_DESCRIPTOR_RANGE atrousUavRange{};
            atrousUavRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            atrousUavRange.NumDescriptors                    = 1;
            atrousUavRange.BaseShaderRegister                = 0;
            atrousUavRange.RegisterSpace                     = 0;
            atrousUavRange.OffsetInDescriptorsFromTableStart = 0;

            D3D12_ROOT_PARAMETER aparams[3]{};
            aparams[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            aparams[0].Constants.ShaderRegister = 0;
            aparams[0].Constants.RegisterSpace  = 0;
            aparams[0].Constants.Num32BitValues = 6;
            aparams[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

            aparams[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            aparams[1].DescriptorTable.NumDescriptorRanges = 1;
            aparams[1].DescriptorTable.pDescriptorRanges   = &atrousSrvRange;
            aparams[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

            aparams[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            aparams[2].DescriptorTable.NumDescriptorRanges = 1;
            aparams[2].DescriptorTable.pDescriptorRanges   = &atrousUavRange;
            aparams[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC arsDesc{};
            arsDesc.NumParameters = 3;
            arsDesc.pParameters   = aparams;
            arsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

            ComPtr<ID3DBlob> arsBlob, arsError;
            if (FAILED(D3D12SerializeRootSignature(&arsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                   arsBlob.ReleaseAndGetAddressOf(),
                                                   arsError.ReleaseAndGetAddressOf()))) {
                if (arsError) OutputDebugStringA((char*)arsError->GetBufferPointer());
                return false;
            }
            if (FAILED(dev->CreateRootSignature(0,
                    arsBlob->GetBufferPointer(), arsBlob->GetBufferSize(),
                    IID_PPV_ARGS(m_reflAtrousRootSignature.ReleaseAndGetAddressOf())))) {
                return false;
            }

            ComPtr<ID3DBlob> acs;
            if (!CompileComputeShader(L"SWRT/SWRT_Reflection_ATrous_CS.hlsl",
                                      "CS_ReflectionATrous", acs)) {
                OutputDebugStringA("GpuSWRT: failed to compile SWRT_Reflection_ATrous_CS.hlsl\n");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC apso{};
            apso.pRootSignature = m_reflAtrousRootSignature.Get();
            apso.CS             = { acs->GetBufferPointer(), acs->GetBufferSize() };
            if (FAILED(dev->CreateComputePipelineState(
                    &apso, IID_PPV_ARGS(m_reflAtrousPso.ReleaseAndGetAddressOf())))) {
                return false;
            }
        }

        return true;
    }

    // =========================================================================
    // CreateReSTIRPipelines
    // =========================================================================

    bool GpuSoftwareRayTracer::CreateReSTIRPipelines(IRHIDevice& device)
    {
        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // ---- ReSTIR root signature ----
        // [0]: Root CBV  (b0) – ReSTIRFrameConstants
        // [1]: Descriptor table – 6 SRVs (t0-t5) BVH
        // [2]: Descriptor table – 4 SRVs (t6-t9) scratch
        // [3]: Descriptor table – 2 UAVs (u0-u1) scratch
        // [4]: Root SRV  (t12) point lights inline
        // [5]: Root SRV  (t13) spot lights inline
        // [6]: Root SRV  (t14) reservoir input inline
        // [7]: Root UAV  (u3)  reservoir output inline
        D3D12_DESCRIPTOR_RANGE bvhRange{};
        bvhRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        bvhRange.NumDescriptors     = kSrvCount;
        bvhRange.BaseShaderRegister = 0;
        bvhRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_DESCRIPTOR_RANGE scratchSrvRange{};
        scratchSrvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        scratchSrvRange.NumDescriptors     = kScratchSrvCount;
        scratchSrvRange.BaseShaderRegister = 6;
        scratchSrvRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_DESCRIPTOR_RANGE scratchUavRange{};
        scratchUavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        scratchUavRange.NumDescriptors     = kScratchUavCount;
        scratchUavRange.BaseShaderRegister = 0;
        scratchUavRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[9]{};
        // [0] Root CBV
        params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        // [1] BVH SRV table
        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &bvhRange;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        // [2] Scratch SRV table (t6-t9)
        params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges   = &scratchSrvRange;
        params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        // [3] Scratch UAV table (u0-u1)
        params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable.NumDescriptorRanges = 1;
        params[3].DescriptorTable.pDescriptorRanges   = &scratchUavRange;
        params[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;
        // [4] Point lights SRV inline (t12)
        params[4].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[4].Descriptor.ShaderRegister = 12;
        params[4].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        // [5] Spot lights SRV inline (t13)
        params[5].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[5].Descriptor.ShaderRegister = 13;
        params[5].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        // [6] Reservoir input SRV inline (t14)
        params[6].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[6].Descriptor.ShaderRegister = 14;
        params[6].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        // [7] Reservoir output UAV inline (u3)
        params[7].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[7].Descriptor.ShaderRegister = 3;
        params[7].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        // [8] Prev-temporal reservoir SRV inline (t15)
        params[8].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[8].Descriptor.ShaderRegister = 15;
        params[8].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 9;
        rsDesc.pParameters   = params;
        rsDesc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> rsBlob, rsError;
        if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                               rsBlob.ReleaseAndGetAddressOf(),
                                               rsError.ReleaseAndGetAddressOf()))) {
            if (rsError) OutputDebugStringA((char*)rsError->GetBufferPointer());
            return false;
        }
        if (FAILED(dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                            IID_PPV_ARGS(m_restirRootSignature.ReleaseAndGetAddressOf())))) {
            return false;
        }

        // Helper: compile + create one compute PSO
        auto MakePso = [&](const wchar_t* path, const char* entry,
                           ComPtr<ID3D12PipelineState>& out) -> bool {
            ComPtr<ID3DBlob> cs;
            if (!CompileComputeShader(path, entry, cs)) {
                OutputDebugStringA(("GpuSWRT ReSTIR: failed to compile " + std::string(entry) + "\n").c_str());
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC psd{};
            psd.pRootSignature = m_restirRootSignature.Get();
            psd.CS             = { cs->GetBufferPointer(), cs->GetBufferSize() };
            return SUCCEEDED(dev->CreateComputePipelineState(&psd,
                IID_PPV_ARGS(out.ReleaseAndGetAddressOf())));
        };

        if (!MakePso(L"SWRT/SWRT_ReSTIR_Initial_CS.hlsl",  "CS_ReSTIR_Initial",  m_restirInitialPso))  return false;
        if (!MakePso(L"SWRT/SWRT_ReSTIR_Temporal_CS.hlsl", "CS_ReSTIR_Temporal", m_restirTemporalPso)) return false;
        if (!MakePso(L"SWRT/SWRT_ReSTIR_Spatial_CS.hlsl",  "CS_ReSTIR_Spatial",  m_restirSpatialPso))  return false;
        if (!MakePso(L"SWRT/SWRT_ReSTIR_Shade_CS.hlsl",    "CS_ReSTIR_Shade",    m_restirShadePso))    return false;
        if (!MakePso(L"SWRT/SWRT_NRD_Pack_CS.hlsl",        "CS_NRD_Pack",        m_nrdPackPso))        return false;
        if (!MakePso(L"SWRT/SWRT_Shadow_ReSTIR_CS.hlsl",   "CS_Shadow_ReSTIR",   m_shadowReSTIRPso))   return false;
        if (!MakePso(L"SWRT/SWRT_Denoise_ATrous_CS.hlsl",  "CS_Denoise_ATrous",  m_restirATrousPso))   return false;

        m_restirPipelineReady = true;
        return true;
    }

} // namespace SasamiRenderer
