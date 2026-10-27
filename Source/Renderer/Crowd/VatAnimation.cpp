#include "Renderer/Crowd/VatAnimation.h"

#include "Renderer/Crowd/VatSkinning.h"
#include "Renderer/Scene/AnimationController.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>

namespace SasamiEngine::Render::Crowd
{
    namespace
    {
        void NormalizeOrZero(float n[3])
        {
            const float lenSq = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
            if (lenSq > 1e-20f)
            {
                const float invLen = 1.0f / std::sqrt(lenSq);
                n[0] *= invLen;
                n[1] *= invLen;
                n[2] *= invLen;
            }
            else
            {
                n[0] = n[1] = n[2] = 0.0f;
            }
        }
    }

    size_t VatBakedAnimation::SizeInBytes() const
    {
        return (positions.size() + normals.size() + boneRows.size()) * sizeof(float);
    }

    bool BakeVat(const std::vector<SkinnedMesh>& meshes, const Skeleton& skeleton,
                 const std::vector<SkeletonAnimation>& animations, const VatBakeDesc& desc,
                 VatBakedAnimation& out, std::string& error)
    {
        out = VatBakedAnimation();
        error.clear();

        uint32_t vertexCount = 0;
        for (const SkinnedMesh& mesh : meshes)
        {
            vertexCount += mesh.VertexCount();
        }
        if (vertexCount == 0)
        {
            error = "no vertices";
            return false;
        }
        if (skeleton.boneCount == 0)
        {
            error = "boneCount 0";
            return false;
        }
        if (skeleton.boneCount > Skeleton::kMaxBones)
        {
            error = "boneCount > " + std::to_string(Skeleton::kMaxBones);
            return false;
        }
        if (!(desc.sampleRate > 0.0f))
        {
            error = "sampleRate <= 0";
            return false;
        }

        std::vector<int> clipIndices = desc.clipIndices;
        if (clipIndices.empty())
        {
            for (size_t i = 0; i < animations.size(); ++i)
            {
                clipIndices.push_back(static_cast<int>(i));
            }
        }
        if (clipIndices.empty())
        {
            error = "no clips";
            return false;
        }
        for (int index : clipIndices)
        {
            if (index < 0 || static_cast<size_t>(index) >= animations.size())
            {
                error = "clip index " + std::to_string(index) + " out of range";
                return false;
            }
        }

        for (size_t m = 0; m < meshes.size(); ++m)
        {
            const std::vector<SkinnedVertex>& vertices = meshes[m].vertices;
            for (size_t v = 0; v < vertices.size(); ++v)
            {
                for (int i = 0; i < 4; ++i)
                {
                    if (vertices[v].boneIndices[i] >= skeleton.boneCount)
                    {
                        error = "bone index " + std::to_string(vertices[v].boneIndices[i]) + " >= boneCount "
                              + std::to_string(skeleton.boneCount) + " (mesh " + std::to_string(m)
                              + " vertex " + std::to_string(v) + ")";
                        return false;
                    }
                }
            }
        }

        VatBakedAnimation result;
        uint32_t totalFrames = 0;
        for (int index : clipIndices)
        {
            const SkeletonAnimation& anim = animations[static_cast<size_t>(index)];
            VatClip clip;
            clip.name        = anim.name;
            clip.durationSec = anim.durationSec;
            clip.frameCount  = 1;
            if (anim.durationSec > 0.0f)
            {
                const double frames = std::ceil(static_cast<double>(anim.durationSec) * static_cast<double>(desc.sampleRate));
                clip.frameCount = std::max<uint32_t>(1, static_cast<uint32_t>(frames));
            }
            clip.firstFrame = totalFrames;
            totalFrames += clip.frameCount;
            result.clips.push_back(clip);
        }

        const bool vertexMode = desc.mode == VatMode::Vertex;
        const uint32_t boneCount = skeleton.boneCount;
        if (vertexMode)
        {
            result.positions.resize(static_cast<size_t>(totalFrames) * vertexCount * 4);
            result.normals.resize(static_cast<size_t>(totalFrames) * vertexCount * 4);
        }
        else
        {
            result.boneRows.resize(static_cast<size_t>(totalFrames) * boneCount * 12);
        }

        const auto sharedSkeleton = std::make_shared<Skeleton>(skeleton);

        const float inf = std::numeric_limits<float>::infinity();
        float boundsMin[3] = { inf, inf, inf };
        float boundsMax[3] = { -inf, -inf, -inf };

        std::vector<float> boneMatrices(static_cast<size_t>(Skeleton::kMaxBones) * 16);

        for (size_t c = 0; c < clipIndices.size(); ++c)
        {
            const VatClip& clip = result.clips[c];
            // A fresh controller per clip: AnimationController keeps the last local pose for channels
            // the playing clip does not drive, so a shared one would leak the previous clip's pose
            // into this one and make the bake depend on clip order.
            AnimationController controller;
            controller.SetSkeleton(sharedSkeleton);
            controller.AddAnimation(animations[static_cast<size_t>(clipIndices[c])]);
            for (uint32_t f = 0; f < clip.frameCount; ++f)
            {
                const float t = (clip.frameCount > 0 && clip.durationSec > 0.0f)
                    ? static_cast<float>(f) * clip.durationSec / static_cast<float>(clip.frameCount)
                    : 0.0f;
                controller.PlayAnimation(0, true);
                controller.Update(t);
                controller.GetBoneMatrices(boneMatrices.data());

                const uint32_t frame = clip.firstFrame + f;
                if (!vertexMode)
                {
                    for (uint32_t b = 0; b < boneCount; ++b)
                    {
                        ColumnMajorToBoneRows(&boneMatrices[static_cast<size_t>(b) * 16],
                                              &result.boneRows[(static_cast<size_t>(frame) * boneCount + b) * 12]);
                    }
                }

                uint32_t vertexIndex = 0;
                for (const SkinnedMesh& mesh : meshes)
                {
                    for (const SkinnedVertex& vertex : mesh.vertices)
                    {
                        float pos[3];
                        float nrm[3];
                        SkinVertexCpu(vertex, boneMatrices.data(), pos, nrm);
                        for (int k = 0; k < 3; ++k)
                        {
                            boundsMin[k] = std::min(boundsMin[k], pos[k]);
                            boundsMax[k] = std::max(boundsMax[k], pos[k]);
                        }
                        if (vertexMode)
                        {
                            const size_t base = (static_cast<size_t>(frame) * vertexCount + vertexIndex) * 4;
                            for (int k = 0; k < 3; ++k)
                            {
                                result.positions[base + k] = pos[k];
                                result.normals[base + k]   = nrm[k];
                            }
                            result.positions[base + 3] = 0.0f;
                            result.normals[base + 3]   = 0.0f;
                        }
                        ++vertexIndex;
                    }
                }
            }
        }

        result.mode        = desc.mode;
        result.vertexCount = vertexCount;
        result.boneCount   = boneCount;
        result.totalFrames = totalFrames;
        for (int k = 0; k < 3; ++k)
        {
            result.boundsMin[k] = boundsMin[k];
            result.boundsMax[k] = boundsMax[k];
        }

        out = std::move(result);
        return true;
    }

