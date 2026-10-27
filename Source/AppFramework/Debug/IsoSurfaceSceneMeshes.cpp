#include "AppFramework/Debug/IsoSurfaceSceneMeshes.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Object/StaticModel.h"
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Scene/SurfaceMaterialFactory.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        // Different capacities, so the two meshes can be told apart among the submitted ones by vertex count.
        // Bistro roof fires reached 140-160k triangles with the ImGui surface, mostly flames.
        constexpr uint32_t FLAME_MAX_TRIANGLES = 131072;
        constexpr uint32_t SMOKE_MAX_TRIANGLES = 65536;
    }

    std::string IsoSurfaceSceneMeshes::Enable(ApplicationCore& app, const float boundsMin[3], const float boundsMax[3])
    {
        Disable(app);
        for (int c = 0; c < 3; ++c) {
            m_boundsMin[c] = boundsMin[c];
            m_boundsMax[c] = boundsMax[c];
        }
        if (m_flame.Capacity() != FLAME_MAX_TRIANGLES) {
            m_flame.Configure(FLAME_MAX_TRIANGLES);
        }
        if (m_smoke.Capacity() != SMOKE_MAX_TRIANGLES) {
            m_smoke.Configure(SMOKE_MAX_TRIANGLES);
        }
        const Render::Fluid::IsoSurfaceMesh empty;
        m_flame.Update(empty, nullptr, m_boundsMin, m_boundsMax);
        m_smoke.Update(empty, nullptr, m_boundsMin, m_boundsMax);

        // Albedo is the vertex colour (baseColor 1). FluidApp has no IBL, so both get a little emissive or
        // faces turned away from the sun go black; the flame's orange glow is by eye.
        SurfaceMaterial flame = MakeOpaqueSurfaceMaterial(1.0f, 1.0f, 1.0f, 0.9f, 0.0f);
        flame.emissive[0] = 1.6f;
        flame.emissive[1] = 0.7f;
        flame.emissive[2] = 0.2f;
        SurfaceMaterial smoke = MakeOpaqueSurfaceMaterial(1.0f, 1.0f, 1.0f, 1.0f, 0.0f);
        for (int c = 0; c < 3; ++c) {
            smoke.emissive[c] = 0.06f;
        }
        m_model = app.CreateStaticModel();
        m_model->AddStaticMesh(m_flame.GetMesh(), flame);
        m_model->AddStaticMesh(m_smoke.GetMesh(), smoke);
        // Kept out of the cluster (mesh-shader) path, which never sees vertex updates; the vertex path
        // draws them in either mode.
        m_model->SetDynamicVertices(0, true);
        m_model->SetDynamicVertices(1, true);
        app.InvalidateRenderObjects();
        m_flameIndex = SIZE_MAX;
        m_smokeIndex = SIZE_MAX;
        m_pendingFrames = 0;

        std::ostringstream out;
        out << "OK scene surfaces flameCapacity=" << FLAME_MAX_TRIANGLES << " smokeCapacity=" << SMOKE_MAX_TRIANGLES
            << " bounds=" << m_boundsMin[0] << "," << m_boundsMin[1] << "," << m_boundsMin[2]
            << ".." << m_boundsMax[0] << "," << m_boundsMax[1] << "," << m_boundsMax[2];
        return out.str();
    }

    void IsoSurfaceSceneMeshes::Disable(ApplicationCore& app)
    {
        if (IsEnabled(app)) {
            app.DeleteObject(m_model);
            app.InvalidateRenderObjects();
        }
        m_model = nullptr;
        m_flameIndex = SIZE_MAX;
        m_smokeIndex = SIZE_MAX;
    }

    bool IsoSurfaceSceneMeshes::IsEnabled(const ApplicationCore& app)
    {
        if (!m_model) {
            return false;
        }
        const std::vector<StaticModel*> models = app.GetStaticModels();
        if (std::find(models.begin(), models.end(), m_model) == models.end()) {
            m_model = nullptr;
        }
        return m_model != nullptr;
    }

    bool IsoSurfaceSceneMeshes::Update(ApplicationCore& app, const Render::Fluid::IsoSurfaceMesh& flame,
                                       Render::Fluid::IsoSurfaceVertexColor flameColor,
                                       const Render::Fluid::IsoSurfaceMesh& smoke,
                                       Render::Fluid::IsoSurfaceVertexColor smokeColor)
    {
        if (!IsEnabled(app)) {
            return false;
        }
        const auto start = std::chrono::steady_clock::now();
        m_flame.Update(flame, flameColor, m_boundsMin, m_boundsMax);
        m_smoke.Update(smoke, smokeColor, m_boundsMin, m_boundsMax);
        const auto uploadStart = std::chrono::steady_clock::now();
        m_uploadBytes = 0;
        const size_t submitted = app.GetRenderer().GetSceneSubmitter().GetMeshes().size();
        if (submitted != m_submittedCount) {
            // A rebuild may have moved the meshes.
            m_flameIndex = SIZE_MAX;
            m_smokeIndex = SIZE_MAX;
            m_submittedCount = submitted;
        }
        const bool flameOk = Push(app, m_flame, m_flameIndex);
        const bool smokeOk = Push(app, m_smoke, m_smokeIndex);
        if (!flameOk || !smokeOk) {
            ++m_pendingFrames;
        }
        const auto end = std::chrono::steady_clock::now();
        m_pushMs = std::chrono::duration<double, std::milli>(end - start).count();
        m_uploadMs = std::chrono::duration<double, std::milli>(end - uploadStart).count();
        return true;
    }

    bool IsoSurfaceSceneMeshes::Push(ApplicationCore& app, const Render::Fluid::IsoSurfaceDynamicMesh& dynamic, size_t& meshIndex)
    {
        Renderer& renderer = app.GetRenderer();
        const Render::Mesh& mesh = dynamic.GetMesh();
        if (meshIndex == SIZE_MAX) {
            // Only these meshes have exactly this vertex count and an identity index buffer of this length.
            const std::vector<Render::Mesh>& meshes = renderer.GetSceneSubmitter().GetMeshes();
            for (size_t i = 0; i < meshes.size(); ++i) {
                if (meshes[i].vertices.size() == mesh.vertices.size() && meshes[i].indices == mesh.indices) {
                    meshIndex = i;
                    break;
                }
            }
            if (meshIndex == SIZE_MAX) {
                return false;   // not submitted yet (the rebuild runs on a later frame)
            }
        }
        // Only the dirty slots: the ones past it still hold collapsed points, and the bounds vertices never move
        // after Enable (the ring copies start from the uploaded mesh, which has them). The ring also rewrites
        // whatever its next copy missed while other copies were current, so the bytes written can exceed this.
        const size_t dirty = dynamic.DirtyVertexCount();
        size_t written = 0;
        bool ok = renderer.UpdateStaticMeshDynamicVertices(meshIndex, mesh.vertices.data(), mesh.vertices.size(), 0,
                                                           dirty, &written);
        // Draw only the written slots: the collapsed tail still cost a full vertex pass per view (smoke.mesh
        // live with 0 triangles read Shadow 9.3 ms / GBuffer 3.2 ms). Slot 0 is a collapsed point when
        // nothing is written, and a count of 0 would make the draw paths fall back to Draw(vertexCount).
        const size_t drawSlots = dynamic.Written() > 0 ? dynamic.Written() : 1;
        ok = ok && renderer.SetStaticMeshDrawIndexCount(meshIndex, drawSlots * 3);
        if (!ok) {
            meshIndex = SIZE_MAX;
            return false;
        }
        m_uploadBytes += written * sizeof(Render::Vertex);
        return true;
    }
}
