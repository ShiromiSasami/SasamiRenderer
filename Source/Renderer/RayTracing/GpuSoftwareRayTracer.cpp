#define NOMINMAX
#include "Renderer/RayTracing/GpuSoftwareRayTracer.h"

#include "Renderer/Scene/RenderLightProxy.h"
#include "Renderer/Scene/LightSystem.h"
#include "Renderer/Utilities/ResourceUploadUtility.h"

#include "Foundation/Math/MathUtil.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <vector>

#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl.h>

#include "d3dx12.h"

// ---- NRD / NRI integration (included in exactly this one TU) ----
#include "NRI.h"
#include "NRIDeviceCreation.h"
#include "Extensions/NRIWrapperD3D12.h"
#include "Extensions/NRIHelper.h"
#include "NRD.h"
#include "NRDIntegration.h"
#include "NRDIntegration.hpp"

using Microsoft::WRL::ComPtr;

namespace SasamiRenderer
{
    // -------------------------------------------------------------------------
    // Internal helpers (anonymous namespace)
    // -------------------------------------------------------------------------
    namespace
    {
        // ---- Lightweight math types ----
        struct F2 { float x = 0.f, y = 0.f; };
        struct F3 { float x = 0.f, y = 0.f, z = 0.f; };

        F3 operator+(const F3& a, const F3& b) { return { a.x+b.x, a.y+b.y, a.z+b.z }; }
        F3 operator-(const F3& a, const F3& b) { return { a.x-b.x, a.y-b.y, a.z-b.z }; }
        F3 operator*(const F3& a, float s)      { return { a.x*s,   a.y*s,   a.z*s   }; }
        F3 operator/(const F3& a, float s)      { return { a.x/s,   a.y/s,   a.z/s   }; }
        F3 fmin3(const F3& a, const F3& b) { return { std::min(a.x,b.x), std::min(a.y,b.y), std::min(a.z,b.z) }; }
        F3 fmax3(const F3& a, const F3& b) { return { std::max(a.x,b.x), std::max(a.y,b.y), std::max(a.z,b.z) }; }

        float GetAxis(const F3& v, int a) { return a==0?v.x : a==1?v.y : v.z; }

        float SurfaceArea(const F3& mn, const F3& mx)
        {
            F3 e{ std::max(0.f, mx.x-mn.x), std::max(0.f, mx.y-mn.y), std::max(0.f, mx.z-mn.z) };
            return 2.f * (e.x*e.y + e.y*e.z + e.z*e.x);
        }

        void StoreBounds(const F3& mn, const F3& mx, float outMn[3], float outMx[3])
        {
            outMn[0]=mn.x; outMn[1]=mn.y; outMn[2]=mn.z;
            outMx[0]=mx.x; outMx[1]=mx.y; outMx[2]=mx.z;
        }

        // ---- SAH BVH constants ----
        constexpr uint32_t kBvhLeafSize  = 4u;
        constexpr uint32_t kBvhBinCount  = 24u;

        // ---- Triangle reference (for BVH build) ----
        struct TriangleRef
        {
            uint32_t index = 0;
            F3 bMin{FLT_MAX,FLT_MAX,FLT_MAX};
            F3 bMax{-FLT_MAX,-FLT_MAX,-FLT_MAX};
            F3 centroid{};
        };

        // ---- SAH split finder for triangles ----
        bool FindTriSahSplit(const std::vector<TriangleRef>& refs,
                             uint32_t begin, uint32_t end,
                             int& outAxis, float& outPos)
        {
            struct Bin { F3 bMin{FLT_MAX,FLT_MAX,FLT_MAX}; F3 bMax{-FLT_MAX,-FLT_MAX,-FLT_MAX}; uint32_t n=0; };

            F3 cMin{FLT_MAX,FLT_MAX,FLT_MAX}, cMax{-FLT_MAX,-FLT_MAX,-FLT_MAX};
            for (uint32_t i=begin; i<end; ++i) {
                cMin = fmin3(cMin, refs[i].centroid);
                cMax = fmax3(cMax, refs[i].centroid);
            }
            F3 ext = cMax - cMin;
            outAxis = 0;
            if (ext.y > ext.x && ext.y >= ext.z) outAxis = 1;
            else if (ext.z > ext.x)               outAxis = 2;

            const float axExt = GetAxis(ext, outAxis);
            if (axExt <= 1e-6f) return false;

            std::array<Bin, kBvhBinCount> bins{};
            const float inv = 1.f / axExt;
            for (uint32_t i=begin; i<end; ++i) {
                const float norm = (GetAxis(refs[i].centroid, outAxis) - GetAxis(cMin, outAxis)) * inv;
                const uint32_t b = std::min(kBvhBinCount-1u, (uint32_t)(norm * (float)kBvhBinCount));
                bins[b].bMin = fmin3(bins[b].bMin, refs[i].bMin);
                bins[b].bMax = fmax3(bins[b].bMax, refs[i].bMax);
                ++bins[b].n;
            }

            std::array<float,kBvhBinCount-1> lA{}, rA{};
            std::array<uint32_t,kBvhBinCount-1> lN{}, rN{};
            { F3 mn{FLT_MAX,FLT_MAX,FLT_MAX},mx{-FLT_MAX,-FLT_MAX,-FLT_MAX}; uint32_t n=0;
              for (uint32_t i=0; i+1<kBvhBinCount; ++i) {
                mn=fmin3(mn,bins[i].bMin); mx=fmax3(mx,bins[i].bMax); n+=bins[i].n;
                lA[i]=(n>0)?SurfaceArea(mn,mx):0.f; lN[i]=n; }}
            { F3 mn{FLT_MAX,FLT_MAX,FLT_MAX},mx{-FLT_MAX,-FLT_MAX,-FLT_MAX}; uint32_t n=0;
              for (int i=(int)kBvhBinCount-1; i>0; --i) {
                mn=fmin3(mn,bins[i].bMin); mx=fmax3(mx,bins[i].bMax); n+=bins[i].n;
                rA[i-1]=(n>0)?SurfaceArea(mn,mx):0.f; rN[i-1]=n; }}

            float best=FLT_MAX; uint32_t bSplit=0; bool found=false;
            for (uint32_t i=0; i+1<kBvhBinCount; ++i) {
                if (!lN[i]||!rN[i]) continue;
                float c=(float)lN[i]*lA[i]+(float)rN[i]*rA[i];
                if (c<best) { best=c; bSplit=i; found=true; }
            }
            if (!found) return false;
            outPos = GetAxis(cMin, outAxis) + axExt * ((float)(bSplit+1u)/(float)kBvhBinCount);
            return true;
        }

        // ---- Recursive mesh BLAS build ----
        int BuildMeshBvh(std::vector<TriangleRef>& refs,
                         std::vector<uint32_t>& indices,
                         std::vector<GpuSoftwareRayTracer::BvhNode>& nodes,
                         uint32_t begin, uint32_t end)
        {
            GpuSoftwareRayTracer::BvhNode node{};
            F3 bMin{FLT_MAX,FLT_MAX,FLT_MAX}, bMax{-FLT_MAX,-FLT_MAX,-FLT_MAX};
            F3 cMin{FLT_MAX,FLT_MAX,FLT_MAX}, cMax{-FLT_MAX,-FLT_MAX,-FLT_MAX};
            for (uint32_t i=begin; i<end; ++i) {
                bMin=fmin3(bMin,refs[i].bMin); bMax=fmax3(bMax,refs[i].bMax);
                cMin=fmin3(cMin,refs[i].centroid); cMax=fmax3(cMax,refs[i].centroid);
            }
            StoreBounds(bMin, bMax, node.boundsMin, node.boundsMax);
            const int nIdx = (int)nodes.size();
            nodes.push_back(node);

            const uint32_t count = end - begin;
            if (count <= kBvhLeafSize) {
                nodes[nIdx].leftChild    = -(int32_t(indices.size()) + 1);
                nodes[nIdx].rightOrCount = (int32_t)count;
                for (uint32_t i=begin; i<end; ++i) indices.push_back(refs[i].index);
                return nIdx;
            }

            int axis=0; float pos=0.f;
            uint32_t mid = begin + count/2u;
            if (FindTriSahSplit(refs, begin, end, axis, pos)) {
                auto it = std::partition(refs.begin()+begin, refs.begin()+end,
                    [axis,pos](const TriangleRef& r){ return GetAxis(r.centroid,axis)<pos; });
                mid = (uint32_t)(it - refs.begin());
            }
            if (mid==begin || mid==end) {
                F3 ext = cMax - cMin;
                axis=0; if(ext.y>ext.x&&ext.y>=ext.z) axis=1; else if(ext.z>ext.x) axis=2;
                mid = begin + count/2u;
                std::nth_element(refs.begin()+begin, refs.begin()+mid, refs.begin()+end,
                    [axis](const TriangleRef& a, const TriangleRef& b){
                        return GetAxis(a.centroid,axis)<GetAxis(b.centroid,axis); });
            }
            nodes[nIdx].leftChild    = BuildMeshBvh(refs, indices, nodes, begin, mid);
            nodes[nIdx].rightOrCount = BuildMeshBvh(refs, indices, nodes, mid,   end);
            return nIdx;
        }

        // ---- SAH split finder for instances (TLAS) ----
        bool FindInstSahSplit(const std::vector<uint32_t>& order,
                              const RayTracingScene& scene,
                              uint32_t begin, uint32_t end,
                              int& outAxis, float& outPos)
        {
            struct Bin { F3 bMin{FLT_MAX,FLT_MAX,FLT_MAX}; F3 bMax{-FLT_MAX,-FLT_MAX,-FLT_MAX}; uint32_t n=0; };

            F3 cMin{FLT_MAX,FLT_MAX,FLT_MAX}, cMax{-FLT_MAX,-FLT_MAX,-FLT_MAX};
            for (uint32_t i=begin; i<end; ++i) {
                const auto& inst = scene.instances[order[i]];
                F3 mn{inst.worldBoundsMin[0],inst.worldBoundsMin[1],inst.worldBoundsMin[2]};
                F3 mx{inst.worldBoundsMax[0],inst.worldBoundsMax[1],inst.worldBoundsMax[2]};
                F3 c = (mn+mx) * 0.5f;
                cMin=fmin3(cMin,c); cMax=fmax3(cMax,c);
            }
            F3 ext = cMax - cMin;
            outAxis=0; if(ext.y>ext.x&&ext.y>=ext.z) outAxis=1; else if(ext.z>ext.x) outAxis=2;
            const float axExt = GetAxis(ext, outAxis);
            if (axExt<=1e-6f) return false;

            std::array<Bin,kBvhBinCount> bins{};
            const float inv=1.f/axExt;
            for (uint32_t i=begin; i<end; ++i) {
                const auto& inst = scene.instances[order[i]];
                F3 mn{inst.worldBoundsMin[0],inst.worldBoundsMin[1],inst.worldBoundsMin[2]};
                F3 mx{inst.worldBoundsMax[0],inst.worldBoundsMax[1],inst.worldBoundsMax[2]};
                F3 c = (mn+mx)*0.5f;
                uint32_t b = std::min(kBvhBinCount-1u,(uint32_t)((GetAxis(c,outAxis)-GetAxis(cMin,outAxis))*inv*(float)kBvhBinCount));
                bins[b].bMin=fmin3(bins[b].bMin,mn); bins[b].bMax=fmax3(bins[b].bMax,mx); ++bins[b].n;
            }

            std::array<float,kBvhBinCount-1> lA{},rA{}; std::array<uint32_t,kBvhBinCount-1> lN{},rN{};
            { F3 mn{FLT_MAX,FLT_MAX,FLT_MAX},mx{-FLT_MAX,-FLT_MAX,-FLT_MAX}; uint32_t n=0;
              for (uint32_t i=0; i+1<kBvhBinCount; ++i) {
                mn=fmin3(mn,bins[i].bMin); mx=fmax3(mx,bins[i].bMax); n+=bins[i].n;
                lA[i]=(n>0)?SurfaceArea(mn,mx):0.f; lN[i]=n; }}
            { F3 mn{FLT_MAX,FLT_MAX,FLT_MAX},mx{-FLT_MAX,-FLT_MAX,-FLT_MAX}; uint32_t n=0;
              for (int i=(int)kBvhBinCount-1; i>0; --i) {
                mn=fmin3(mn,bins[i].bMin); mx=fmax3(mx,bins[i].bMax); n+=bins[i].n;
                rA[i-1]=(n>0)?SurfaceArea(mn,mx):0.f; rN[i-1]=n; }}

            float best=FLT_MAX; uint32_t bSplit=0; bool found=false;
            for (uint32_t i=0; i+1<kBvhBinCount; ++i) {
                if (!lN[i]||!rN[i]) continue;
                float c=(float)lN[i]*lA[i]+(float)rN[i]*rA[i];
                if (c<best){best=c; bSplit=i; found=true;}
            }
            if (!found) return false;
            outPos = GetAxis(cMin,outAxis) + axExt*((float)(bSplit+1u)/(float)kBvhBinCount);
            return true;
        }

