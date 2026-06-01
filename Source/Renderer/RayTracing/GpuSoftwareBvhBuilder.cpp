// GpuSoftwareBvhBuilder.cpp
// CPU-side SAH BVH construction for GpuSoftwareRayTracer.
// No D3D12 runtime usage — pure geometry/math code.
#define NOMINMAX
#include "Renderer/RayTracing/GpuSoftwareRayTracer.h"

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstring>
#include <vector>

namespace SasamiRenderer
{
    namespace
    {
        // ---- Lightweight math types ----
        struct F2 { float x = 0.f, y = 0.f; };
        struct F3 { float x = 0.f, y = 0.f, z = 0.f; };

        F3 operator+(const F3& a, const F3& b) { return { a.x+b.x, a.y+b.y, a.z+b.z }; }
        F3 operator-(const F3& a, const F3& b) { return { a.x-b.x, a.y-b.y, a.z-b.z }; }
        F3 operator*(const F3& a, float s)      { return { a.x*s,   a.y*s,   a.z*s   }; }
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

        // ---- Recursive TLAS build ----
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

    } // anonymous namespace

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

    // =========================================================================
    // BuildMeshBvhSah
    // =========================================================================

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

    // =========================================================================
    // RebuildTlas
    // =========================================================================

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

} // namespace SasamiRenderer
