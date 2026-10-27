#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Renderer/Scene/AnimationController.h"
#include "Renderer/Scene/SurfaceMaterial.h"
#include "Renderer/Structures/Mesh.h"

namespace SasamiEngine::Render
{
    struct CpuTextureRgba8
    {
        uint64_t id = 0;
        std::vector<uint8_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct RenderProxy
    {
        // Shared, immutable geometry: every proxy built from one StaticMeshSource points at the same
        // Mesh, so rebuilding proxies copies no vertices and SceneSubmitter can dedupe by pointer.
        std::shared_ptr<const Mesh> mesh;
        std::shared_ptr<const CpuTextureRgba8> albedoTexture;
        std::shared_ptr<const CpuTextureRgba8> occlusionTexture;
        std::shared_ptr<const CpuTextureRgba8> normalTexture;
        std::shared_ptr<const CpuTextureRgba8> emissiveTexture; // sRGB, scales material.emissive
        bool usesMetallicRoughnessTexture = false;
        SurfaceMaterial material;
        bool transparent = false;
        // The app rewrites this mesh's vertices in place (Renderer::UpdateStaticMeshDynamicVertices). Such a
        // mesh gets its own m_meshes entry (never shared by geometry hash) and no cluster DAG, and is
        // drawn by the vertex path even while the mesh-shader cluster path owns the static opaque items.
        bool dynamicVertices = false;
        std::string debugLabel;
        float model[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
    };

    // Proxy for a skinned (bone-animated) mesh. AnimationController is owned by the caller.
    struct SkinnedRenderProxy
    {
        SkinnedMesh                             mesh;
        uint64_t                                meshId = 0; // stable identity; lets the submitter skip GPU re-upload
        AnimationController*                    animController = nullptr; // must remain alive
        std::shared_ptr<const CpuTextureRgba8>  albedoTexture;
        std::shared_ptr<const CpuTextureRgba8>  occlusionTexture;
        std::shared_ptr<const CpuTextureRgba8>  normalTexture;
        std::shared_ptr<const CpuTextureRgba8>  emissiveTexture;
        SurfaceMaterial                         material;
        bool                                    transparent = false;
        float                                   model[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
    };
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