    VatFrameBlend ComputeVatFrameBlend(const VatClip& clip, float timeSec)
    {
        VatFrameBlend blend;
        blend.frameA = clip.firstFrame;
        blend.frameB = clip.firstFrame;
        blend.alpha  = 0.0f;
        if (clip.frameCount <= 1 || clip.durationSec <= 0.0f)
        {
            return blend;
        }

        const float d = clip.durationSec;
        float t = std::fmod(timeSec, d);
        if (t < 0.0f)
        {
            t += d;
        }

        const float framePos = t / d * static_cast<float>(clip.frameCount);
        uint32_t i = static_cast<uint32_t>(std::floor(framePos));
        i = std::min(i, clip.frameCount - 1);
        const float alpha = std::clamp(framePos - static_cast<float>(i), 0.0f, 1.0f);

        blend.frameA = clip.firstFrame + i;
        blend.frameB = clip.firstFrame + (i + 1) % clip.frameCount;
        blend.alpha  = alpha;
        return blend;
    }

    bool SampleVatVertex(const VatBakedAnimation& baked, uint32_t clipIndex, float timeSec,
                         uint32_t vertexIndex, const SkinnedVertex& vertex,
                         float outPosition[3], float outNormal[3])
    {
        if (clipIndex >= baked.clips.size() || vertexIndex >= baked.vertexCount)
        {
            return false;
        }

        const VatFrameBlend blend = ComputeVatFrameBlend(baked.clips[clipIndex], timeSec);
        float posA[3], nrmA[3], posB[3], nrmB[3];

        if (baked.mode == VatMode::Vertex)
        {
            const size_t baseA = (static_cast<size_t>(blend.frameA) * baked.vertexCount + vertexIndex) * 4;
            const size_t baseB = (static_cast<size_t>(blend.frameB) * baked.vertexCount + vertexIndex) * 4;
            for (int k = 0; k < 3; ++k)
            {
                posA[k] = baked.positions[baseA + k];
                nrmA[k] = baked.normals[baseA + k];
                posB[k] = baked.positions[baseB + k];
                nrmB[k] = baked.normals[baseB + k];
            }
        }
        else
        {
            const float* rowsA = &baked.boneRows[static_cast<size_t>(blend.frameA) * baked.boneCount * 12];
            const float* rowsB = &baked.boneRows[static_cast<size_t>(blend.frameB) * baked.boneCount * 12];
            SkinVertexWithBoneRows(vertex, rowsA, posA, nrmA);
            SkinVertexWithBoneRows(vertex, rowsB, posB, nrmB);
        }

        float nrm[3];
        for (int k = 0; k < 3; ++k)
        {
            outPosition[k] = posA[k] + (posB[k] - posA[k]) * blend.alpha;
            nrm[k]         = nrmA[k] + (nrmB[k] - nrmA[k]) * blend.alpha;
        }
        NormalizeOrZero(nrm);
        for (int k = 0; k < 3; ++k)
        {
            outNormal[k] = nrm[k];
        }
        return true;
    }
}
