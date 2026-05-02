#include "Object/StaticModel.h"
#include <cmath>
#include <utility>

namespace SasamiRenderer
{
    namespace
    {
        constexpr float kPi = 3.1415926535f;

        Vertex MakeVertex(float px, float py, float pz,
                          float nx, float ny, float nz,
                          float u, float v)
        {
            Vertex vertex{};
            vertex.position[0] = px;
            vertex.position[1] = py;
            vertex.position[2] = pz;
            vertex.normal[0] = nx;
            vertex.normal[1] = ny;
            vertex.normal[2] = nz;
            vertex.color[0] = 1.0f;
            vertex.color[1] = 1.0f;
            vertex.color[2] = 1.0f;
            vertex.color[3] = 1.0f;
            vertex.uv[0] = u;
            vertex.uv[1] = v;
            return vertex;
        }

        Mesh CreateBoxMesh(const StaticModel::BoxDesc& desc)
        {
            const float width = (std::fabs(desc.width) > 0.0f) ? std::fabs(desc.width) : 1.0f;
            const float height = (std::fabs(desc.height) > 0.0f) ? std::fabs(desc.height) : 1.0f;
            const float depth = (std::fabs(desc.depth) > 0.0f) ? std::fabs(desc.depth) : 1.0f;
            const float hx = width * 0.5f;
            const float hy = height * 0.5f;
            const float hz = depth * 0.5f;

            Mesh mesh;
            mesh.vertices = {
                MakeVertex(-hx,  hy,  hz,  0.0f,  0.0f,  1.0f, 0.0f, 0.0f),
                MakeVertex(-hx, -hy,  hz,  0.0f,  0.0f,  1.0f, 0.0f, 1.0f),
                MakeVertex( hx, -hy,  hz,  0.0f,  0.0f,  1.0f, 1.0f, 1.0f),
                MakeVertex( hx,  hy,  hz,  0.0f,  0.0f,  1.0f, 1.0f, 0.0f),

                MakeVertex( hx,  hy, -hz,  0.0f,  0.0f, -1.0f, 0.0f, 0.0f),
                MakeVertex( hx, -hy, -hz,  0.0f,  0.0f, -1.0f, 0.0f, 1.0f),
                MakeVertex(-hx, -hy, -hz,  0.0f,  0.0f, -1.0f, 1.0f, 1.0f),
                MakeVertex(-hx,  hy, -hz,  0.0f,  0.0f, -1.0f, 1.0f, 0.0f),

                MakeVertex( hx,  hy,  hz,  1.0f,  0.0f,  0.0f, 0.0f, 0.0f),
                MakeVertex( hx, -hy,  hz,  1.0f,  0.0f,  0.0f, 0.0f, 1.0f),
                MakeVertex( hx, -hy, -hz,  1.0f,  0.0f,  0.0f, 1.0f, 1.0f),
                MakeVertex( hx,  hy, -hz,  1.0f,  0.0f,  0.0f, 1.0f, 0.0f),

                MakeVertex(-hx,  hy, -hz, -1.0f,  0.0f,  0.0f, 0.0f, 0.0f),
                MakeVertex(-hx, -hy, -hz, -1.0f,  0.0f,  0.0f, 0.0f, 1.0f),
                MakeVertex(-hx, -hy,  hz, -1.0f,  0.0f,  0.0f, 1.0f, 1.0f),
                MakeVertex(-hx,  hy,  hz, -1.0f,  0.0f,  0.0f, 1.0f, 0.0f),

                MakeVertex(-hx,  hy, -hz,  0.0f,  1.0f,  0.0f, 0.0f, 0.0f),
                MakeVertex(-hx,  hy,  hz,  0.0f,  1.0f,  0.0f, 0.0f, 1.0f),
                MakeVertex( hx,  hy,  hz,  0.0f,  1.0f,  0.0f, 1.0f, 1.0f),
                MakeVertex( hx,  hy, -hz,  0.0f,  1.0f,  0.0f, 1.0f, 0.0f),

                MakeVertex(-hx, -hy,  hz,  0.0f, -1.0f,  0.0f, 0.0f, 0.0f),
                MakeVertex(-hx, -hy, -hz,  0.0f, -1.0f,  0.0f, 0.0f, 1.0f),
                MakeVertex( hx, -hy, -hz,  0.0f, -1.0f,  0.0f, 1.0f, 1.0f),
                MakeVertex( hx, -hy,  hz,  0.0f, -1.0f,  0.0f, 1.0f, 0.0f),
            };
            mesh.indices = {
                 0,  1,  2,  0,  2,  3,
                 4,  5,  6,  4,  6,  7,
                 8,  9, 10,  8, 10, 11,
                12, 13, 14, 12, 14, 15,
                16, 17, 18, 16, 18, 19,
                20, 21, 22, 20, 22, 23,
            };
            return mesh;
        }

