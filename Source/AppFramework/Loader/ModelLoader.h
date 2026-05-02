#pragma once
#include <string>
#include <vector>
#include "Renderer/Scene/SurfaceMaterial.h"
#include "Renderer/Structures/Vertex.h"
#include "Renderer/Structures/Mesh.h"

namespace SasamiRenderer
{
	// Minimal OBJ loader: supports v / vt / f (triangulated or polygons -> fan)
	// Ignores materials and normals. Generates a vertex per index (non-indexed).
	bool LoadOBJ(const std::string& path, std::vector<SasamiRenderer::Vertex>& outVertices);

    enum class StaticModelFormat
    {
        Obj,
        Gltf,
        GltfStatic = Gltf, // Backward compatibility alias
    };

    struct LoadedStaticMesh
    {
        Mesh mesh;
        std::string texturePath;
        std::string occlusionTexturePath;
        std::string metallicRoughnessTexturePath;
        SurfaceMaterial material;
        float localTransform[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
    };

    struct GltfImage {
        std::string uri;
    };

    struct GltfMaterial {
        int baseColorTexture = -1; // index into textures array
        int occlusionTexture = -1; // index into textures array
        int metallicRoughnessTexture = -1; // index into textures array
        int emissiveTexture = -1; // index into textures array
        float baseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float emissiveFactor[3] = { 0.0f, 0.0f, 0.0f };
        float metallicFactor = 1.0f;
        float roughnessFactor = 1.0f;
        float occlusionStrength = 1.0f;
        bool alphaBlend = false;
        float transmissionFactor = 0.0f;
        float ior = 1.5f;
    };

    struct GltfPrimitiveInstance {
        Mesh mesh;
        int materialIndex = -1;
        float transform[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
    };

    struct GltfTexture {
        int imageIndex = -1;
    };

    struct GltfScene {
        std::vector<GltfPrimitiveInstance> primitives;
        std::vector<GltfMaterial> materials;
        std::vector<GltfImage> images;
        std::vector<GltfTexture> textures;
    };

    // Minimal glTF 2.0 loader for static meshes with baseColor/occlusion textures.
    bool LoadGLTFStatic(const std::string& path, GltfScene& outScene);
    bool LoadStaticModel(const std::string& path,
                         StaticModelFormat format,
                         float uniformScale,
                         std::vector<LoadedStaticMesh>& outMeshes);
}

