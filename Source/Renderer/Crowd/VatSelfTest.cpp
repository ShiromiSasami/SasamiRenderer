#include "Renderer/Crowd/VatSelfTest.h"

#include "Renderer/Crowd/VatAnimation.h"
#include "Renderer/Crowd/VatSkinning.h"
#include "Renderer/Scene/AnimationController.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace SasamiEngine::Render::Crowd
{
    namespace
    {
        struct Rig
        {
            Skeleton                       skeleton{};
            std::vector<SkeletonAnimation> animations;
            std::vector<SkinnedMesh>       meshes;
        };

        AnimKeyframe MakeKey(float time, float x, float y, float z, float w)
        {
            AnimKeyframe key{};
            key.time = time;
            key.value[0] = x;
            key.value[1] = y;
            key.value[2] = z;
            key.value[3] = w;
            return key;
        }

        SkinnedVertex MakeVertex(float x, float y, float z, const float normal[3],
                                 uint8_t boneA, float weightA, uint8_t boneB, float weightB)
        {
            SkinnedVertex v{};
            v.position[0] = x;
            v.position[1] = y;
            v.position[2] = z;
            v.normal[0] = normal[0];
            v.normal[1] = normal[1];
            v.normal[2] = normal[2];
            v.boneIndices[0] = boneA;
            v.boneIndices[1] = boneB;
            v.boneWeights[0] = weightA;
            v.boneWeights[1] = weightB;
            return v;
        }

        Rig BuildRig()
        {
            Rig rig;
            Skeleton& s = rig.skeleton;
            s.boneCount = 2;
            s.parentIndex[0] = -1;
            s.parentIndex[1] = 0;
            std::snprintf(s.boneName[0], sizeof(s.boneName[0]), "Root");
            std::snprintf(s.boneName[1], sizeof(s.boneName[1]), "Child");
            for (uint32_t b = 0; b < 2; ++b)
            {
                s.bindLocalR[b][3] = 1.0f;
                s.bindLocalS[b][0] = s.bindLocalS[b][1] = s.bindLocalS[b][2] = 1.0f;
                s.inverseBindPose[b][0] = s.inverseBindPose[b][5] = s.inverseBindPose[b][10] = s.inverseBindPose[b][15] = 1.0f;
            }
            s.bindLocalT[1][1] = 1.0f;
            s.inverseBindPose[1][13] = -1.0f;

            SkeletonAnimation bend;
            bend.name = "Bend";
            bend.durationSec = 1.0f;
            bend.boneTracks.resize(2);
            bend.boneTracks[1].rotation = {
                MakeKey(0.0f, 0.0f, 0.0f, 0.0f, 1.0f),
                MakeKey(0.5f, 0.0f, 0.0f, 0.3826834f, 0.9238795f),
                MakeKey(1.0f, 0.0f, 0.0f, 0.7071068f, 0.7071068f),
            };

            SkeletonAnimation lift;
            lift.name = "Lift";
            lift.durationSec = 0.25f;
            lift.boneTracks.resize(2);
            lift.boneTracks[0].translation = {
                MakeKey(0.0f, 0.0f, 0.0f, 0.0f, 0.0f),
                MakeKey(0.25f, 0.0f, 0.5f, 0.0f, 0.0f),
            };

            rig.animations = { bend, lift };

            const float nz[3] = { 0.0f, 0.0f, 1.0f };
            const float nx[3] = { 1.0f, 0.0f, 0.0f };
            SkinnedMesh mesh;
            mesh.vertices = {
                MakeVertex(0.0f, 0.0f, 0.0f, nz, 0, 1.0f, 0, 0.0f),
                MakeVertex(0.0f, 1.0f, 0.0f, nx, 1, 1.0f, 0, 0.0f),
                MakeVertex(1.0f, 1.0f, 0.0f, nz, 1, 1.0f, 0, 0.0f),
                MakeVertex(0.5f, 0.5f, 0.0f, nx, 0, 0.5f, 1, 0.5f),
            };
            mesh.indices = { 0, 1, 2 };
            rig.meshes.push_back(std::move(mesh));
            return rig;
        }

        // Reference pose of one (clip, time) through a fresh controller, as the baker does it.
        void ReferenceBoneMatrices(const Rig& rig, int clip, float t, std::vector<float>& outMatrices)
        {
            AnimationController controller;
            controller.SetSkeleton(std::make_shared<Skeleton>(rig.skeleton));
            for (const SkeletonAnimation& anim : rig.animations)
            {
                controller.AddAnimation(anim);
            }
            controller.PlayAnimation(clip, true);
            controller.Update(t);
            outMatrices.assign(static_cast<size_t>(Skeleton::kMaxBones) * 16, 0.0f);
            controller.GetBoneMatrices(outMatrices.data());
        }

        float FrameTime(const VatClip& clip, uint32_t frame)
        {
            return clip.frameCount > 1 && clip.durationSec > 0.0f
                ? static_cast<float>(frame) * clip.durationSec / static_cast<float>(clip.frameCount)
                : 0.0f;
        }

        bool Near(float a, float b, float tolerance)
        {
            return std::fabs(a - b) <= tolerance;
        }
    }

    std::string RunVatSelfTest()
    {
        uint32_t checks = 0;
        std::string firstError;
        auto check = [&](bool condition, const char* description, int a = -1, int b = -1, int c = -1)
        {
            ++checks;
            if (!condition && firstError.empty())
            {
                firstError = description;
                if (a >= 0) { firstError += " clip=" + std::to_string(a); }
                if (b >= 0) { firstError += " frame=" + std::to_string(b); }
                if (c >= 0) { firstError += " vertex=" + std::to_string(c); }
            }
        };

        Rig rig = BuildRig();
        constexpr uint32_t VERTS = 4;

        VatBakeDesc vertexDesc;
        vertexDesc.mode = VatMode::Vertex;
        vertexDesc.sampleRate = 30.0f;
        VatBakeDesc boneDesc = vertexDesc;
        boneDesc.mode = VatMode::Bone;

        VatBakedAnimation vertexBake;
        VatBakedAnimation boneBake;
        std::string error;

        const bool vertexBaked = BakeVat(rig.meshes, rig.skeleton, rig.animations, vertexDesc, vertexBake, error);
        check(vertexBaked, "vertex bake failed");
        if (!vertexBaked)
        {
            return "ERR " + firstError + ": " + error;
        }
        const bool boneBaked = BakeVat(rig.meshes, rig.skeleton, rig.animations, boneDesc, boneBake, error);
        check(boneBaked, "bone bake failed");
        if (!boneBaked)
        {
            return "ERR " + firstError + ": " + error;
        }

        // (a) Frame layout.
        check(vertexBake.clips.size() == 2, "layout: clip count");
        if (vertexBake.clips.size() != 2)
        {
            return "ERR " + firstError;
        }
        check(vertexBake.clips[0].frameCount == 30 && vertexBake.clips[0].firstFrame == 0, "layout: clip 0 frames");
        check(vertexBake.clips[1].frameCount == 8 && vertexBake.clips[1].firstFrame == 30, "layout: clip 1 frames");
        check(vertexBake.totalFrames == 38, "layout: total frames");
        check(vertexBake.vertexCount == VERTS && vertexBake.boneCount == 2, "layout: vertex / bone count");
        check(vertexBake.positions.size() == 38u * VERTS * 4u && vertexBake.normals.size() == 38u * VERTS * 4u, "layout: vertex table size");
        check(vertexBake.boneRows.empty(), "layout: vertex mode has no bone rows");
        if (!firstError.empty())
        {
            return "ERR " + firstError;
        }

        // (b) Vertex mode equals direct skinning, bit for bit.
        std::vector<float> matrices;
        for (uint32_t ci = 0; ci < 2; ++ci)
        {
            const VatClip& clip = vertexBake.clips[ci];
            for (uint32_t f = 0; f < clip.frameCount; ++f)
            {
                ReferenceBoneMatrices(rig, static_cast<int>(ci), FrameTime(clip, f), matrices);
                const size_t row = static_cast<size_t>(clip.firstFrame + f) * VERTS;
                bool positionsEqual = true;
                bool normalsEqual = true;
                for (uint32_t v = 0; v < VERTS; ++v)
                {
                    float p[3], n[3];
                    SkinVertexCpu(rig.meshes[0].vertices[v], matrices.data(), p, n);
                    for (int k = 0; k < 3; ++k)
                    {
                        positionsEqual = positionsEqual && p[k] == vertexBake.positions[(row + v) * 4 + k];
                        normalsEqual = normalsEqual && n[k] == vertexBake.normals[(row + v) * 4 + k];
                    }
                }
                check(positionsEqual, "vertex bake position differs from direct skinning", static_cast<int>(ci), static_cast<int>(f));
                check(normalsEqual, "vertex bake normal differs from direct skinning", static_cast<int>(ci), static_cast<int>(f));
            }
        }

        // (c) Bone mode reconstructs vertex mode.
        check(boneBake.boneRows.size() == 38u * 2u * 12u, "bone: row table size");
        check(boneBake.positions.empty(), "bone: no vertex positions stored");
        if (boneBake.boneRows.size() == 38u * 2u * 12u)
        {
            for (uint32_t frame = 0; frame < 38; ++frame)
            {
                const float* rows = boneBake.boneRows.data() + static_cast<size_t>(frame) * 2 * 12;
                bool match = true;
                for (uint32_t v = 0; v < VERTS; ++v)
                {
                    float p[3], n[3];
                    SkinVertexWithBoneRows(rig.meshes[0].vertices[v], rows, p, n);
                    for (int k = 0; k < 3; ++k)
                    {
                        match = match && Near(p[k], vertexBake.positions[(static_cast<size_t>(frame) * VERTS + v) * 4 + k], 1e-5f);
                        match = match && Near(n[k], vertexBake.normals[(static_cast<size_t>(frame) * VERTS + v) * 4 + k], 1e-5f);
                    }
                }
                check(match, "bone rows skinning differs from vertex bake", -1, static_cast<int>(frame));
            }
        }

        // (d) SampleVatVertex at frame times and in bone mode.
        for (uint32_t ci = 0; ci < 2; ++ci)
        {
            const VatClip& clip = vertexBake.clips[ci];
            for (uint32_t f = 0; f < clip.frameCount; ++f)
            {
                const float t = FrameTime(clip, f);
                bool match = true;
                for (uint32_t v = 0; v < VERTS; ++v)
                {
                    float p[3], n[3];
                    const bool ok = SampleVatVertex(vertexBake, ci, t, v, rig.meshes[0].vertices[v], p, n);
                    match = match && ok;
                    for (int k = 0; k < 3 && ok; ++k)
                    {
                        match = match && Near(p[k], vertexBake.positions[(static_cast<size_t>(clip.firstFrame + f) * VERTS + v) * 4 + k], 1e-6f);
                    }
                }
                check(match, "SampleVatVertex at a frame time differs from the baked frame", static_cast<int>(ci), static_cast<int>(f));
            }
            bool modesMatch = true;
            for (uint32_t v = 0; v < VERTS; ++v)
            {
                float pv[3], nv[3], pb[3], nb[3];
                const bool okV = SampleVatVertex(vertexBake, ci, 0.37f, v, rig.meshes[0].vertices[v], pv, nv);
                const bool okB = SampleVatVertex(boneBake, ci, 0.37f, v, rig.meshes[0].vertices[v], pb, nb);
                modesMatch = modesMatch && okV && okB;
                for (int k = 0; k < 3 && okV && okB; ++k)
                {
                    modesMatch = modesMatch && Near(pv[k], pb[k], 1e-5f);
                }
            }
            check(modesMatch, "bone mode sample at t=0.37 differs from vertex mode", static_cast<int>(ci));
        }
        {
            float p[3], n[3];
            check(!SampleVatVertex(vertexBake, 2, 0.0f, 0, rig.meshes[0].vertices[0], p, n), "SampleVatVertex accepts an out-of-range clip");
            check(!SampleVatVertex(vertexBake, 0, 0.0f, VERTS, rig.meshes[0].vertices[0], p, n), "SampleVatVertex accepts an out-of-range vertex");
        }

        // (e) Frame blend and wrap.
        {
            const VatClip& c0 = vertexBake.clips[0];
            const VatClip& c1 = vertexBake.clips[1];
            VatFrameBlend b = ComputeVatFrameBlend(c0, 1.0f);
            check(b.frameA == 0 && Near(b.alpha, 0.0f, 1e-4f), "blend: t == duration wraps to frame 0");
            b = ComputeVatFrameBlend(c0, -0.25f);
            check(b.frameA == 22 && b.frameB == 23 && Near(b.alpha, 0.5f, 1e-4f), "blend: negative time wraps");
            b = ComputeVatFrameBlend(c0, 5.5f / 30.0f);
            check(b.frameA == 5 && b.frameB == 6 && Near(b.alpha, 0.5f, 1e-4f), "blend: mid frame 5");
            b = ComputeVatFrameBlend(c1, 0.2499f);
            check(b.frameA == c1.firstFrame + 7 && b.frameB == c1.firstFrame, "blend: last frame wraps to the clip's first frame");
        }

        // (f) Spot geometry.
        {
            const VatClip& c0 = vertexBake.clips[0];
            const VatClip& c1 = vertexBake.clips[1];
            const float* p = &vertexBake.positions[(static_cast<size_t>(c0.firstFrame + 15) * VERTS + 2) * 4];
            check(Near(p[0], 0.7071068f, 1e-4f) && Near(p[1], 1.7071068f, 1e-4f) && Near(p[2], 0.0f, 1e-4f), "geometry: vertex 2 at 45 degrees");
            const float* q = &vertexBake.positions[(static_cast<size_t>(c1.firstFrame + 4) * VERTS + 0) * 4];
            check(Near(q[1], 0.25f, 1e-5f), "geometry: lifted root vertex");
        }

        // (g) Bounds.
        {
            bool contained = true;
            for (size_t i = 0; i < vertexBake.positions.size(); i += 4)
            {
                for (int k = 0; k < 3; ++k)
                {
                    contained = contained && vertexBake.positions[i + k] >= vertexBake.boundsMin[k] - 1e-6f
                                          && vertexBake.positions[i + k] <= vertexBake.boundsMax[k] + 1e-6f;
                }
            }
            check(contained, "bounds: contain every baked position");
            bool same = true;
            for (int k = 0; k < 3; ++k)
            {
                same = same && Near(vertexBake.boundsMin[k], boneBake.boundsMin[k], 1e-6f)
                            && Near(vertexBake.boundsMax[k], boneBake.boundsMax[k], 1e-6f);
            }
            check(same, "bounds: bone mode equals vertex mode");
        }

        // (h) Failures and clip selection.
        {
            VatBakedAnimation out;
            std::string err;

            err.clear();
            check(!BakeVat({}, rig.skeleton, rig.animations, vertexDesc, out, err) && !err.empty(), "fail: empty meshes");

            VatBakeDesc badRate = vertexDesc;
            badRate.sampleRate = 0.0f;
            err.clear();
            check(!BakeVat(rig.meshes, rig.skeleton, rig.animations, badRate, out, err) && !err.empty(), "fail: zero sample rate");

            VatBakeDesc badClip = vertexDesc;
            badClip.clipIndices = { 5 };
            err.clear();
            check(!BakeVat(rig.meshes, rig.skeleton, rig.animations, badClip, out, err) && !err.empty(), "fail: clip index out of range");

            std::vector<SkinnedMesh> badMeshes = rig.meshes;
            badMeshes[0].vertices[1].boneIndices[0] = 7;
            err.clear();
            check(!BakeVat(badMeshes, rig.skeleton, rig.animations, vertexDesc, out, err) && !err.empty(), "fail: bone index >= boneCount");

            Skeleton noBones = rig.skeleton;
            noBones.boneCount = 0;
            err.clear();
            check(!BakeVat(rig.meshes, noBones, rig.animations, vertexDesc, out, err) && !err.empty(), "fail: zero bones");

            VatBakeDesc single = vertexDesc;
            single.clipIndices = { 1 };
            err.clear();
            const bool ok = BakeVat(rig.meshes, rig.skeleton, rig.animations, single, out, err);
            check(ok && out.clips.size() == 1 && out.clips[0].firstFrame == 0 && out.clips[0].frameCount == 8, "select: single clip {1}");
        }

        // (i) Normals are unit length.
        {
            bool unit = true;
            for (size_t i = 0; i < vertexBake.normals.size(); i += 4)
            {
                const float x = vertexBake.normals[i];
                const float y = vertexBake.normals[i + 1];
                const float z = vertexBake.normals[i + 2];
                unit = unit && Near(std::sqrt(x * x + y * y + z * z), 1.0f, 1e-5f);
            }
            check(unit, "normals: unit length");
        }

        if (!firstError.empty())
        {
            return "ERR " + firstError;
        }
        return "OK checks=" + std::to_string(checks);
    }
}