        // ---- Recursive TLAS build (free function, called from RebuildTlas member) ----
        int BuildTlasFree(std::vector<uint32_t>& order,
                      const RayTracingScene& scene,
                      std::vector<GpuSoftwareRayTracer::TlasNode>& nodes,
                      uint32_t begin, uint32_t end)
        {
            GpuSoftwareRayTracer::TlasNode node{};
            F3 bMin{FLT_MAX,FLT_MAX,FLT_MAX}, bMax{-FLT_MAX,-FLT_MAX,-FLT_MAX};
            F3 cMin{FLT_MAX,FLT_MAX,FLT_MAX}, cMax{-FLT_MAX,-FLT_MAX,-FLT_MAX};
            for (uint32_t i=begin; i<end; ++i) {
                const auto& inst = scene.instances[order[i]];
                F3 mn{inst.worldBoundsMin[0],inst.worldBoundsMin[1],inst.worldBoundsMin[2]};
                F3 mx{inst.worldBoundsMax[0],inst.worldBoundsMax[1],inst.worldBoundsMax[2]};
                F3 c=(mn+mx)*0.5f;
                bMin=fmin3(bMin,mn); bMax=fmax3(bMax,mx);
                cMin=fmin3(cMin,c);  cMax=fmax3(cMax,c);
            }
            StoreBounds(bMin, bMax, node.boundsMin, node.boundsMax);
            const int nIdx = (int)nodes.size();
            nodes.push_back(node);

            const uint32_t count = end - begin;
            if (count <= kBvhLeafSize) {
                nodes[nIdx].leftChild    = -(int32_t(begin) + 1);
                nodes[nIdx].rightOrCount = (int32_t)count;
                return nIdx;
            }

            int axis=0; float pos=0.f;
            uint32_t mid = begin + count/2u;
            if (FindInstSahSplit(order, scene, begin, end, axis, pos)) {
                auto it = std::partition(order.begin()+begin, order.begin()+end,
                    [&scene,axis,pos](uint32_t idx){
                        const auto& inst = scene.instances[idx];
                        F3 mn{inst.worldBoundsMin[0],inst.worldBoundsMin[1],inst.worldBoundsMin[2]};
                        F3 mx{inst.worldBoundsMax[0],inst.worldBoundsMax[1],inst.worldBoundsMax[2]};
                        return GetAxis((mn+mx)*0.5f, axis) < pos; });
                mid = (uint32_t)(it - order.begin());
            }
            if (mid==begin||mid==end) {
                F3 ext=cMax-cMin; axis=0;
                if(ext.y>ext.x&&ext.y>=ext.z) axis=1; else if(ext.z>ext.x) axis=2;
                mid = begin+count/2u;
                std::nth_element(order.begin()+begin, order.begin()+mid, order.begin()+end,
                    [&scene,axis](uint32_t a, uint32_t b){
                        const auto& ia=scene.instances[a]; const auto& ib=scene.instances[b];
                        F3 ca{ia.worldBoundsMin[0],ia.worldBoundsMin[1],ia.worldBoundsMin[2]};
                        F3 cb{ib.worldBoundsMin[0],ib.worldBoundsMin[1],ib.worldBoundsMin[2]};
                        F3 ma{ia.worldBoundsMax[0],ia.worldBoundsMax[1],ia.worldBoundsMax[2]};
                        F3 mb{ib.worldBoundsMax[0],ib.worldBoundsMax[1],ib.worldBoundsMax[2]};
                        return GetAxis((ca+ma)*0.5f,axis)<GetAxis((cb+mb)*0.5f,axis); });
            }
            nodes[nIdx].leftChild    = BuildTlasFree(order, scene, nodes, begin, mid);
            nodes[nIdx].rightOrCount = BuildTlasFree(order, scene, nodes, mid,   end);
            return nIdx;
        }

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

        // ---- Create a default-heap GPU buffer and upload data ----
        // Creates its own command list, uploads, and stalls GPU. Use only on dirty/init.
        bool CreateAndUploadBuffer(IRHIDevice& device,
                                   const void* data, UINT64 byteSize,
                                   D3D12_RESOURCE_STATES finalState,
                                   Resource& outBuffer)
        {
            if (byteSize == 0) { outBuffer.Reset(); return true; }

            D3D12_HEAP_PROPERTIES defHeap{};
            defHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC bdesc{};
            bdesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bdesc.Width     = byteSize;
            bdesc.Height = bdesc.DepthOrArraySize = bdesc.MipLevels = 1;
            bdesc.SampleDesc.Count = 1;
            bdesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(device.CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE,
                                                       &bdesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                       nullptr, outBuffer))) {
                return false;
            }

