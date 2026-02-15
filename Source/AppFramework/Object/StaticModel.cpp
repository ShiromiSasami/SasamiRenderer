#include "Object/StaticModel.h"
#include <utility>

namespace SasamiRenderer
{
    bool StaticModel::LoadModel(const std::string& assetPath, ModelFormat format, float uniformScale)
    {
        return MeshComponentRef().LoadModel(assetPath, format, uniformScale);
    }

    std::vector<RenderProxy> StaticModel::BuildRenderProxies() const
    {
        return MeshComponentRef().BuildRenderProxies();
    }

    void StaticModel::AddStaticMesh(Mesh mesh, const std::string& texturePath)
    {
        MeshComponentRef().AddStaticMesh(std::move(mesh), texturePath);
    }

    void StaticModel::ClearModel()
    {
        MeshComponentRef().Clear();
    }

    void StaticModel::SetTranslation(float x, float y, float z)
    {
        MeshComponentRef().SetTranslation(x, y, z);
    }
}
