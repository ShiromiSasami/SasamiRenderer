#pragma once

#include "Renderer/Fluid/IsoSurfaceDynamicMesh.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace SasamiEngine::App
{
    class ApplicationCore;
    class StaticModel;

    namespace Debug
    {
        // Draws the wind effects' flame and smoke iso-surfaces as two opaque StaticModels in the frame
        // (GBuffer, lit, shadowed) instead of ImGui triangles. Each model holds one IsoSurfaceDynamicMesh
        // with a fixed capacity, and Update() writes its vertices every frame through
        // Renderer::UpdateStaticMeshDynamicVertices (a ring of vertex buffer copies, so a frame in flight never
        // reads a copy being rewritten), so the scene is not rebuilt per frame.
        // Debt: the submitter's bounds / RT BLAS keep the upload-time state (the meshes are marked
        // dynamicVertices, so the cluster path skips them). Opaque only: smoke is a solid grey hull, and flames get the
        // blackbody colour in the albedo plus one constant emissive (the material has no per-vertex emissive).
        class IsoSurfaceSceneMeshes
        {
        public:
            // Adds the two models with empty meshes. Surfaces must stay inside [boundsMin, boundsMax]: the
            // renderer culls the models by the box computed at upload. Returns "OK ..." or "ERR ...".
            std::string Enable(ApplicationCore& app, const float boundsMin[3], const float boundsMax[3]);
            void Disable(ApplicationCore& app);
            // False when the models are gone (disabled, or deleted by a scene load / the editor).
            bool IsEnabled(const ApplicationCore& app);
            // Writes both surfaces into the models' vertex buffers. Returns false when not enabled.
            bool Update(ApplicationCore& app, const Render::Fluid::IsoSurfaceMesh& flame,
                        Render::Fluid::IsoSurfaceVertexColor flameColor,
                        const Render::Fluid::IsoSurfaceMesh& smoke,
                        Render::Fluid::IsoSurfaceVertexColor smokeColor);

            uint32_t FlameTriangles() const { return m_flame.Written(); }
            uint32_t SmokeTriangles() const { return m_smoke.Written(); }
            uint32_t DroppedTriangles() const { return m_flame.Dropped() + m_smoke.Dropped(); }
            double LastPushMs() const { return m_pushMs; }      // fill + upload
            double LastUploadMs() const { return m_uploadMs; }
            uint64_t LastUploadBytes() const { return m_uploadBytes; }
            // Frames whose push did not reach the GPU (mesh not submitted yet after a rebuild, or write failed).
            uint64_t PendingFrames() const { return m_pendingFrames; }

        private:
            // Uploads the dirty slot range of `mesh`.
            bool Push(ApplicationCore& app, const Render::Fluid::IsoSurfaceDynamicMesh& mesh, size_t& meshIndex);

            StaticModel* m_model = nullptr;   // trusted only while it is in the app's static model list
            Render::Fluid::IsoSurfaceDynamicMesh m_flame;
            Render::Fluid::IsoSurfaceDynamicMesh m_smoke;
            size_t m_flameIndex = SIZE_MAX;   // index among SceneSubmitter::GetMeshes(), SIZE_MAX = look up
            size_t m_smokeIndex = SIZE_MAX;
            size_t m_submittedCount = 0;      // GetMeshes().size() when the indices were found
            float m_boundsMin[3] = { 0.0f, 0.0f, 0.0f };
            float m_boundsMax[3] = { 0.0f, 0.0f, 0.0f };
            double m_pushMs = 0.0;
            double m_uploadMs = 0.0;
            uint64_t m_uploadBytes = 0;
            uint64_t m_pendingFrames = 0;
        };
    }
}
namespace SasamiEngine { using namespace App; }
