#include "AppFramework/Debug/SmokeSurfaceOverlay.h"

#include "AppFramework/ApplicationCore.h"
#include "Renderer/Fluid/IsoSurfaceMesh.h"
#include "Renderer/Fluid/ParticleSplatGrid.h"
#include "Renderer/Runtime/Renderer.h"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        // The material carries the colour; the vertex colour only multiplies it.
        void WhiteVertex(float, float outRgba[4])
        {
            outRgba[0] = outRgba[1] = outRgba[2] = outRgba[3] = 1.0f;
        }
    }

    std::string SmokeSurfaceOverlay::Enable(ApplicationCore& app, float iso)
    {
        Disable(app);
        SmokeVolumeSim& sim = app.GetRenderer().GetSmokeSimMutable();
        const float cell = sim.GetCellSize();
        const float boundsMin[3] = { sim.GetOriginX(), sim.GetOriginY(), sim.GetOriginZ() };
        const float boundsMax[3] = { boundsMin[0] + cell * float(sim.GetCountX()),
                                     boundsMin[1] + cell * float(sim.GetCountY()),
                                     boundsMin[2] + cell * float(sim.GetCountZ()) };
        const std::string result = m_meshes.Enable(app, boundsMin, boundsMax);
        if (result.rfind("OK", 0) != 0) {
            return result;
        }
        sim.SetDensityStreaming(true);
        m_enabled = true;
        m_iso = iso;
        m_lastRecord = UINT64_MAX;
        m_meshedFrames = 0;
        m_triangles = 0;
        return "OK live=1 " + result.substr(3);
    }

    void SmokeSurfaceOverlay::Disable(ApplicationCore& app)
    {
        if (!m_enabled) {
            return;
        }
        m_meshes.Disable(app);
        app.GetRenderer().GetSmokeSimMutable().SetDensityStreaming(false);
        m_enabled = false;
    }

    void SmokeSurfaceOverlay::Tick(ApplicationCore& app)
    {
        if (!m_enabled) {
            return;
        }
        if (!m_meshes.IsEnabled(app)) {
            // The model was deleted (scene load, editor): stop streaming too.
            app.GetRenderer().GetSmokeSimMutable().SetDensityStreaming(false);
            m_enabled = false;
            return;
        }
        SmokeVolumeSim& sim = app.GetRenderer().GetSmokeSimMutable();
        uint64_t record = 0;
        if (!sim.ReadStreamedDensity(m_density, record) || record == m_lastRecord) {
            return;
        }
        m_lastRecord = record;

        const auto start = std::chrono::steady_clock::now();
        Render::Fluid::SplatGridDesc desc;
        desc.origin[0] = sim.GetOriginX();
        desc.origin[1] = sim.GetOriginY();
        desc.origin[2] = sim.GetOriginZ();
        desc.cellSize = sim.GetCellSize();
        desc.dims[0] = sim.GetCountX();
        desc.dims[1] = sim.GetCountY();
        desc.dims[2] = sim.GetCountZ();
        Render::Fluid::IsoSurfaceMesh mesh;
        Render::Fluid::ExtractIsoSurface(desc, m_density.data(), m_iso, 0.0f, mesh);
        m_meshMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        m_triangles = mesh.TriangleCount();
        m_openEdges = Render::Fluid::CountOpenEdges(mesh);

        const Render::Fluid::IsoSurfaceMesh none;
        m_meshes.Update(app, none, WhiteVertex, mesh, WhiteVertex);
        ++m_meshedFrames;
    }

    std::string SmokeSurfaceOverlay::Status() const
    {
        std::ostringstream out;
        out << std::fixed << std::setprecision(3);
        out << "OK live=" << (m_enabled ? 1 : 0) << " iso=" << m_iso
            << " record=" << (m_lastRecord == UINT64_MAX ? -1 : static_cast<int64_t>(m_lastRecord))
            << " meshedFrames=" << m_meshedFrames << " tris=" << m_triangles
            << " drawnTris=" << m_meshes.SmokeTriangles() << " dropped=" << m_meshes.DroppedTriangles()
            << " openEdges=" << m_openEdges << " meshMs=" << m_meshMs
            << " pushMs=" << m_meshes.LastPushMs() << " pendingFrames=" << m_meshes.PendingFrames();
        return out.str();
    }
}