            // Upload staging
            D3D12_HEAP_PROPERTIES upHeap{};
            upHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
            Resource staging;
            if (FAILED(device.CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE,
                                                       &bdesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr, staging))) {
                return false;
            }
            void* mapped = nullptr;
            if (FAILED(staging.Map(0, nullptr, &mapped))) return false;
            memcpy(mapped, data, byteSize);
            staging.Unmap(0, nullptr);

            CommandAllocator alloc; CommandList cmdList;
            if (FAILED(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, alloc))) return false;
            if (FAILED(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, cmdList))) return false;

            cmdList.Get()->CopyBufferRegion(outBuffer.Get(), 0, staging.Get(), 0, byteSize);
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(outBuffer.Get(),
                D3D12_RESOURCE_STATE_COPY_DEST, finalState);
            cmdList.Get()->ResourceBarrier(1, &barrier);
            cmdList.Get()->Close();
            ID3D12CommandList* lists[] = { cmdList.Get() };
            device.GetCommandQueue()->ExecuteCommandLists(1, lists);
            device.WaitForGPU();
            return true;
        }

        // ---- Create a StructuredBuffer SRV ----
        void CreateStructuredSrv(ID3D12Device* dev, Resource& buf,
                                 UINT elemCount, UINT stride,
                                 D3D12_CPU_DESCRIPTOR_HANDLE dest)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Format                  = DXGI_FORMAT_UNKNOWN;
            d.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            d.Buffer.FirstElement     = 0;
            d.Buffer.NumElements      = elemCount;
            d.Buffer.StructureByteStride = stride;
            d.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_NONE;
            dev->CreateShaderResourceView(buf.Get(), &d, dest);
        }

        // ---- Create a null SRV (for empty buffers) ----
        void CreateNullStructuredSrv(ID3D12Device* dev, D3D12_CPU_DESCRIPTOR_HANDLE dest)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Format                  = DXGI_FORMAT_UNKNOWN;
            d.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
            d.Buffer.NumElements      = 0;
            d.Buffer.StructureByteStride = 4u;
            dev->CreateShaderResourceView(nullptr, &d, dest);
        }

        // ---- Create a UAV-capable default-heap buffer (no initial data) ----
        bool CreateUavBuffer(IRHIDevice& device, UINT64 byteSize,
                             D3D12_RESOURCE_STATES initialState, Resource& out)
        {
            if (byteSize == 0) { out.Reset(); return false; }
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width     = byteSize;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return SUCCEEDED(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                &desc, initialState, nullptr, out));
        }

        // ---- Create a Texture2D with UAV support ----
        bool CreateUavTexture2D(IRHIDevice& device, UINT w, UINT h, DXGI_FORMAT fmt,
                                D3D12_RESOURCE_STATES initialState, Resource& out)
        {
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width     = w; desc.Height = h;
            desc.DepthOrArraySize = 1; desc.MipLevels = 1;
            desc.Format    = fmt;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return SUCCEEDED(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                &desc, initialState, nullptr, out));
        }

        // ---- Transition barrier shorthand ----
        D3D12_RESOURCE_BARRIER MakeTransition(ID3D12Resource* res,
                                               D3D12_RESOURCE_STATES from,
                                               D3D12_RESOURCE_STATES to)
        {
            return CD3DX12_RESOURCE_BARRIER::Transition(res, from, to);
        }

        // ---- Create a Texture2D SRV into a given CPU handle ----
        void CreateTex2DSrv(ID3D12Device* dev, ID3D12Resource* tex,
                            DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE dest)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Format      = fmt;
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(tex, &d, dest);
        }

        // ---- Create a Texture2D UAV into a given CPU handle ----
        void CreateTex2DUav(ID3D12Device* dev, ID3D12Resource* tex,
                            DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE dest)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
            d.Format        = fmt;
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            dev->CreateUnorderedAccessView(tex, nullptr, &d, dest);
        }

    } // anonymous namespace

    // =========================================================================
    // GpuSoftwareRayTracer — constructor / destructor
    // =========================================================================

    GpuSoftwareRayTracer::GpuSoftwareRayTracer() = default;
    GpuSoftwareRayTracer::~GpuSoftwareRayTracer()
    {
        if (m_frameConstantsMapped && m_frameConstantsBuffer.IsValid()) {
            m_frameConstantsBuffer.Unmap(0, nullptr);
        }
        m_frameConstantsMapped = nullptr;
        if (m_restirConstantsMapped && m_restirConstantsBuffer.IsValid()) {
            m_restirConstantsBuffer.Unmap(0, nullptr);
        }
        m_restirConstantsMapped = nullptr;
        if (m_lightDataMapped && m_lightDataBuffer.IsValid()) {
            m_lightDataBuffer.Unmap(0, nullptr);
        }
        m_lightDataMapped = nullptr;
        // Destroy NRD integration before GPU resources are freed
        if (m_nrdIntegration) {
            m_nrdIntegration->Destroy();
            m_nrdIntegration.reset();
        }
    }

    // =========================================================================
    // GetBvhGpuAddresses
    // =========================================================================

    GpuSoftwareRayTracer::BvhGpuAddresses GpuSoftwareRayTracer::GetBvhGpuAddresses() const
    {
        BvhGpuAddresses out{};
        if (!m_initialized) return out;
        out.bvhNodes  = m_bvhNodesBuffer.IsValid() ? m_bvhNodesBuffer.GetGPUVirtualAddress() : 0u;
        out.triangles = m_triangleBuffer.IsValid()  ? m_triangleBuffer.GetGPUVirtualAddress()  : 0u;
        out.meshInfo  = m_meshInfoBuffer.IsValid()  ? m_meshInfoBuffer.GetGPUVirtualAddress()  : 0u;
        out.instances = m_instanceBuffer.IsValid()  ? m_instanceBuffer.GetGPUVirtualAddress()  : 0u;
        out.tlasNodes = m_tlasBuffer.IsValid()      ? m_tlasBuffer.GetGPUVirtualAddress()      : 0u;
        out.materials = m_materialBuffer.IsValid()  ? m_materialBuffer.GetGPUVirtualAddress()  : 0u;
        out.valid = (out.bvhNodes != 0);
        return out;
    }

    // =========================================================================
    // Initialize
    // =========================================================================

    bool GpuSoftwareRayTracer::Initialize(IRHIDevice& device)
    {
        if (m_initialized) return true;

        ID3D12Device* dev = device.GetDevice();
        if (!dev) return false;

        // ---- Descriptor heap (GPU-visible CBV_SRV_UAV) ----
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kTotalDescriptors;
        hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device.CreateDescriptorHeap(hd, m_descHeap))) return false;
        m_descIncrementSize = device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // Fill all SRV slots with null descriptors initially
        for (UINT i = 0; i < kSrvCount; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_descHeap.GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += i * m_descIncrementSize;
            CreateNullStructuredSrv(dev, cpu);
        }

        // ---- Frame constants buffer:
        // Slot 0: shared/legacy shadow or AO
        // Slot 1: legacy reflection
        // Slots 2-5: SWRT directional shadow cascades
        // ----
        constexpr UINT64 kCbufSlotSize = 256u;
        constexpr UINT64 kCbufTotalSize = kCbufSlotSize * (2u + LightSystem::kDirectionalCascadeCount);
        if (!ResourceUploadUtility::CreateUploadBuffer(device, kCbufTotalSize,
                                                        m_frameConstantsBuffer,
                                                        reinterpret_cast<void**>(&m_frameConstantsMapped))) {
            return false;
        }

        // ---- ReSTIR constants buffer: 7 slots of 256 bytes ----
        constexpr UINT64 kRestirCbufTotal = 256u * kReSTIRCbufSlotCount;
        if (!ResourceUploadUtility::CreateUploadBuffer(device, kRestirCbufTotal,
                                                        m_restirConstantsBuffer,
                                                        reinterpret_cast<void**>(&m_restirConstantsMapped))) {
            return false;
        }

        // ---- Light data upload buffer ----
        constexpr UINT64 kLightBufSize =
            kMaxPointLights * sizeof(GpuPointLightRT) +
            kMaxSpotLights  * sizeof(GpuSpotLightRT);
        if (!ResourceUploadUtility::CreateUploadBuffer(device, kLightBufSize,
                                                        m_lightDataBuffer,
                                                        reinterpret_cast<void**>(&m_lightDataMapped))) {
            return false;
        }

        // ---- Compile shaders + create pipelines ----
        if (!CreatePipelines(device)) return false;

        // ---- Try to compile ReSTIR shaders (non-fatal if they fail) ----
        if (!CreateReSTIRPipelines(device)) {
            OutputDebugStringA("GpuSWRT: ReSTIR pipeline creation failed – falling back to legacy reflection.\n");
        }

        m_initialized = true;
        return true;
    }

    // =========================================================================
    // CreatePipelines
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

        D3D12_ROOT_PARAMETER params[4]{};
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
        rsDesc.NumParameters     = 4;
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

            ComPtr<ID3DBlob> acs;
            if (!CompileComputeShader(L"SWRT/SWRT_Reflection_ATrous_CS.hlsl",
                                      "CS_ReflectionATrous", acs)) {
                OutputDebugStringA("GpuSWRT: failed to compile SWRT_Reflection_ATrous_CS.hlsl\n");
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC apso{};
            apso.pRootSignature = m_reflTemporalRootSignature.Get();
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
        // [7]: Root UAV  (u2)  reservoir output inline
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
        scratchUavRange.NumDescriptors     = kScratchUavCount;  // 3: u0-u2
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
        // [7] Reservoir output UAV inline (u2)
        params[7].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[7].Descriptor.ShaderRegister = 2;
        params[7].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        // [8] Prev-temporal reservoir SRV inline (t15) — previous frame's temporal output for ping-pong
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

    // =========================================================================
    // AllocateReSTIRBuffers
    // =========================================================================

    bool GpuSoftwareRayTracer::AllocateReSTIRBuffers(IRHIDevice& device,
                                                       uint32_t w, uint32_t h)
    {
        const UINT64 pixelCount = (UINT64)w * h;
        constexpr UINT64 kReservoirStride = 16u; // sizeof(Reservoir) = uint+float+uint+float

        // Release existing resources
        m_gBufferTexture.Reset();
        m_prevGBufferTexture.Reset();
        m_reservoirBuffer[0].Reset(); m_reservoirBuffer[1].Reset(); m_reservoirBuffer[2].Reset();
        m_prevColorTexture.Reset();
        m_shadedColorTexture.Reset();
        m_nrdDiffIn.Reset();
        m_nrdViewZ.Reset();
        m_nrdNormalRoughness.Reset();
        m_nrdMotionVec.Reset();
        m_nrdDiffOut.Reset();
        m_atrousPingPong.Reset();
        m_nrdReady  = false;
        m_atrousReady = false;

        constexpr auto kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_gBufferTexture))        return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_prevGBufferTexture))    return false;
        if (!CreateUavBuffer(device, pixelCount * kReservoirStride, kSrv, m_reservoirBuffer[0]))               return false;
        if (!CreateUavBuffer(device, pixelCount * kReservoirStride, kSrv, m_reservoirBuffer[1]))               return false;
        if (!CreateUavBuffer(device, pixelCount * kReservoirStride, kSrv, m_reservoirBuffer[2]))               return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_prevColorTexture))       return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_shadedColorTexture))     return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_atrousPingPong))         return false;
        m_atrousReady = true;

        if (!AllocateNrdBuffers(device, w, h)) return false;

        m_restirWidth   = w;
        m_restirHeight  = h;
        m_restirFrameIndex = 0;
        memset(m_prevVP, 0, sizeof(m_prevVP));
        m_prevVP[0] = m_prevVP[5] = m_prevVP[10] = m_prevVP[15] = 1.0f;
        memset(m_prevReflectionCameraPos, 0, sizeof(m_prevReflectionCameraPos));

        return true;
    }

    // =========================================================================
    // AllocateNrdBuffers
    // =========================================================================

    bool GpuSoftwareRayTracer::AllocateNrdBuffers(IRHIDevice& device, uint32_t w, uint32_t h)
    {
        constexpr auto kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_nrdDiffIn))          return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16_FLOAT,          kSrv, m_nrdViewZ))            return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM,     kSrv, m_nrdNormalRoughness))  return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16_FLOAT,       kSrv, m_nrdMotionVec))        return false;
        if (!CreateUavTexture2D(device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_nrdDiffOut))          return false;

        return true;
    }

    // =========================================================================
    // InitializeNrdIntegration
    // =========================================================================

    bool GpuSoftwareRayTracer::InitializeNrdIntegration(IRHIDevice& device, uint32_t w, uint32_t h)
    {
        ID3D12Device* d3d12Device = device.GetDevice();
        if (!d3d12Device) return false;

        nrd::DenoiserDesc denoiserDescs[1] = {};
        denoiserDescs[0].identifier = 0u;
        denoiserDescs[0].denoiser   = nrd::Denoiser::RELAX_DIFFUSE;

        nrd::InstanceCreationDesc instanceDesc = {};
        instanceDesc.denoisers    = denoiserDescs;
        instanceDesc.denoisersNum = 1;

        nrd::IntegrationCreationDesc integrationDesc = {};
        strncpy_s(integrationDesc.name, "RELAX_D", sizeof(integrationDesc.name) - 1);
        integrationDesc.resourceWidth       = static_cast<uint16_t>(w);
        integrationDesc.resourceHeight      = static_cast<uint16_t>(h);
        integrationDesc.queuedFrameNum      = 3;
        integrationDesc.autoWaitForIdle     = true;
        integrationDesc.residencyPriority   = 0.0f;

        nri::DeviceCreationD3D12Desc deviceD3D12Desc = {};
        deviceD3D12Desc.d3d12Device            = d3d12Device;
        deviceD3D12Desc.queueFamilies          = nullptr;
        deviceD3D12Desc.queueFamilyNum         = 0;
        deviceD3D12Desc.enableNRIValidation    = false;
        deviceD3D12Desc.disableD3D12EnhancedBarriers = true;
        deviceD3D12Desc.disableNVAPIInitialization   = true;

        m_nrdIntegration = std::make_unique<nrd::Integration>();
        nrd::Result result = m_nrdIntegration->RecreateD3D12(integrationDesc, instanceDesc, deviceD3D12Desc);
        if (result != nrd::Result::SUCCESS) {
            OutputDebugStringA("GpuSWRT: NRD RecreateD3D12 failed\n");
            m_nrdIntegration.reset();
            return false;
        }

        m_nrdReady = true;
        return true;
    }

    // =========================================================================
    // FillReSTIRConstants
    // =========================================================================

    void GpuSoftwareRayTracer::FillReSTIRConstants(const ReflectionTextureDesc& desc,
                                                    float stepWidth,
                                                    ReSTIRFrameConstants& out) const
    {
        memset(&out, 0, sizeof(out));
        memcpy(out.invVP,     desc.frameDesc.inverseViewProjection, 64);
        memcpy(out.prevVP, m_prevVP,                                 64);
        out.cameraPos[0]          = desc.frameDesc.cameraPosition[0];
        out.cameraPos[1]          = desc.frameDesc.cameraPosition[1];
        out.cameraPos[2]          = desc.frameDesc.cameraPosition[2];
        out.tMin                  = 0.001f;
        out.renderWidth           = desc.width;
        out.renderHeight          = desc.height;
        out.frameIndex            = m_restirFrameIndex;
        out.reservoirWidth        = desc.width;
        // When the camera moved since the last frame, discard temporal history entirely
        // (alpha = 1.0 = full current-frame weight) to prevent ghosting / lag.
        // When the camera is stationary, accumulate normally at 0.1 (10% current frame).
        out.temporalAlpha         = desc.cameraChanged ? 1.0f : 0.1f;
        out.phiColor              = 4.0f;
        out.phiNormal             = 128.0f;
        out.phiDepth              = 1.0f;
        out.stepWidth             = stepWidth;
        out.maxSurfaceRoughness   = desc.maxSurfaceRoughness;
        out.maxPrimaryHitDistance = desc.maxPrimaryHitDistance;
        out.minReflectionEnergy   = desc.minReflectionEnergy;
        out.shadowBias            = 0.002f;
        out.ambientColor[0] = out.ambientColor[1] = out.ambientColor[2] = 0.1f;
        out.ambientIntensity = 1.0f;

        const auto& dl = desc.frameDesc.directionalLight;
        {
            float fwd[3]{};
            Math::DirectionFromYawPitch(dl.yaw, dl.pitch, fwd);
            out.dirLightDir[0] = -fwd[0];
            out.dirLightDir[1] = -fwd[1];
            out.dirLightDir[2] = -fwd[2];
        }
        out.dirLightIntensity = dl.intensity;
        out.dirLightColor[0]  = dl.color[0];
        out.dirLightColor[1]  = dl.color[1];
        out.dirLightColor[2]  = dl.color[2];

        uint32_t nPt = 0, nSp = 0;
        if (desc.frameDesc.pointLights)
            nPt = std::min((uint32_t)desc.frameDesc.pointLights->size(), kMaxPointLights);
        if (desc.frameDesc.spotLights)
            nSp = std::min((uint32_t)desc.frameDesc.spotLights->size(), kMaxSpotLights);
        out.pointLightCount = nPt;
        out.spotLightCount  = nSp;
    }

    // =========================================================================
    // SetScratchTexSRV / SetScratchTexUAV
    // =========================================================================

    void GpuSoftwareRayTracer::SetScratchTexSRV(ID3D12Device* dev, Resource& tex,
                                                  DXGI_FORMAT fmt, UINT slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_descHeap.GetCPUDescriptorHandleForHeapStart();
        h.ptr += (kScratchSrvBase + slot) * m_descIncrementSize;
        CreateTex2DSrv(dev, tex.Get(), fmt, h);
    }

    void GpuSoftwareRayTracer::SetScratchTexUAV(ID3D12Device* dev, Resource& tex,
                                                  DXGI_FORMAT fmt, UINT slot)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_descHeap.GetCPUDescriptorHandleForHeapStart();
        h.ptr += (kScratchUavBase + slot) * m_descIncrementSize;
        CreateTex2DUav(dev, tex.Get(), fmt, h);
    }

    // =========================================================================
    // UpdateScene
    // =========================================================================

    void GpuSoftwareRayTracer::UpdateScene(const RayTracingScene& scene,
                                           IRHIDevice& device,
                                           CommandList& /*unusedCmdList*/)
    {
        if (!m_initialized) return;

        m_scene = scene;

        const bool geoDirty  = (scene.geometryVersion != m_bvhGeometryVersion);
        const bool instDirty = (scene.instanceVersion  != m_bvhInstanceVersion);
        if (!geoDirty && !instDirty) return;

        RebuildAccelerationStructures();
        UploadBvhBuffers(device);
    }

    // =========================================================================
    // RebuildAccelerationStructures
    // =========================================================================

    void GpuSoftwareRayTracer::RebuildAccelerationStructures()
    {
        m_meshAccelerations.clear();
        m_meshAccelerations.resize(m_scene.meshes.size());

        for (size_t meshIdx=0; meshIdx<m_scene.meshes.size(); ++meshIdx) {
            BuildMeshBvhSah((uint32_t)meshIdx);
        }

        // Compute flat offsets for GPU buffer layout
        uint32_t nodeOff=0, triOff=0;
        for (auto& ma : m_meshAccelerations) {
            ma.nodeOffset = nodeOff;
            ma.triOffset  = triOff;
            nodeOff += (uint32_t)ma.nodes.size();
            triOff  += (uint32_t)ma.triangles.size();
        }

        RebuildTlas();

        m_bvhGeometryVersion = m_scene.geometryVersion;
        m_bvhInstanceVersion = m_scene.instanceVersion;
    }

    void GpuSoftwareRayTracer::BuildMeshBvhSah(uint32_t meshIdx)
    {
        auto& ma = m_meshAccelerations[meshIdx];
        ma.nodes.clear();
        ma.triangles.clear();

        const RayTracingMesh& rtMesh = m_scene.meshes[meshIdx];
        const auto& verts   = rtMesh.mesh.vertices;
        const auto& indices = rtMesh.mesh.indices;

        const uint32_t triCount = indices.empty()
            ? (uint32_t)(verts.size() / 3u)
            : (uint32_t)(indices.size() / 3u);
        if (triCount == 0) return;

        auto getV = [&](uint32_t ti, uint32_t vi) -> const Vertex& {
            return indices.empty() ? verts[ti*3+vi] : verts[indices[ti*3+vi]];
        };

        ma.triangles.resize(triCount);
        std::vector<TriangleRef> refs(triCount);

        for (uint32_t ti=0; ti<triCount; ++ti) {
            const Vertex& v0 = getV(ti,0);
            const Vertex& v1 = getV(ti,1);
            const Vertex& v2 = getV(ti,2);

            F3 p0{v0.position[0],v0.position[1],v0.position[2]};
            F3 p1{v1.position[0],v1.position[1],v1.position[2]};
            F3 p2{v2.position[0],v2.position[1],v2.position[2]};
            F3 e1 = p1-p0, e2 = p2-p0;

            GpuTriangle& gt = ma.triangles[ti];
            gt.p0[0]=p0.x;  gt.p0[1]=p0.y;  gt.p0[2]=p0.z;
            gt.edge1[0]=e1.x; gt.edge1[1]=e1.y; gt.edge1[2]=e1.z;
            gt.edge2[0]=e2.x; gt.edge2[1]=e2.y; gt.edge2[2]=e2.z;
            gt.n0[0]=v0.normal[0]; gt.n0[1]=v0.normal[1]; gt.n0[2]=v0.normal[2];
            gt.n1[0]=v1.normal[0]; gt.n1[1]=v1.normal[1]; gt.n1[2]=v1.normal[2];
            gt.n2[0]=v2.normal[0]; gt.n2[1]=v2.normal[1]; gt.n2[2]=v2.normal[2];
            gt.uv0[0]=v0.uv[0]; gt.uv0[1]=v0.uv[1];
            gt.uv1[0]=v1.uv[0]; gt.uv1[1]=v1.uv[1];
            gt.uv2[0]=v2.uv[0]; gt.uv2[1]=v2.uv[1];

            TriangleRef& r = refs[ti];
            r.index   = ti;
            r.bMin    = fmin3(fmin3(p0,p1),p2);
            r.bMax    = fmax3(fmax3(p0,p1),p2);
            r.centroid = (p0+p1+p2) * (1.f/3.f);
        }

        std::vector<uint32_t> triIndices;
        BuildMeshBvh(refs, triIndices, ma.nodes, 0, triCount);

        // Reorder triangles to match BVH leaf order
        if (triIndices.size() == ma.triangles.size()) {
            std::vector<GpuTriangle> ordered;
            ordered.reserve(triIndices.size());
            for (uint32_t idx : triIndices) ordered.push_back(ma.triangles[idx]);
            ma.triangles = std::move(ordered);
        }
    }

    void GpuSoftwareRayTracer::RebuildTlas()
    {
        m_topLevelNodes.clear();
        m_topLevelInstanceOrder.clear();
        if (m_scene.instances.empty()) return;

        m_topLevelInstanceOrder.resize(m_scene.instances.size());
        for (uint32_t i=0; i<(uint32_t)m_topLevelInstanceOrder.size(); ++i)
            m_topLevelInstanceOrder[i] = i;

        BuildTlasFree(m_topLevelInstanceOrder, m_scene, m_topLevelNodes, 0,
                      (uint32_t)m_topLevelInstanceOrder.size());
    }

    // =========================================================================
    // UploadBvhBuffers — flatten CPU BVH into GPU buffers
    // =========================================================================

    bool GpuSoftwareRayTracer::UploadBvhBuffers(IRHIDevice& device)
    {
        ID3D12Device* dev = device.GetDevice();
        if (!dev || !m_descHeap.Get()) return false;

        // ---- Flatten BLAS nodes ----
        std::vector<BvhNode> allNodes;
        std::vector<GpuTriangle> allTris;
        std::vector<GpuMeshInfo> meshInfos;
        for (const auto& ma : m_meshAccelerations) {
            GpuMeshInfo mi{ ma.nodeOffset, ma.triOffset, 0, 0 };
            meshInfos.push_back(mi);
            // Interior node child indices from BuildMeshBvh are mesh-local (0-based within ma.nodes).
            // Offset them to absolute global indices so the GPU traversal can index g_bvhNodes directly.
            for (BvhNode node : ma.nodes) {
                if (!node.IsLeaf()) {
                    node.leftChild    += static_cast<int32_t>(ma.nodeOffset);
                    node.rightOrCount += static_cast<int32_t>(ma.nodeOffset);
                }
                allNodes.push_back(node);
            }
            allTris.insert(allTris.end(), ma.triangles.begin(), ma.triangles.end());
        }

        // ---- Instance buffer ----
        std::vector<GpuInstanceInfo> instBuf;
        instBuf.reserve(m_topLevelInstanceOrder.size());
        for (uint32_t ordIdx=0; ordIdx<(uint32_t)m_topLevelInstanceOrder.size(); ++ordIdx) {
            const uint32_t instIdx = m_topLevelInstanceOrder[ordIdx];
            const RayTracingInstance& ri = m_scene.instances[instIdx];
            GpuInstanceInfo gi{};
            gi.meshIndex     = ri.meshIndex;
            gi.materialIndex = ri.materialIndex;
            memcpy(gi.world,    ri.model,        64);
            memcpy(gi.invWorld, ri.inverseModel,  64);
            memcpy(gi.worldBoundsMin, ri.worldBoundsMin, 12);
            memcpy(gi.worldBoundsMax, ri.worldBoundsMax, 12);
            instBuf.push_back(gi);
        }

        // ---- Material buffer ----
        std::vector<GpuMaterial> matBuf;
        matBuf.reserve(m_scene.materials.size());
        for (const auto& mat : m_scene.materials) {
            GpuMaterial gm{};
            gm.baseColor[0] = mat.material.baseColor[0];
            gm.baseColor[1] = mat.material.baseColor[1];
            gm.baseColor[2] = mat.material.baseColor[2];
            gm.baseColor[3] = mat.material.baseColor[3];
            gm.roughness = mat.material.roughness;
            gm.metallic  = mat.material.metallic;
            gm.transmission = mat.material.transmission;
            gm.ior = mat.material.ior;
            gm.specularColor[0] = mat.material.specularColor[0];
            gm.specularColor[1] = mat.material.specularColor[1];
            gm.specularColor[2] = mat.material.specularColor[2];
            gm.workflow = static_cast<float>(static_cast<uint32_t>(mat.material.workflow));
            gm.emissive[0] = mat.material.emissive[0];
            gm.emissive[1] = mat.material.emissive[1];
            gm.emissive[2] = mat.material.emissive[2];
            gm.occlusionStrength = mat.material.occlusionStrength;
            matBuf.push_back(gm);
        }

        // ---- Fallback: ensure at least one entry for empty scenes ----
        if (allNodes.empty())   allNodes.push_back({});
        if (allTris.empty())    allTris.push_back({});
        if (meshInfos.empty())  meshInfos.push_back({});
        if (instBuf.empty())    instBuf.push_back({});
        if (matBuf.empty())     matBuf.push_back({});
        if (m_topLevelNodes.empty()) m_topLevelNodes.push_back({});

        // ---- Upload to GPU (stalls GPU once if dirty) ----
        if (!CreateAndUploadBuffer(device, allNodes.data(),   allNodes.size()*sizeof(BvhNode),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m_bvhNodesBuffer)) return false;
        if (!CreateAndUploadBuffer(device, allTris.data(),    allTris.size()*sizeof(GpuTriangle),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m_triangleBuffer)) return false;
        if (!CreateAndUploadBuffer(device, meshInfos.data(),  meshInfos.size()*sizeof(GpuMeshInfo),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m_meshInfoBuffer)) return false;
        if (!CreateAndUploadBuffer(device, instBuf.data(),    instBuf.size()*sizeof(GpuInstanceInfo),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m_instanceBuffer)) return false;
        if (!CreateAndUploadBuffer(device, m_topLevelNodes.data(), m_topLevelNodes.size()*sizeof(TlasNode),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m_tlasBuffer)) return false;
        if (!CreateAndUploadBuffer(device, matBuf.data(),     matBuf.size()*sizeof(GpuMaterial),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m_materialBuffer)) return false;

        // ---- Create SRVs in descriptor heap ----
        auto cpuBase = m_descHeap.GetCPUDescriptorHandleForHeapStart();
        auto srv = [&](UINT slot) -> D3D12_CPU_DESCRIPTOR_HANDLE {
            return { cpuBase.ptr + slot * m_descIncrementSize };
        };

        CreateStructuredSrv(dev, m_bvhNodesBuffer,  (UINT)allNodes.size(),  sizeof(BvhNode),       srv(0));
        CreateStructuredSrv(dev, m_triangleBuffer,  (UINT)allTris.size(),   sizeof(GpuTriangle),   srv(1));
        CreateStructuredSrv(dev, m_meshInfoBuffer,  (UINT)meshInfos.size(), sizeof(GpuMeshInfo),   srv(2));
        CreateStructuredSrv(dev, m_instanceBuffer,  (UINT)instBuf.size(),   sizeof(GpuInstanceInfo), srv(3));
        CreateStructuredSrv(dev, m_tlasBuffer,      (UINT)m_topLevelNodes.size(), sizeof(TlasNode), srv(4));
        CreateStructuredSrv(dev, m_materialBuffer,  (UINT)matBuf.size(),    sizeof(GpuMaterial),   srv(5));

        m_uploadedGeometryVersion = m_scene.geometryVersion;
        m_uploadedInstanceVersion = m_scene.instanceVersion;
        return true;
    }

    // =========================================================================
    // UpdateTextureUav — write UAV descriptor for output texture
    // =========================================================================

    void GpuSoftwareRayTracer::UpdateTextureUav(ID3D12Device* dev, Resource& texture,
                                                 DXGI_FORMAT format, UINT slotIndex, UINT arraySize)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dest = m_descHeap.GetCPUDescriptorHandleForHeapStart();
        dest.ptr += slotIndex * m_descIncrementSize;

        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format        = format;
        if (arraySize > 1u)
        {
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray.ArraySize = arraySize;
            d.Texture2DArray.FirstArraySlice = 0u;
        }
        else
        {
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        }
        dev->CreateUnorderedAccessView(texture.Get(), nullptr, &d, dest);
    }

    // =========================================================================
    // FillShadowConstants / FillReflectionConstants
    // =========================================================================

    void GpuSoftwareRayTracer::FillShadowConstants(const DirectionalShadowMapDesc& desc,
                                                    ShadowFrameConstants& out) const
    {
        memset(&out, 0, sizeof(out));
        memcpy(out.invLightVP, desc.inverseLightViewProjection, 64);
        memcpy(out.lightVP,    desc.lightViewProjection,        64);
        out.width   = desc.width;
        out.height  = desc.height;
        out.tMin    = 0.001f;
        out.depthBias = std::max(0.0f, desc.depthBias);
        out.arraySlice = desc.arraySlice;
    }

    void GpuSoftwareRayTracer::FillReflectionConstants(const ReflectionTextureDesc& desc,
                                                        ReflectionFrameConstants& out) const
    {
        memset(&out, 0, sizeof(out));
        memcpy(out.invVP, desc.frameDesc.inverseViewProjection, 64);
        out.cameraPos[0]       = desc.frameDesc.cameraPosition[0];
        out.cameraPos[1]       = desc.frameDesc.cameraPosition[1];
        out.cameraPos[2]       = desc.frameDesc.cameraPosition[2];
        out.tMin               = 0.001f;
        out.renderWidth        = desc.width;
        out.renderHeight       = desc.height;
        out.updatePhaseCount   = desc.updatePhaseCount;
        out.updatePhaseIndex   = desc.updatePhaseIndex;
        out.maxSurfaceRoughness   = desc.maxSurfaceRoughness;
        out.maxPrimaryHitDistance = desc.maxPrimaryHitDistance;
        out.maxReflectionDistance = desc.maxReflectionTraceDistance;
        out.minReflectionEnergy   = desc.minReflectionEnergy;

        const auto& dl = desc.frameDesc.directionalLight;
        // Compute direction from yaw/pitch (towards the light, opposite of forward)
        {
            float fwd[3]{};
            Math::DirectionFromYawPitch(dl.yaw, dl.pitch, fwd);
            out.dirLightDir[0] = -fwd[0];
            out.dirLightDir[1] = -fwd[1];
            out.dirLightDir[2] = -fwd[2];
        }
        out.dirLightIntensity = dl.intensity;
        out.dirLightColor[0]  = dl.color[0];
        out.dirLightColor[1]  = dl.color[1];
        out.dirLightColor[2]  = dl.color[2];
        out.shadowBias        = 0.002f;
        out.ambientColor[0]   = 0.1f;
        out.ambientColor[1]   = 0.1f;
        out.ambientColor[2]   = 0.1f;
        out.ambientIntensity  = 1.0f;
        out.iblEnabled        = desc.iblEnabled ? 1.0f : 0.0f;
        out.iblIntensity      = desc.iblIntensity;
        out.iblPrefilterMaxMip = desc.iblPrefilterMaxMip;
        out.pad0              = 0.0f;
        out.pointLightCount   = 0u;
        out.spotLightCount    = 0u;
        out.samplesPerPixel   = std::max(1u, desc.samplesPerPixel);
        out.samplingMode      = std::min(desc.samplingMode, 2u);
        out.frameIndex        = m_restirFrameIndex;
        // GBuffer native resolution — may be larger than renderWidth/Height when the
        // reflection is rendered at a reduced resolution scale.  The CS uses these to
        // scale its Load() coordinates so each reflection pixel samples the correct
        // G-Buffer texel rather than a biased top-left region of the screen.
        out.gbufferWidth  = (desc.gbufferWidth  > 0) ? desc.gbufferWidth  : desc.width;
        out.gbufferHeight = (desc.gbufferHeight > 0) ? desc.gbufferHeight : desc.height;
        out.maxBounces    = std::max(1u, desc.maxBounces);
    }

    void GpuSoftwareRayTracer::FillAmbientOcclusionConstants(const AmbientOcclusionTextureDesc& desc,
                                                             AmbientOcclusionFrameConstants& out) const
    {
        memset(&out, 0, sizeof(out));
        memcpy(out.invVP, desc.inverseViewProjection, 64);
        out.cameraPos[0] = desc.cameraPosition[0];
        out.cameraPos[1] = desc.cameraPosition[1];
        out.cameraPos[2] = desc.cameraPosition[2];
        out.tMin         = desc.tMin;
        out.renderWidth  = desc.width;
        out.renderHeight = desc.height;
        out.sampleCount  = std::max(1u, desc.sampleCount);
        out.frameIndex   = desc.frameIndex;
        out.radius       = desc.radius;
        out.power        = desc.power;
        out.gbufferWidth  = (desc.gbufferWidth > 0u) ? desc.gbufferWidth : desc.width;
        out.gbufferHeight = (desc.gbufferHeight > 0u) ? desc.gbufferHeight : desc.height;
    }

    // =========================================================================
    // RenderDirectionalShadowMap
    // =========================================================================

    bool GpuSoftwareRayTracer::RenderDirectionalShadowMap(const DirectionalShadowMapDesc& desc,
                                                           IRHIDevice& device,
                                                           CommandList& cmdList,
                                                           Resource& outTexture,
                                                           RayTracingRuntimeStats& outStats)
    {
        (void)outStats;
        if (!m_initialized || !m_shadowPso || !outTexture.IsValid()) return false;
        if (desc.width == 0 || desc.height == 0) return false;

        ID3D12Device* dev = device.GetDevice();
        ID3D12GraphicsCommandList* cl = cmdList.Get();
        if (!dev || !cl) return false;

        // Update UAV slot [6] for shadow output
        UpdateTextureUav(dev, outTexture, DXGI_FORMAT_R32_FLOAT, kShadowUavSlot,
                         LightSystem::kDirectionalCascadeCount);

        // Fill shadow constants into slot 0 of the frame constants buffer
        {
            const UINT64 cbufOffset = static_cast<UINT64>(desc.constantBufferSlot) * 256u;
            ShadowFrameConstants sc{};
            FillShadowConstants(desc, sc);
            memcpy(m_frameConstantsMapped + cbufOffset, &sc, sizeof(sc));
        }

        // Bind compute pipeline
        cl->SetPipelineState(m_shadowPso.Get());
        cl->SetComputeRootSignature(m_rootSignature.Get());

        // Bind descriptor heap
        ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
        cl->SetDescriptorHeaps(1, heaps);

        // Root[0]: inline CBV (slot 0 of frame constants buffer)
        cl->SetComputeRootConstantBufferView(0,
            m_frameConstantsBuffer.GetGPUVirtualAddress() +
            static_cast<UINT64>(desc.constantBufferSlot) * 256u);

        // Root[1]: SRV table (t0-t5, starts at heap offset 0)
        cl->SetComputeRootDescriptorTable(1, m_descHeap.GetGPUDescriptorHandleForHeapStart());

        // Root[2]: UAV table (shadow slot)
        D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
        uavGpu.ptr += kShadowUavSlot * m_descIncrementSize;
        cl->SetComputeRootDescriptorTable(2, uavGpu);

        // Root[3]: G-Buffer SRV table (t6-t8) — shadow shader doesn't use t6-t8,
        // but the root signature requires all tables to be set. Point to heap[kGBufferSrvBase]
        // which holds null SRVs (safe to bind, never accessed by shadow CS).
        D3D12_GPU_DESCRIPTOR_HANDLE gbufGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
        gbufGpu.ptr += kGBufferSrvBase * m_descIncrementSize;
        cl->SetComputeRootDescriptorTable(3, gbufGpu);

        // Dispatch (16x16 threads)
        const UINT gx = (desc.width  + 15u) / 16u;
        const UINT gy = (desc.height + 15u) / 16u;
        cl->Dispatch(gx, gy, 1u);

        outStats.usingHardwarePath = false;
        outStats.renderWidth  = desc.width;
        outStats.renderHeight = desc.height;
        return true;
    }

    bool GpuSoftwareRayTracer::RenderAmbientOcclusionTexture(const AmbientOcclusionTextureDesc& desc,
                                                             IRHIDevice& device,
                                                             CommandList& cmdList,
                                                             Resource& outTexture,
                                                             RayTracingRuntimeStats& outStats)
    {
        (void)outStats;
        if (!m_initialized || !m_aoPso || !outTexture.IsValid()) return false;
        if (desc.width == 0 || desc.height == 0) return false;

        ID3D12Device* dev = device.GetDevice();
        ID3D12GraphicsCommandList* cl = cmdList.Get();
        if (!dev || !cl) return false;

        UpdateTextureUav(dev, outTexture, DXGI_FORMAT_R8G8B8A8_UNORM, kReflectionUavSlot);

        {
            AmbientOcclusionFrameConstants ao{};
            FillAmbientOcclusionConstants(desc, ao);
            memcpy(m_frameConstantsMapped, &ao, sizeof(ao));
        }

        // Bind GBuffer SRVs. The AO shader only reads t6, but the shared root signature
        // exposes t6-t8 as a single table so we populate all three slots.
        auto setRawSrv = [&](ID3D12Resource* resource, DXGI_FORMAT format, UINT slot) {
            D3D12_CPU_DESCRIPTOR_HANDLE dest = m_descHeap.GetCPUDescriptorHandleForHeapStart();
            dest.ptr += slot * m_descIncrementSize;

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(resource, &srvDesc, dest);
        };
        setRawSrv(desc.gbufferNormalTex, DXGI_FORMAT_R16G16B16A16_FLOAT, kGBufferSrvBase + 0u);
        setRawSrv(desc.gbufferMaterialTex, DXGI_FORMAT_R8G8B8A8_UNORM, kGBufferSrvBase + 1u);
        setRawSrv(desc.gbufferAlbedoTex, DXGI_FORMAT_R8G8B8A8_UNORM, kGBufferSrvBase + 2u);

        cl->SetPipelineState(m_aoPso.Get());
        cl->SetComputeRootSignature(m_rootSignature.Get());

        ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootConstantBufferView(0, m_frameConstantsBuffer.GetGPUVirtualAddress());
        cl->SetComputeRootDescriptorTable(1, m_descHeap.GetGPUDescriptorHandleForHeapStart());

        D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
        uavGpu.ptr += kReflectionUavSlot * m_descIncrementSize;
        cl->SetComputeRootDescriptorTable(2, uavGpu);

        D3D12_GPU_DESCRIPTOR_HANDLE gbufGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
        gbufGpu.ptr += kGBufferSrvBase * m_descIncrementSize;
        cl->SetComputeRootDescriptorTable(3, gbufGpu);

        const UINT gx = (desc.width + 15u) / 16u;
        const UINT gy = (desc.height + 15u) / 16u;
        cl->Dispatch(gx, gy, 1u);

        outStats.usingHardwarePath = false;
        outStats.renderWidth = desc.width;
        outStats.renderHeight = desc.height;
        return true;
    }

    // =========================================================================
    // RenderReflectionTextureReSTIR
    // =========================================================================

    bool GpuSoftwareRayTracer::RenderReflectionTextureReSTIR(
                                                        const ReflectionTextureDesc& desc,
                                                        IRHIDevice& device,
                                                        CommandList& cmdList,
                                                        Resource& outTexture,
                                                        RayTracingRuntimeStats& outStats)
    {
        (void)outStats;
        if (!m_restirPipelineReady || !outTexture.IsValid()) return false;
        if (desc.width == 0 || desc.height == 0) return false;

        ID3D12Device*              dev = device.GetDevice();
        ID3D12GraphicsCommandList* cl  = cmdList.Get();
        if (!dev || !cl) return false;

        // ---- Reallocate internal buffers if resolution changed ----
        if (desc.width != m_restirWidth || desc.height != m_restirHeight)
        {
            if (!AllocateReSTIRBuffers(device, desc.width, desc.height)) return false;
        }

        // ---- Initialize NRD on first frame (after buffers are allocated) ----
        if (!m_nrdReady && m_restirWidth > 0 && m_restirHeight > 0)
        {
            InitializeNrdIntegration(device, m_restirWidth, m_restirHeight);
        }

        // ---- Upload light data (persistently-mapped upload buffer) ----
        uint32_t nPt = 0, nSp = 0;
        if (desc.frameDesc.pointLights)
        {
            nPt = std::min((uint32_t)desc.frameDesc.pointLights->size(), kMaxPointLights);
            auto* dst = reinterpret_cast<GpuPointLightRT*>(m_lightDataMapped);
            for (uint32_t i = 0; i < nPt; ++i)
            {
                const auto& src = (*desc.frameDesc.pointLights)[i];
                dst[i].pos[0] = src.pos[0];
                dst[i].pos[1] = src.pos[1];
                dst[i].pos[2] = src.pos[2];
                dst[i].range  = src.range;
                dst[i].colorIntensity[0] = src.color[0] * src.intensity;
                dst[i].colorIntensity[1] = src.color[1] * src.intensity;
                dst[i].colorIntensity[2] = src.color[2] * src.intensity;
                dst[i].pad = 0.0f;
            }
        }
        if (desc.frameDesc.spotLights)
        {
            nSp = std::min((uint32_t)desc.frameDesc.spotLights->size(), kMaxSpotLights);
            uint8_t* spotBase = m_lightDataMapped
                + kMaxPointLights * sizeof(GpuPointLightRT);
            auto* dst = reinterpret_cast<GpuSpotLightRT*>(spotBase);
            for (uint32_t i = 0; i < nSp; ++i)
            {
                const auto& src = (*desc.frameDesc.spotLights)[i];
                dst[i].pos[0] = src.pos[0];
                dst[i].pos[1] = src.pos[1];
                dst[i].pos[2] = src.pos[2];
                dst[i].range  = src.range;
                float dir[3]{};
                Math::DirectionFromYawPitch(src.yaw, src.pitch, dir);
                dst[i].dir[0] = dir[0];
                dst[i].dir[1] = dir[1];
                dst[i].dir[2] = dir[2];
                dst[i].cosInner       = std::cos(src.innerAngle);
                dst[i].colorIntensity[0] = src.color[0] * src.intensity;
                dst[i].colorIntensity[1] = src.color[1] * src.intensity;
                dst[i].colorIntensity[2] = src.color[2] * src.intensity;
                dst[i].cosOuter       = std::cos(src.outerAngle);
            }
        }

        // ---- Fill constant buffer slots ----
        {
            ReSTIRFrameConstants base{};
            FillReSTIRConstants(desc, 1.0f, base);
            base.pointLightCount = nPt;
            base.spotLightCount  = nSp;
            memcpy(m_restirConstantsMapped, &base, sizeof(base));

            static const float kSteps[5] = { 1.f, 2.f, 4.f, 8.f, 16.f };
            for (int i = 0; i < 5; ++i)
            {
                ReSTIRFrameConstants ac{};
                FillReSTIRConstants(desc, kSteps[i], ac);
                ac.pointLightCount = nPt;
                ac.spotLightCount  = nSp;
                memcpy(m_restirConstantsMapped + (UINT64)(i + 1) * 256, &ac, sizeof(ac));
            }
            // Slot 6: shadow ReSTIR (same as base)
            memcpy(m_restirConstantsMapped + 6u * 256,
                   m_restirConstantsMapped, sizeof(ReSTIRFrameConstants));
        }

        // ---- Common heap / root state ----
        const UINT gx = (desc.width  + 15u) / 16u;
        const UINT gy = (desc.height + 15u) / 16u;

        ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(m_restirRootSignature.Get());

        const D3D12_GPU_DESCRIPTOR_HANDLE heapGpuStart =
            m_descHeap.GetGPUDescriptorHandleForHeapStart();

        D3D12_GPU_DESCRIPTOR_HANDLE bvhGpu = heapGpuStart;           // slots 0-5

        const D3D12_GPU_VIRTUAL_ADDRESS cbBase =
            m_restirConstantsBuffer.GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS ptLightsVA =
            m_lightDataBuffer.GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS spLightsVA =
            ptLightsVA + kMaxPointLights * sizeof(GpuPointLightRT);

        constexpr auto kSrv     = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        constexpr auto kUav     = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        constexpr auto kCopySrc = D3D12_RESOURCE_STATE_COPY_SOURCE;
        constexpr auto kCopyDst = D3D12_RESOURCE_STATE_COPY_DEST;
        constexpr D3D12_GPU_VIRTUAL_ADDRESS kNullVA = 0ull;

        // Per-pass descriptor slot helpers.
        // Slots are double-buffered by frame index so CPU can write frame N's descriptors
        // while the GPU is still executing frame N-1's dispatches, with no race condition
        // and no WaitForGPU required.
        // Frame slot layout: frameSlot s, pass p → SRV [14 + s*60 + 6p .. +3], UAV [.. +4 .. +5]
        const UINT frameSlot = m_restirFrameIndex & 1u;
        const UINT frameSlotOffset = frameSlot * kReSTIRPassCount * kReSTIRPassDescStride;
        auto PassSrvBase = [&](int p) -> UINT {
            return kReSTIRPassDescBase + frameSlotOffset + static_cast<UINT>(p) * kReSTIRPassDescStride;
        };
        auto PassUavBase = [&](int p) -> UINT {
            return PassSrvBase(p) + 4u;
        };
        auto PassSrvGpu = [&](int p) -> D3D12_GPU_DESCRIPTOR_HANDLE {
            D3D12_GPU_DESCRIPTOR_HANDLE h = heapGpuStart;
            h.ptr += PassSrvBase(p) * m_descIncrementSize;
            return h;
        };
        auto PassUavGpu = [&](int p) -> D3D12_GPU_DESCRIPTOR_HANDLE {
            D3D12_GPU_DESCRIPTOR_HANDLE h = heapGpuStart;
            h.ptr += PassUavBase(p) * m_descIncrementSize;
            return h;
        };
        auto WritePassSRV = [&](int p, UINT s, Resource& tex, DXGI_FORMAT fmt) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = m_descHeap.GetCPUDescriptorHandleForHeapStart();
            h.ptr += (PassSrvBase(p) + s) * m_descIncrementSize;
            CreateTex2DSrv(dev, tex.Get(), fmt, h);
        };
        auto WritePassSRVRaw = [&](int p, UINT s, ID3D12Resource* tex, DXGI_FORMAT fmt) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = m_descHeap.GetCPUDescriptorHandleForHeapStart();
            h.ptr += (PassSrvBase(p) + s) * m_descIncrementSize;
            CreateTex2DSrv(dev, tex, fmt, h);
        };
        auto WritePassUAV = [&](int p, UINT s, Resource& tex, DXGI_FORMAT fmt) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = m_descHeap.GetCPUDescriptorHandleForHeapStart();
            h.ptr += (PassUavBase(p) + s) * m_descIncrementSize;
            CreateTex2DUav(dev, tex.Get(), fmt, h);
        };

        // ---- Pre-write ALL per-pass descriptors before recording any dispatches ----
        // SetComputeRootDescriptorTable records a GPU heap pointer; the GPU reads the
        // actual descriptor at ExecuteCommandLists time. If multiple passes share the
        // same heap slots and the CPU overwrites them between SetDescriptorTable calls,
        // every pass sees the last-written value. Assigning unique slots per pass and
        // writing them all up front makes each pass reference stable, independent data.

        // Pass 0 (Initial): UAV[0]=gBuffer
        WritePassUAV(0, 0u, m_gBufferTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        // Pass 1 (Temporal): SRV[0]=gBuffer, SRV[1]=prevGBuffer, SRV[2]=materialTex(roughness)
        WritePassSRV(1, 0u, m_gBufferTexture,     DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassSRV(1, 1u, m_prevGBufferTexture,  DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (desc.gbufferMaterialTex)
            WritePassSRVRaw(1, 2u, desc.gbufferMaterialTex, DXGI_FORMAT_R8G8B8A8_UNORM);
        // Pass 2 (Spatial): SRV[0]=gBuffer, SRV[1]=materialTex(roughness)
        WritePassSRV(2, 0u, m_gBufferTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (desc.gbufferMaterialTex)
            WritePassSRVRaw(2, 1u, desc.gbufferMaterialTex, DXGI_FORMAT_R8G8B8A8_UNORM);
        // Pass 3 (Shade): SRV[0]=gBuffer, SRV[1]=materialTex, SRV[2]=albedoTex; UAV[0]=shadedColor
        WritePassSRV(3, 0u, m_gBufferTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (desc.gbufferMaterialTex)
            WritePassSRVRaw(3, 1u, desc.gbufferMaterialTex, DXGI_FORMAT_R8G8B8A8_UNORM);
        if (desc.gbufferAlbedoTex)
            WritePassSRVRaw(3, 2u, desc.gbufferAlbedoTex,   DXGI_FORMAT_R8G8B8A8_UNORM);
        WritePassUAV(3, 0u, m_shadedColorTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        // Pass 4 (Shade temporal accumulate into prevColor): SRV[0]=shadedColor, SRV[1]=prevColor
        WritePassSRV(4, 0u, m_shadedColorTexture, DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassSRV(4, 1u, m_prevColorTexture,   DXGI_FORMAT_R16G16B16A16_FLOAT);
        // Pass 5 (NRD Pack): SRV[0]=shadedColor, SRV[1]=gBuffer, SRV[2]=material
        //                     UAV[0]=nrdDiffIn, UAV[1]=nrdViewZ, UAV[2]=nrdNormalRoughness
        WritePassSRV(5, 0u, m_shadedColorTexture,  DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassSRV(5, 1u, m_gBufferTexture,       DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (desc.gbufferMaterialTex)
            WritePassSRVRaw(5, 2u, desc.gbufferMaterialTex, DXGI_FORMAT_R8G8B8A8_UNORM);
        WritePassUAV(5, 0u, m_nrdDiffIn,           DXGI_FORMAT_R16G16B16A16_FLOAT);
        WritePassUAV(5, 1u, m_nrdViewZ,             DXGI_FORMAT_R16_FLOAT);
        WritePassUAV(5, 2u, m_nrdNormalRoughness,   DXGI_FORMAT_R8G8B8A8_UNORM);
        // Pass 6 (A-Trous ping→pong): SRV[0]=nrdDiffOut, SRV[1]=gBuffer; UAV[0]=atrousPingPong
        if (m_atrousReady)
        {
            WritePassSRV(6, 0u, m_nrdDiffOut,      DXGI_FORMAT_R16G16B16A16_FLOAT);
            WritePassSRV(6, 1u, m_gBufferTexture,   DXGI_FORMAT_R16G16B16A16_FLOAT);
            WritePassUAV(6, 0u, m_atrousPingPong,   DXGI_FORMAT_R16G16B16A16_FLOAT);
            // Pass 7 (A-Trous pong→ping): SRV[0]=atrousPingPong, SRV[1]=gBuffer; UAV[0]=nrdDiffOut
            WritePassSRV(7, 0u, m_atrousPingPong,   DXGI_FORMAT_R16G16B16A16_FLOAT);
            WritePassSRV(7, 1u, m_gBufferTexture,   DXGI_FORMAT_R16G16B16A16_FLOAT);
            WritePassUAV(7, 0u, m_nrdDiffOut,       DXGI_FORMAT_R16G16B16A16_FLOAT);
        }

        // Helper: bind all 9 root parameters with per-pass descriptor tables
        auto BindRoots = [&](D3D12_GPU_VIRTUAL_ADDRESS cbVA,
                             D3D12_GPU_VIRTUAL_ADDRESS lightPtVA,
                             D3D12_GPU_VIRTUAL_ADDRESS lightSpVA,
                             D3D12_GPU_VIRTUAL_ADDRESS reservoirInVA,
                             D3D12_GPU_VIRTUAL_ADDRESS reservoirOutVA,
                             D3D12_GPU_DESCRIPTOR_HANDLE passSrvGpuH,
                             D3D12_GPU_DESCRIPTOR_HANDLE passUavGpuH,
                             D3D12_GPU_VIRTUAL_ADDRESS prevTemporalVA)
        {
            cl->SetComputeRootConstantBufferView (0, cbVA);
            cl->SetComputeRootDescriptorTable    (1, bvhGpu);
            cl->SetComputeRootDescriptorTable    (2, passSrvGpuH);
            cl->SetComputeRootDescriptorTable    (3, passUavGpuH);
            cl->SetComputeRootShaderResourceView (4, lightPtVA);
            cl->SetComputeRootShaderResourceView (5, lightSpVA);
            cl->SetComputeRootShaderResourceView (6, reservoirInVA);
            cl->SetComputeRootUnorderedAccessView(7, reservoirOutVA);
            cl->SetComputeRootShaderResourceView (8, prevTemporalVA);
        };

        // =========================================================================
        // Pass 1: GBuffer capture + Initial Reservoir (WRS M=8)
        // u0 = gBuffer (scratch UAV[0]),  u2 = reservoirBuffer[0] (inline UAV)
        // t12/t13 = lights,  BVH t0-t5 used for primary rays
        // =========================================================================
        {
            D3D12_RESOURCE_BARRIER pre[] = {
                MakeTransition(m_gBufferTexture.Get(),     kSrv, kUav),
                MakeTransition(m_reservoirBuffer[0].Get(), kSrv, kUav),
            };
            cl->ResourceBarrier(2, pre);

            cl->SetPipelineState(m_restirInitialPso.Get());
            BindRoots(cbBase, ptLightsVA, spLightsVA,
                      kNullVA, m_reservoirBuffer[0].GetGPUVirtualAddress(),
                      PassSrvGpu(0), PassUavGpu(0), kNullVA);
            cl->Dispatch(gx, gy, 1u);

            D3D12_RESOURCE_BARRIER post[] = {
                MakeTransition(m_gBufferTexture.Get(),     kUav, kSrv),
                MakeTransition(m_reservoirBuffer[0].Get(), kUav, kSrv),
            };
            cl->ResourceBarrier(2, post);
        }

        // =========================================================================
        // Pass 2: Temporal Reservoir Reuse  (ping-pong fix)
        // t6  = gBuffer (scratch SRV[0]),  t7 = prevGBuffer (scratch SRV[1])
        // t14 = reservoirBuffer[0] (current-frame initial, inline SRV)
        // t15 = reservoirBuffer[prevTemporalIdx] (prev-frame temporal output, inline SRV)
        // u2  = reservoirBuffer[temporalWriteIdx] (temporal output, inline UAV)
        //
        // Ping-pong:  even frame → write [2], prev = [1]
        //             odd  frame → write [1], prev = [2]
        // =========================================================================
        {
            const uint32_t temporalWriteIdx = (m_restirFrameIndex % 2u == 0u) ? 2u : 1u;
            const uint32_t prevTemporalIdx  = (m_restirFrameIndex % 2u == 0u) ? 1u : 2u;

            D3D12_RESOURCE_BARRIER pre[] = {
                MakeTransition(m_reservoirBuffer[temporalWriteIdx].Get(), kSrv, kUav),
            };
            cl->ResourceBarrier(1, pre);

            cl->SetPipelineState(m_restirTemporalPso.Get());
            BindRoots(cbBase, kNullVA, kNullVA,
                      m_reservoirBuffer[0].GetGPUVirtualAddress(),
                      m_reservoirBuffer[temporalWriteIdx].GetGPUVirtualAddress(),
                      PassSrvGpu(1), PassUavGpu(1),
                      m_reservoirBuffer[prevTemporalIdx].GetGPUVirtualAddress());
            cl->Dispatch(gx, gy, 1u);

            D3D12_RESOURCE_BARRIER post[] = {
                MakeTransition(m_reservoirBuffer[temporalWriteIdx].Get(), kUav, kSrv),
            };
            cl->ResourceBarrier(1, post);

        // =========================================================================
        // Pass 3: Spatial Reservoir Reuse
        // t6  = gBuffer (scratch SRV[0])
        // t14 = reservoirBuffer[temporalWriteIdx] (temporal output, inline SRV)
        // u2  = reservoirBuffer[0] (spatial output → shade input, inline UAV)
        // =========================================================================
            D3D12_RESOURCE_BARRIER pre3[] = {
                MakeTransition(m_reservoirBuffer[0].Get(), kSrv, kUav),
            };
            cl->ResourceBarrier(1, pre3);

            cl->SetPipelineState(m_restirSpatialPso.Get());
            BindRoots(cbBase, kNullVA, kNullVA,
                      m_reservoirBuffer[temporalWriteIdx].GetGPUVirtualAddress(),
                      m_reservoirBuffer[0].GetGPUVirtualAddress(),
                      PassSrvGpu(2), PassUavGpu(2), kNullVA);
            cl->Dispatch(gx, gy, 1u);

            D3D12_RESOURCE_BARRIER post3[] = {
                MakeTransition(m_reservoirBuffer[0].Get(), kUav, kSrv),
            };
            cl->ResourceBarrier(1, post3);
        }

        // =========================================================================
        // Pass 4: Final Shading (shadow ray + PBR)
        // t6 = gBuffer (scratch SRV[0]),  u0 = shadedColor (scratch UAV[0])
        // t12/t13 = lights,  t14 = reservoirBuffer[0] (inline SRV)
        // =========================================================================
        {
            D3D12_RESOURCE_BARRIER pre[] = {
                MakeTransition(m_shadedColorTexture.Get(), kSrv, kUav),
            };
            cl->ResourceBarrier(1, pre);

            cl->SetPipelineState(m_restirShadePso.Get());
            BindRoots(cbBase, ptLightsVA, spLightsVA,
                      m_reservoirBuffer[0].GetGPUVirtualAddress(),
                      kNullVA,
                      PassSrvGpu(3), PassUavGpu(3), kNullVA);
            cl->Dispatch(gx, gy, 1u);

            D3D12_RESOURCE_BARRIER post[] = {
                MakeTransition(m_shadedColorTexture.Get(), kUav, kSrv),
            };
            cl->ResourceBarrier(1, post);
        }

        // =========================================================================
        // Pass 5: NRD Pack – convert shaded color + GBuffer into NRD input textures
        // t6 = shadedColor  t7 = gBuffer  t8 = material
        // u0 = nrdDiffIn   u1 = nrdViewZ  u2 = nrdNormalRoughness
        // =========================================================================
        {
            D3D12_RESOURCE_BARRIER pre[] = {
                MakeTransition(m_nrdDiffIn.Get(),          kSrv, kUav),
                MakeTransition(m_nrdViewZ.Get(),            kSrv, kUav),
                MakeTransition(m_nrdNormalRoughness.Get(),  kSrv, kUav),
            };
            cl->ResourceBarrier(3, pre);

            cl->SetPipelineState(m_nrdPackPso.Get());
            BindRoots(cbBase, kNullVA, kNullVA, kNullVA, kNullVA,
                      PassSrvGpu(5), PassUavGpu(5), kNullVA);
            cl->Dispatch(gx, gy, 1u);

            D3D12_RESOURCE_BARRIER post[] = {
                MakeTransition(m_nrdDiffIn.Get(),          kUav, kSrv),
                MakeTransition(m_nrdViewZ.Get(),            kUav, kSrv),
                MakeTransition(m_nrdNormalRoughness.Get(),  kUav, kSrv),
            };
            cl->ResourceBarrier(3, post);
        }

        // =========================================================================
        // NRD RELAX_DIFFUSE denoise
        // =========================================================================
        if (m_nrdReady && m_nrdIntegration)
        {
            m_nrdIntegration->NewFrame();

            nrd::CommonSettings commonSettings = {};
            // Provide identity matrices – proper view/projection matrices improve quality
            // but NRD works without them (temporal accumulation is degraded only).
            commonSettings.frameIndex = m_restirFrameIndex;
            commonSettings.resourceSize[0]     = static_cast<uint16_t>(desc.width);
            commonSettings.resourceSize[1]     = static_cast<uint16_t>(desc.height);
            commonSettings.resourceSizePrev[0] = static_cast<uint16_t>(desc.width);
            commonSettings.resourceSizePrev[1] = static_cast<uint16_t>(desc.height);
            commonSettings.rectSize[0]     = static_cast<uint16_t>(desc.width);
            commonSettings.rectSize[1]     = static_cast<uint16_t>(desc.height);
            commonSettings.rectSizePrev[0] = static_cast<uint16_t>(desc.width);
            commonSettings.rectSizePrev[1] = static_cast<uint16_t>(desc.height);
            commonSettings.motionVectorScale[0] = 1.0f;
            commonSettings.motionVectorScale[1] = 1.0f;
            commonSettings.motionVectorScale[2] = 0.0f; // 2D screen-space MV
            if (desc.cameraChanged)
                commonSettings.accumulationMode = nrd::AccumulationMode::RESTART;

            m_nrdIntegration->SetCommonSettings(commonSettings);

            nrd::RelaxSettings relaxSettings = {};
            m_nrdIntegration->SetDenoiserSettings(0u, &relaxSettings);

            // Transition nrdDiffOut to UAV so NRD can write to it
            {
                D3D12_RESOURCE_BARRIER b = MakeTransition(m_nrdDiffOut.Get(), kSrv, kUav);
                cl->ResourceBarrier(1, &b);
            }

            nrd::ResourceSnapshot resourceSnapshot;
            resourceSnapshot.restoreInitialState = false;

            auto MakeNrdResource = [](ID3D12Resource* res, DXGI_FORMAT fmt,
                                      nri::AccessBits access, nri::Layout layout,
                                      nri::StageBits stages) -> nrd::Resource
            {
                nrd::Resource r{};
                r.d3d12.resource = res;
                r.d3d12.format   = static_cast<DXGIFormat>(fmt);
                r.state.access   = access;
                r.state.layout   = layout;
                r.state.stages   = stages;
                return r;
            };

            constexpr auto kSrvAccess  = nri::AccessBits::SHADER_RESOURCE;
            constexpr auto kUavAccess  = nri::AccessBits::SHADER_RESOURCE_STORAGE;
            constexpr auto kSrvLayout  = nri::Layout::SHADER_RESOURCE;
            constexpr auto kUavLayout  = nri::Layout::SHADER_RESOURCE_STORAGE;
            constexpr auto kCompStage  = nri::StageBits::COMPUTE_SHADER;

            nrd::Resource resDiffIn = MakeNrdResource(
                m_nrdDiffIn.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                kSrvAccess, kSrvLayout, kCompStage);
            nrd::Resource resViewZ = MakeNrdResource(
                m_nrdViewZ.Get(), DXGI_FORMAT_R16_FLOAT,
                kSrvAccess, kSrvLayout, kCompStage);
            nrd::Resource resNormRough = MakeNrdResource(
                m_nrdNormalRoughness.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                kSrvAccess, kSrvLayout, kCompStage);
            nrd::Resource resMV = MakeNrdResource(
                m_nrdMotionVec.Get(), DXGI_FORMAT_R16G16_FLOAT,
                kSrvAccess, kSrvLayout, kCompStage);
            nrd::Resource resDiffOut = MakeNrdResource(
                m_nrdDiffOut.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                kUavAccess, kUavLayout, kCompStage);

            resourceSnapshot.SetResource(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, resDiffIn);
            resourceSnapshot.SetResource(nrd::ResourceType::IN_VIEWZ,                 resViewZ);
            resourceSnapshot.SetResource(nrd::ResourceType::IN_NORMAL_ROUGHNESS,      resNormRough);
            resourceSnapshot.SetResource(nrd::ResourceType::IN_MV,                    resMV);
            resourceSnapshot.SetResource(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, resDiffOut);

            nri::CommandBufferD3D12Desc cmdBufDesc = {};
            cmdBufDesc.d3d12CommandList      = cl;
            cmdBufDesc.d3d12CommandAllocator = nullptr; // optional

            const nrd::Identifier denoiser0 = 0u;
            m_nrdIntegration->DenoiseD3D12(&denoiser0, 1u, cmdBufDesc, resourceSnapshot);

            // Re-bind our descriptor heap (NRD replaces it during Denoise)
            ID3D12DescriptorHeap* ownHeap = m_descHeap.Get();
            cl->SetDescriptorHeaps(1, &ownHeap);
            cl->SetComputeRootSignature(m_restirRootSignature.Get());

            // Transition nrdDiffOut back to SRV for the copy below
            {
                D3D12_RESOURCE_BARRIER b = MakeTransition(m_nrdDiffOut.Get(), kUav, kSrv);
                cl->ResourceBarrier(1, &b);
            }
        }
        else
        {
            // NRD not ready: fall back to copying shadedColor directly
        }

        // =========================================================================
        // A-Trous spatial denoising (3 passes: step=1,2,4) on NRD output
        // Pass 6: nrdDiffOut(SRV) → atrousPingPong(UAV)
        // Pass 7: atrousPingPong(SRV) → nrdDiffOut(UAV)
        // Pass 6: nrdDiffOut(SRV) → atrousPingPong(UAV)  ← final result in atrousPingPong
        // =========================================================================
        if (m_atrousReady && m_restirATrousPso && m_nrdReady)
        {
            // nrdDiffOut is already in SRV state after NRD. Run 5 A-Trous passes (step=1,2,4,8,16)
            // ping-ponging between nrdDiffOut and atrousPingPong (SVGF: Schied et al. 2017).
            // Final result lands in atrousPingPong (5 passes = odd, ping path last).
            {
                for (int ai = 0; ai < 5; ++ai)
                {
                    const bool pingPass = (ai % 2 == 0); // true = Pass6 (nrdDiffOut→pong), false = Pass7 (pong→nrdDiffOut)
                    const D3D12_GPU_VIRTUAL_ADDRESS cbATrous = cbBase + (UINT64)(ai + 1) * 256u;
                    ID3D12Resource* readTex  = pingPass ? m_nrdDiffOut.Get()     : m_atrousPingPong.Get();
                    ID3D12Resource* writeTex = pingPass ? m_atrousPingPong.Get() : m_nrdDiffOut.Get();
                    int passIdx = pingPass ? 6 : 7;

                    D3D12_RESOURCE_BARRIER pre = MakeTransition(writeTex, kSrv, kUav);
                    cl->ResourceBarrier(1, &pre);
                    (void)readTex; // SRV state maintained from previous barrier

                    cl->SetPipelineState(m_restirATrousPso.Get());
                    BindRoots(cbATrous, kNullVA, kNullVA, kNullVA, kNullVA,
                              PassSrvGpu(passIdx), PassUavGpu(passIdx), kNullVA);
                    cl->Dispatch(gx, gy, 1u);

                    D3D12_RESOURCE_BARRIER post = MakeTransition(writeTex, kUav, kSrv);
                    cl->ResourceBarrier(1, &post);
                }
                // After 5 passes: final result is in atrousPingPong (SRV state).
            }
        }

        // =========================================================================
        // Copy NRD denoised result (or shadedColor fallback) → outTexture + prevColor
        // =========================================================================
        {
            // After A-Trous: use atrousPingPong if available, else nrdDiffOut or shadedColor
            ID3D12Resource* srcTex;
            if (m_nrdReady && m_atrousReady)
                srcTex = m_atrousPingPong.Get();
            else if (m_nrdReady)
                srcTex = m_nrdDiffOut.Get();
            else
                srcTex = m_shadedColorTexture.Get();

            D3D12_RESOURCE_BARRIER pre[] = {
                MakeTransition(srcTex,                   kSrv,                               kCopySrc),
                MakeTransition(outTexture.Get(),         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kCopyDst),
                MakeTransition(m_prevColorTexture.Get(), kSrv,                               kCopyDst),
            };
            cl->ResourceBarrier(3, pre);

            cl->CopyResource(outTexture.Get(),         srcTex);
            cl->CopyResource(m_prevColorTexture.Get(), srcTex);

            D3D12_RESOURCE_BARRIER post[] = {
                MakeTransition(srcTex,                   kCopySrc, kSrv),
                MakeTransition(outTexture.Get(),         kCopyDst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                MakeTransition(m_prevColorTexture.Get(), kCopyDst, kSrv),
            };
            cl->ResourceBarrier(3, post);
        }

        // =========================================================================
        // Copy gBuffer → prevGBuffer for next frame reprojection
        // =========================================================================
        {
            D3D12_RESOURCE_BARRIER pre[] = {
                MakeTransition(m_gBufferTexture.Get(),     kSrv, kCopySrc),
                MakeTransition(m_prevGBufferTexture.Get(), kSrv, kCopyDst),
            };
            cl->ResourceBarrier(2, pre);

            cl->CopyResource(m_prevGBufferTexture.Get(), m_gBufferTexture.Get());

            D3D12_RESOURCE_BARRIER post[] = {
                MakeTransition(m_gBufferTexture.Get(),     kCopySrc, kSrv),
                MakeTransition(m_prevGBufferTexture.Get(), kCopyDst, kSrv),
            };
            cl->ResourceBarrier(2, post);
        }

        // =========================================================================
        // Advance per-frame state
        // =========================================================================
        ++m_restirFrameIndex;
        Math::Invert4x4(desc.frameDesc.inverseViewProjection, m_prevVP);
        m_prevReflectionCameraPos[0] = desc.frameDesc.cameraPosition[0];
        m_prevReflectionCameraPos[1] = desc.frameDesc.cameraPosition[1];
        m_prevReflectionCameraPos[2] = desc.frameDesc.cameraPosition[2];

        outStats.usingHardwarePath = false;
        outStats.renderWidth  = desc.width;
        outStats.renderHeight = desc.height;
        return true;
    }

    // =========================================================================
    // RenderShadowReSTIR
    // Dispatches SWRT_Shadow_ReSTIR_CS using the GBuffer populated by the most
    // recent RenderReflectionTextureReSTIR call.
    // outTexture: UAV on entry/exit (R16G16B16A16_FLOAT).
    // =========================================================================

    bool GpuSoftwareRayTracer::RenderShadowReSTIR(const ReflectionTextureDesc& desc,
                                                   IRHIDevice& device,
                                                   CommandList& cmdList,
                                                   Resource& outTexture,
                                                   RayTracingRuntimeStats& outStats)
    {
        (void)outStats;
        if (!m_restirPipelineReady || !outTexture.IsValid()) return false;
        if (!m_gBufferTexture.IsValid()) return false;
        if (desc.width == 0 || desc.height == 0) return false;

        ID3D12Device*              dev = device.GetDevice();
        ID3D12GraphicsCommandList* cl  = cmdList.Get();
        if (!dev || !cl) return false;

        // ---- Common setup ----
        const UINT gx = (desc.width  + 15u) / 16u;
        const UINT gy = (desc.height + 15u) / 16u;

        ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(m_restirRootSignature.Get());

        const D3D12_GPU_DESCRIPTOR_HANDLE heapGpuStart =
            m_descHeap.GetGPUDescriptorHandleForHeapStart();

        D3D12_GPU_DESCRIPTOR_HANDLE bvhGpu = heapGpuStart;  // slots 0-5

        D3D12_GPU_DESCRIPTOR_HANDLE scratchSrvGpu = heapGpuStart;
        scratchSrvGpu.ptr += kScratchSrvBase * m_descIncrementSize;

        D3D12_GPU_DESCRIPTOR_HANDLE scratchUavGpu = heapGpuStart;
        scratchUavGpu.ptr += kScratchUavBase * m_descIncrementSize;

        const D3D12_GPU_VIRTUAL_ADDRESS cbSlot6 =
            m_restirConstantsBuffer.GetGPUVirtualAddress() + 6u * 256u;
        const D3D12_GPU_VIRTUAL_ADDRESS ptLightsVA =
            m_lightDataBuffer.GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS spLightsVA =
            ptLightsVA + kMaxPointLights * sizeof(GpuPointLightRT);

        constexpr auto kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        constexpr auto kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        constexpr D3D12_GPU_VIRTUAL_ADDRESS kNullVA = 0ull;

        // ---- GBuffer: UAV→SRV (was left in SRV state after ReSTIR passes) ----
        // gBufferTexture is already SRV after RenderReflectionTextureReSTIR; just bind it.
        SetScratchTexSRV(dev, m_gBufferTexture, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);

        // ---- outTexture: must be in UAV state (caller guarantee) ----
        SetScratchTexUAV(dev, outTexture, DXGI_FORMAT_R16G16B16A16_FLOAT, 0);

        // ---- Bind and dispatch ----
        cl->SetPipelineState(m_shadowReSTIRPso.Get());
        cl->SetComputeRootConstantBufferView (0, cbSlot6);
        cl->SetComputeRootDescriptorTable    (1, bvhGpu);
        cl->SetComputeRootDescriptorTable    (2, scratchSrvGpu);
        cl->SetComputeRootDescriptorTable    (3, scratchUavGpu);
        cl->SetComputeRootShaderResourceView (4, ptLightsVA);
        cl->SetComputeRootShaderResourceView (5, spLightsVA);
        cl->SetComputeRootShaderResourceView (6, kNullVA);
        cl->SetComputeRootUnorderedAccessView(7, kNullVA);
        cl->SetComputeRootShaderResourceView (8, kNullVA);
        cl->Dispatch(gx, gy, 1u);

        return true;
    }

    // =========================================================================
    // RenderReflectionTexture
    // =========================================================================

    bool GpuSoftwareRayTracer::RenderReflectionTexture(const ReflectionTextureDesc& desc,
                                                        IRHIDevice& device,
                                                        CommandList& cmdList,
                                                        Resource& outTexture,
                                                        RayTracingRuntimeStats& outStats)
    {
        (void)outStats;
        if (!m_initialized || !outTexture.IsValid()) return false;
        if (desc.width == 0 || desc.height == 0) return false;

        // Route through ReSTIR + SVGF pipeline when mode is ReSTIR and pipeline is ready
        if (m_swrtMode == SwrtMode::ReSTIR && m_restirPipelineReady)
            return RenderReflectionTextureReSTIR(desc, device, cmdList, outTexture, outStats);

        if (!m_reflectionPso) return false;

        ID3D12Device* dev = device.GetDevice();
        ID3D12GraphicsCommandList* cl = cmdList.Get();
        if (!dev || !cl) return false;

        // Update UAV slot [7] for reflection output
        UpdateTextureUav(dev, outTexture, DXGI_FORMAT_R16G16B16A16_FLOAT, kReflectionUavSlot);

        // Fill reflection constants into slot 1 of the frame constants buffer (offset 256)
        {
            ReflectionFrameConstants rc{};
            FillReflectionConstants(desc, rc);
            memcpy(m_frameConstantsMapped + 256u, &rc, sizeof(rc));
        }

        // Bind compute pipeline
        cl->SetPipelineState(m_reflectionPso.Get());
        cl->SetComputeRootSignature(m_rootSignature.Get());

        // Bind descriptor heap
        ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
        cl->SetDescriptorHeaps(1, heaps);

        // Root[0]: inline CBV (slot 1 of frame constants buffer)
        cl->SetComputeRootConstantBufferView(0,
            m_frameConstantsBuffer.GetGPUVirtualAddress() + 256u);

        // Root[1]: SRV table (t0-t5)
        cl->SetComputeRootDescriptorTable(1, m_descHeap.GetGPUDescriptorHandleForHeapStart());

        // Root[2]: UAV table (reflection slot)
        D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
        uavGpu.ptr += kReflectionUavSlot * m_descIncrementSize;
        cl->SetComputeRootDescriptorTable(2, uavGpu);

        // Root[3]: G-Buffer SRV table (t6-t8): Normal, Material, Albedo.
        // Populate heap slots kGBufferSrvBase+0..+3 from desc.
        // Null SRVs are written when the texture pointer is null (shader outputs black).
        {
            auto writeGBufSrv = [&](ID3D12Resource* tex, DXGI_FORMAT fmt, UINT slotOffset)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE h = m_descHeap.GetCPUDescriptorHandleForHeapStart();
                h.ptr += (kGBufferSrvBase + slotOffset) * m_descIncrementSize;
                if (tex)
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                    srvDesc.Format                  = fmt;
                    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srvDesc.Texture2D.MipLevels     = 1;
                    dev->CreateShaderResourceView(tex, &srvDesc, h);
                }
                else
                {
                    // Null descriptor: shader reads (0,0,0,0)
                    D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc{};
                    nullDesc.Format                  = fmt;
                    nullDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
                    nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    nullDesc.Texture2D.MipLevels     = 1;
                    dev->CreateShaderResourceView(nullptr, &nullDesc, h);
                }
            };
            auto writeCubeSrv = [&](ID3D12Resource* tex, DXGI_FORMAT fmt, UINT slotOffset, UINT mipLevels)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE h = m_descHeap.GetCPUDescriptorHandleForHeapStart();
                h.ptr += (kGBufferSrvBase + slotOffset) * m_descIncrementSize;
                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.Format                  = fmt;
                srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.TextureCube.MostDetailedMip = 0;
                srvDesc.TextureCube.MipLevels       = mipLevels;
                srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
                dev->CreateShaderResourceView(tex, &srvDesc, h);
            };
            writeGBufSrv(desc.gbufferNormalTex,   DXGI_FORMAT_R16G16B16A16_FLOAT, 0u);
            writeGBufSrv(desc.gbufferMaterialTex, DXGI_FORMAT_R8G8B8A8_UNORM,     1u);
            writeGBufSrv(desc.gbufferAlbedoTex,   DXGI_FORMAT_R8G8B8A8_UNORM,     2u);
            writeCubeSrv(desc.iblPrefilterTex,    DXGI_FORMAT_R16G16B16A16_FLOAT, 3u,
                         std::max(1u, static_cast<UINT>(desc.iblPrefilterMaxMip) + 1u));
        }
        D3D12_GPU_DESCRIPTOR_HANDLE gbufGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
        gbufGpu.ptr += kGBufferSrvBase * m_descIncrementSize;
        cl->SetComputeRootDescriptorTable(3, gbufGpu);

        // Dispatch (16x16 threads)
        const UINT gx = (desc.width  + 15u) / 16u;
        const UINT gy = (desc.height + 15u) / 16u;
        cl->Dispatch(gx, gy, 1u);

        outStats.usingHardwarePath = false;
        outStats.renderWidth  = desc.width;
        outStats.renderHeight = desc.height;

        // -----------------------------------------------------------------------
        // Temporal EMA pass (Legacy mode only)
        // Blends current-frame reflection with accumulated history to reduce noise.
        // Only runs on full-refresh frames (updatePhaseCount == 1) to avoid
        // blending stale partial-update pixels into the history.
        // -----------------------------------------------------------------------
        if (desc.denoiserEnabled && m_reflTemporalPso && desc.updatePhaseCount == 1u)
        {
            bool firstFrame = (m_reflHistoryWidth  != desc.width ||
                               m_reflHistoryHeight != desc.height);

                // Lazily allocate / reallocate ping-pong history textures on size change
                if (firstFrame)
                {
                    const auto kSrv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    m_reflHistoryA.Reset();
                    m_reflHistoryB.Reset();
                    m_reflHistoryMetaA.Reset();
                    m_reflHistoryMetaB.Reset();
                    m_reflHistoryMaterialA.Reset();
                    m_reflHistoryMaterialB.Reset();
                    m_reflAtrousScratch.Reset();
                    const bool allocOk =
                        CreateUavTexture2D(device, desc.width, desc.height,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_reflHistoryA) &&
                        CreateUavTexture2D(device, desc.width, desc.height,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_reflHistoryB) &&
                        CreateUavTexture2D(device, desc.width, desc.height,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_reflHistoryMetaA) &&
                        CreateUavTexture2D(device, desc.width, desc.height,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_reflHistoryMetaB) &&
                        CreateUavTexture2D(device, desc.width, desc.height,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_reflHistoryMaterialA) &&
                        CreateUavTexture2D(device, desc.width, desc.height,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_reflHistoryMaterialB) &&
                        CreateUavTexture2D(device, desc.width, desc.height,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv, m_reflAtrousScratch);
                if (allocOk)
                {
                    m_reflHistoryWidth  = desc.width;
                    m_reflHistoryHeight = desc.height;
                    m_reflHistoryPingA  = true;
                }
                else
                {
                    firstFrame = false;  // prevent temporal dispatch below
                    m_reflHistoryWidth = m_reflHistoryHeight = 0;
                }
            }

            if (m_reflHistoryA.IsValid() && m_reflHistoryB.IsValid() &&
                m_reflHistoryMetaA.IsValid() && m_reflHistoryMetaB.IsValid() &&
                m_reflHistoryMaterialA.IsValid() && m_reflHistoryMaterialB.IsValid())
            {
                // Ping-pong assignment: one side is read (SRV), the other is written (UAV)
                Resource& histRead  = m_reflHistoryPingA ? m_reflHistoryB : m_reflHistoryA;
                Resource& histWrite = m_reflHistoryPingA ? m_reflHistoryA : m_reflHistoryB;
                Resource& metaRead  = m_reflHistoryPingA ? m_reflHistoryMetaB : m_reflHistoryMetaA;
                Resource& metaWrite = m_reflHistoryPingA ? m_reflHistoryMetaA : m_reflHistoryMetaB;
                Resource& materialRead  = m_reflHistoryPingA ? m_reflHistoryMaterialB : m_reflHistoryMaterialA;
                Resource& materialWrite = m_reflHistoryPingA ? m_reflHistoryMaterialA : m_reflHistoryMaterialB;

                // Keep some history during camera motion instead of fully resetting.
                // A hard reset exposes the raw 1spp/low-res reflection and causes
                // black flicker while the camera is being shaken.
                const float alpha = firstFrame ? 1.0f : (desc.cameraChanged ? 0.35f : desc.temporalAlpha);

                // ---- Barriers: prepare textures for temporal CS ----
                // outTexture: UAV (just written by reflection CS) → NON_PIXEL_SHADER_RESOURCE (SRV t0)
                // histWrite:  NON_PIXEL_SHADER_RESOURCE → UAV (u0)
                D3D12_RESOURCE_BARRIER preBarriers[4] = {
                    CD3DX12_RESOURCE_BARRIER::UAV(outTexture.Get()),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        histWrite.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        metaWrite.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        materialWrite.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                };
                cl->ResourceBarrier(_countof(preBarriers), preBarriers);

                // Transition outTexture UAV → SRV (done after UAV barrier above)
                D3D12_RESOURCE_BARRIER outToSrv = CD3DX12_RESOURCE_BARRIER::Transition(
                    outTexture.Get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                cl->ResourceBarrier(1u, &outToSrv);

                // ---- Populate temporal descriptor slots (14-22) ----
                // Slot 14: SRV → outTexture   (t0 = current frame)
                // Slot 15: SRV → histRead     (t1 = previous EMA)
                // Slot 16: SRV → current meta (t2 = current surface metadata)
                // Slot 17: SRV → history meta (t3 = previous surface metadata)
                // Slot 18: SRV → current material (t4 = current material metadata)
                // Slot 19: SRV → history material (t5 = previous material metadata)
                // Slot 20: UAV → histWrite    (u0 = EMA write)
                // Slot 21: UAV → metaWrite    (u1 = surface metadata write)
                // Slot 22: UAV → materialWrite (u2 = material metadata write)
                {
                    auto cpuBase = m_descHeap.GetCPUDescriptorHandleForHeapStart();
                    auto writeSrv = [&](ID3D12Resource* res, DXGI_FORMAT fmt, UINT slot)
                    {
                        D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
                        h.ptr += slot * m_descIncrementSize;
                        CreateTex2DSrv(dev, res, fmt, h);
                    };
                    auto writeUav = [&](ID3D12Resource* res, DXGI_FORMAT fmt, UINT slot)
                    {
                        D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
                        h.ptr += slot * m_descIncrementSize;
                        CreateTex2DUav(dev, res, fmt, h);
                    };

                    writeSrv(outTexture.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalSrvBase + 0u);
                    writeSrv(histRead.Get(),   DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalSrvBase + 1u);
                    writeSrv(desc.gbufferNormalTex, DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalSrvBase + 2u);
                    writeSrv(metaRead.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalSrvBase + 3u);
                    writeSrv(desc.gbufferMaterialTex, DXGI_FORMAT_R8G8B8A8_UNORM, kTemporalSrvBase + 4u);
                    writeSrv(materialRead.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalSrvBase + 5u);
                    writeUav(histWrite.Get(),  DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalUavBase + 0u);
                    writeUav(metaWrite.Get(),  DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalUavBase + 1u);
                    writeUav(materialWrite.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalUavBase + 2u);
                }

                // ---- Bind temporal pipeline ----
                cl->SetPipelineState(m_reflTemporalPso.Get());
                cl->SetComputeRootSignature(m_reflTemporalRootSignature.Get());
                // Descriptor heap is already bound from the reflection CS dispatch above

                // Root[0]: constants (alpha, reflection size, validation flags, GBuffer size)
                {
                    const uint32_t gbufferWidth = desc.gbufferWidth != 0u ? desc.gbufferWidth : desc.width;
                    const uint32_t gbufferHeight = desc.gbufferHeight != 0u ? desc.gbufferHeight : desc.height;
                    ReflectionTemporalReprojectionConstants reprojection{};
                    memcpy(reprojection.invVP, desc.frameDesc.inverseViewProjection, 64);
                    memcpy(reprojection.prevVP, m_prevVP, 64);
                    reprojection.cameraPos[0] = desc.frameDesc.cameraPosition[0];
                    reprojection.cameraPos[1] = desc.frameDesc.cameraPosition[1];
                    reprojection.cameraPos[2] = desc.frameDesc.cameraPosition[2];
                    reprojection.prevCameraPos[0] = m_prevReflectionCameraPos[0];
                    reprojection.prevCameraPos[1] = m_prevReflectionCameraPos[1];
                    reprojection.prevCameraPos[2] = m_prevReflectionCameraPos[2];
                    if (m_restirConstantsMapped)
                    {
                        memcpy(m_restirConstantsMapped, &reprojection, sizeof(reprojection));
                    }

                    uint32_t constants[7] = {
                        *reinterpret_cast<const uint32_t*>(&alpha),
                        desc.width,
                        desc.height,
                        desc.gbufferNormalTex ? 1u : 0u,
                        gbufferWidth,
                        gbufferHeight,
                        desc.gbufferMaterialTex ? 1u : 0u
                    };
                    cl->SetComputeRoot32BitConstants(0, 7, constants, 0);
                }

                // Root[1]: SRV table starting at slot 14
                D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
                srvGpu.ptr += kTemporalSrvBase * m_descIncrementSize;
                cl->SetComputeRootDescriptorTable(1, srvGpu);

                // Root[2]: UAV table starting at slot 20
                D3D12_GPU_DESCRIPTOR_HANDLE uavGpu2 = m_descHeap.GetGPUDescriptorHandleForHeapStart();
                uavGpu2.ptr += kTemporalUavBase * m_descIncrementSize;
                cl->SetComputeRootDescriptorTable(2, uavGpu2);

                // Root[3]: reprojection constants (b1)
                if (m_restirConstantsBuffer.IsValid())
                {
                    cl->SetComputeRootConstantBufferView(3, m_restirConstantsBuffer.GetGPUVirtualAddress());
                }

                // ---- Dispatch ----
                const UINT tgx = (desc.width  + 15u) / 16u;
                const UINT tgy = (desc.height + 15u) / 16u;
                cl->Dispatch(tgx, tgy, 1u);

                // ---- Copy EMA result (histWrite) back into outTexture ----
                // Barriers: histWrite UAV→COPY_SOURCE, outTexture SRV→COPY_DEST
                D3D12_RESOURCE_BARRIER copyPre[4] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        histWrite.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        outTexture.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        metaWrite.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        materialWrite.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                };
                cl->ResourceBarrier(_countof(copyPre), copyPre);

                cl->CopyResource(outTexture.Get(), histWrite.Get());

                // Restore states: outTexture→UAV (SWRTExecutor will transition UAV→PSR),
                //                 histWrite/metaWrite→NON_PIXEL_SHADER_RESOURCE (next frame's SRVs)
                D3D12_RESOURCE_BARRIER copyPost[2] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        outTexture.Get(),
                        D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        histWrite.Get(),
                        D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                };
                cl->ResourceBarrier(_countof(copyPost), copyPost);

                // Advance ping-pong
                m_reflHistoryPingA = !m_reflHistoryPingA;
            } // end if histories valid
        } // end temporal EMA block

        if (desc.denoiserEnabled &&
            m_reflAtrousPso &&
            m_reflAtrousScratch.IsValid() &&
            desc.gbufferNormalTex &&
            desc.updatePhaseCount == 1u &&
            desc.atrousIterations > 0u)
        {
            auto cpuBase = m_descHeap.GetCPUDescriptorHandleForHeapStart();
            auto writeSrv = [&](ID3D12Resource* res, DXGI_FORMAT fmt, UINT slot)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
                h.ptr += slot * m_descIncrementSize;
                CreateTex2DSrv(dev, res, fmt, h);
            };
            auto writeUav = [&](ID3D12Resource* res, DXGI_FORMAT fmt, UINT slot)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
                h.ptr += slot * m_descIncrementSize;
                CreateTex2DUav(dev, res, fmt, h);
            };

            ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetPipelineState(m_reflAtrousPso.Get());
            cl->SetComputeRootSignature(m_reflTemporalRootSignature.Get());

            const uint32_t iterations = std::min(desc.atrousIterations, 5u);
            for (uint32_t iter = 0; iter < iterations; ++iter)
            {
                writeSrv(outTexture.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalSrvBase + 0u);
                writeSrv(desc.gbufferNormalTex, DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalSrvBase + 1u);
                writeUav(m_reflAtrousScratch.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kTemporalUavBase + 0u);

                D3D12_RESOURCE_BARRIER pre[] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        outTexture.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        m_reflAtrousScratch.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                };
                cl->ResourceBarrier(_countof(pre), pre);

                const float stepWidth = static_cast<float>(1u << iter);
                uint32_t constants[4] = {
                    *reinterpret_cast<const uint32_t*>(&stepWidth),
                    desc.width,
                    desc.height,
                    *reinterpret_cast<const uint32_t*>(&desc.atrousPhiDepth),
                };
                cl->SetComputeRoot32BitConstants(0, 4, constants, 0);

                D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
                srvGpu.ptr += kTemporalSrvBase * m_descIncrementSize;
                cl->SetComputeRootDescriptorTable(1, srvGpu);

                D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = m_descHeap.GetGPUDescriptorHandleForHeapStart();
                uavGpu.ptr += kTemporalUavBase * m_descIncrementSize;
                cl->SetComputeRootDescriptorTable(2, uavGpu);

                const UINT tgx = (desc.width  + 15u) / 16u;
                const UINT tgy = (desc.height + 15u) / 16u;
                cl->Dispatch(tgx, tgy, 1u);

                D3D12_RESOURCE_BARRIER copyPre[] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        m_reflAtrousScratch.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        outTexture.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_DEST),
                };
                cl->ResourceBarrier(_countof(copyPre), copyPre);
                cl->CopyResource(outTexture.Get(), m_reflAtrousScratch.Get());

                D3D12_RESOURCE_BARRIER copyPost[] = {
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        m_reflAtrousScratch.Get(),
                        D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                    CD3DX12_RESOURCE_BARRIER::Transition(
                        outTexture.Get(),
                        D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                };
                cl->ResourceBarrier(_countof(copyPost), copyPost);
            }
        }

        // Legacy SWRT also uses this frame index for per-frame GGX jitter.
        // Without advancing it, rough reflections keep reusing the same sample.
        ++m_restirFrameIndex;
        Math::Invert4x4(desc.frameDesc.inverseViewProjection, m_prevVP);
        m_prevReflectionCameraPos[0] = desc.frameDesc.cameraPosition[0];
        m_prevReflectionCameraPos[1] = desc.frameDesc.cameraPosition[1];
        m_prevReflectionCameraPos[2] = desc.frameDesc.cameraPosition[2];
        return true;
    }

} // namespace SasamiRenderer