        Mesh CreateSphereMesh(const StaticModel::SphereDesc& desc)
        {
            const float radius = (std::fabs(desc.radius) > 0.0f) ? std::fabs(desc.radius) : 0.5f;
            const uint32_t slices = (desc.slices >= 3u) ? desc.slices : 3u;
            const uint32_t stacks = (desc.stacks >= 2u) ? desc.stacks : 2u;

            Mesh mesh;
            mesh.vertices.clear();
            mesh.indices.clear();
            const uint32_t ringVertexCount = slices + 1u;
            mesh.vertices.reserve(static_cast<size_t>(ringVertexCount) * static_cast<size_t>(stacks + 1u));
            mesh.indices.reserve(static_cast<size_t>(slices) * static_cast<size_t>(stacks) * 6u);

            for (uint32_t stack = 0; stack <= stacks; ++stack) {
                const float v = static_cast<float>(stack) / static_cast<float>(stacks);
                const float phi = v * kPi;
                const float y = std::cos(phi);
                const float ringRadius = std::sin(phi);

                for (uint32_t slice = 0; slice <= slices; ++slice) {
                    const float u = static_cast<float>(slice) / static_cast<float>(slices);
                    const float theta = u * (kPi * 2.0f);
                    const float x = std::sin(theta) * ringRadius;
                    const float z = std::cos(theta) * ringRadius;
                    mesh.vertices.push_back(MakeVertex(
                        x * radius, y * radius, z * radius,
                        x, y, z,
                        u, v));
                }
            }

            for (uint32_t stack = 0; stack < stacks; ++stack) {
                for (uint32_t slice = 0; slice < slices; ++slice) {
                    const uint32_t current = stack * ringVertexCount + slice;
                    const uint32_t next = current + ringVertexCount;
                    mesh.indices.push_back(current);
                    mesh.indices.push_back(next + 1u);
                    mesh.indices.push_back(next);
                    mesh.indices.push_back(current);
                    mesh.indices.push_back(current + 1u);
                    mesh.indices.push_back(next + 1u);
                }
            }

            return mesh;
        }
    }

    bool StaticModel::LoadModel(const std::string& assetPath, ModelFormat format, float uniformScale)
    {
        return MeshComponentRef().LoadModel(assetPath, format, uniformScale);
    }

    std::vector<RenderProxy> StaticModel::BuildRenderProxies() const
    {
        return MeshComponentRef().BuildRenderProxies();
    }

    void StaticModel::AddStaticMesh(Mesh mesh,
                                    const std::string& albedoTexturePath,
                                    const std::string& occlusionTexturePath)
    {
        MeshComponentRef().AddStaticMesh(std::move(mesh), albedoTexturePath, occlusionTexturePath);
    }

    void StaticModel::AddStaticMesh(Mesh mesh,
                                    const SurfaceMaterial& material,
                                    const std::string& albedoTexturePath,
                                    const std::string& occlusionTexturePath)
    {
        MeshComponentRef().AddStaticMesh(std::move(mesh), material, albedoTexturePath, occlusionTexturePath);
    }

    void StaticModel::AddBox(const BoxDesc& desc)
    {
        AddStaticMesh(CreateBoxMesh(desc), desc.material, desc.albedoTexturePath, desc.occlusionTexturePath);
    }

    void StaticModel::AddSphere(const SphereDesc& desc)
    {
        AddStaticMesh(CreateSphereMesh(desc), desc.material, desc.albedoTexturePath, desc.occlusionTexturePath);
    }

    bool StaticModel::SetMaterial(size_t meshIndex, const SurfaceMaterial& material)
    {
        return MeshComponentRef().SetMaterial(meshIndex, material);
    }

    const SurfaceMaterial* StaticModel::GetMaterial(size_t meshIndex) const
    {
        return MeshComponentRef().GetMaterial(meshIndex);
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
