#pragma once

#include <string>
#include <vector>

#include "Renderer/Mesh.h"
#include "Renderer/RenderProxy.h"

namespace SasamiRenderer
{
    class MeshComponent
    {
    public:
        enum class ModelFormat
        {
            Obj,
            Gltf,
            GltfStatic = Gltf, // Backward compatibility alias
        };

        bool LoadModel(const std::wstring& assetPath, ModelFormat format, float uniformScale = 1.0f);
        void AddStaticMesh(Mesh mesh, const std::wstring& texturePath = L"");
        std::vector<RenderProxy> BuildRenderProxies() const;

        void Clear();
        void SetTranslation(float x, float y, float z);

    private:
        struct StaticMeshSource
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

        std::vector<StaticMeshSource> m_staticMeshes;
        float m_model[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1,
        };
    };
}
