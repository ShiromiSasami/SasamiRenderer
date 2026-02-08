#pragma once
#include <string>
#include <vector>
#include "Renderer/Vertex.h"
#include "Renderer/Mesh.h"

namespace SasamiRenderer
{
	// Minimal OBJ loader: supports v / vt / f (triangulated or polygons -> fan)
	// Ignores materials and normals. Generates a vertex per index (non-indexed).
	bool LoadOBJ(const std::wstring& path, std::vector<SasamiRenderer::Vertex>& outVertices);

    enum class StaticModelFormat
    {
        Obj,
        Gltf,
        GltfStatic = Gltf, // Backward compatibility alias
    };

    struct LoadedStaticMesh
    {
        Mesh mesh;
        std::wstring texturePath;
        float localTransform[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
    };

    struct GltfImage {
        std::wstring uri;
    };

    struct GltfMaterial {
        int baseColorTexture = -1; // index into textures array
        float baseColorFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
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

    // Minimal glTF 2.0 loader for static meshes with baseColor textures.
    bool LoadGLTFStatic(const std::wstring& path, GltfScene& outScene);
    bool LoadStaticModel(const std::wstring& path,
                         StaticModelFormat format,
                         float uniformScale,
                         std::vector<LoadedStaticMesh>& outMeshes);
}

