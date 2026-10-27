#pragma once

#include "Renderer/Structures/Mesh.h"
#include "Renderer/Structures/Skeleton.h"
#include "Renderer/Structures/SkeletonAnimation.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Crowd
{
    // Two baking strategies for crowd animation, switchable at bake time:
    //  - Vertex: per frame, the skinned position + normal of every vertex (Houdini / SideFX "Vertex
    //    Animation Textures", Unreal AnimToTexture "Vertex" mode). The VS does two fetches, no skinning;
    //    memory is frames x vertices.
    //  - Bone: per frame, a 3x4 skinning matrix per bone (GPU Gems 3 ch.2 "Animated Crowd Rendering",
    //    Unreal AnimToTexture "Bone" mode). The VS keeps 4-influence skinning but reads the matrices
    //    from the baked table; memory is frames x bones.
    enum class VatMode
    {
        Vertex,
        Bone,
    };

    struct VatClip
    {
        std::string name;
        uint32_t    firstFrame  = 0; // row of frame 0 of this clip in the baked table
        uint32_t    frameCount  = 0; // >= 1
        float       durationSec = 0.0f;
    };

    struct VatBakeDesc
    {
        VatMode          mode       = VatMode::Vertex;
        float            sampleRate = 30.0f; // frames per second; must be > 0
        std::vector<int> clipIndices;        // empty = every clip, in source order
    };

    // Frames of every baked clip are stored back to back. A clip of duration d gets
    // frameCount = max(1, ceil(d * sampleRate)) frames sampled at t_f = f * d / frameCount, so the clip
    // loops seamlessly: frame frameCount would equal frame 0 and is not stored.
    // Vertices of all source meshes are concatenated in mesh order (mesh 0 first).
    struct VatBakedAnimation
    {
        VatMode              mode        = VatMode::Vertex;
        uint32_t             vertexCount = 0;
        uint32_t             boneCount   = 0;
        uint32_t             totalFrames = 0;
        std::vector<VatClip> clips;

        // Vertex mode only: float4 per (frame, vertex), index = (frame * vertexCount + vertex) * 4;
        // xyz = model-space value, w = 0. Normals are unit length.
        std::vector<float> positions;
        std::vector<float> normals;

        // Bone mode only: 12 floats (3x4 rows, see VatSkinning.h) per (frame, bone),
        // index = (frame * boneCount + bone) * 12.
        std::vector<float> boneRows;

        // Model-space AABB over every baked frame (both modes; bone mode skins on the CPU to get it).
        float boundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float boundsMax[3] = { 0.0f, 0.0f, 0.0f };

        size_t SizeInBytes() const;
    };

    // Evaluates each selected clip with AnimationController at the frame times above and fills `out`.
    // Fails (returns false, sets `error`) on: no meshes / zero vertices, boneCount 0 or > kMaxBones,
    // no clips selected, a clip index out of range, sampleRate <= 0, a bone index >= boneCount.
    bool BakeVat(const std::vector<SkinnedMesh>& meshes, const Skeleton& skeleton,
                 const std::vector<SkeletonAnimation>& animations, const VatBakeDesc& desc,
                 VatBakedAnimation& out, std::string& error);

    // Frame pair + blend for playback time `timeSec` of clip `clipIndex` (wraps; negative wraps too).
    // frameA/frameB are absolute rows in the baked table. This is the CPU spec of the 8c shader lookup.
    struct VatFrameBlend
    {
        uint32_t frameA = 0;
        uint32_t frameB = 0;
        float    alpha  = 0.0f; // weight of frameB
    };
    VatFrameBlend ComputeVatFrameBlend(const VatClip& clip, float timeSec);

    // CPU reference of one vertex at playback time (vertex mode: lerp of the two frames, normal
    // renormalized; bone mode: skins `vertex` with each frame's rows, then the same lerp).
    // `vertex` is only read in bone mode. Returns false on an out-of-range clip or vertex index.
    bool SampleVatVertex(const VatBakedAnimation& baked, uint32_t clipIndex, float timeSec,
                         uint32_t vertexIndex, const SkinnedVertex& vertex,
                         float outPosition[3], float outNormal[3]);
}

namespace SasamiEngine { using namespace Render; }
