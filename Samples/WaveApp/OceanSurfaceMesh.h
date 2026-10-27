#pragma once

#include "CoastalWindField.h"
#include "GerstnerWaveSet.h"
#include "Renderer/Structures/Mesh.h"

#include <cstdint>
#include <vector>

namespace SasamiEngine::Sample::Wave
{
    struct OceanSurfaceSettings
    {
        uint32_t resolution = 128;      // quads per side
        float size = 60.0f;             // m, square centred on the origin
        float foamJacobian = 0.45f;     // J below this starts whitecap foam
        float foamDecay = 1.6f;         // s, foam *= exp(-dt / decay)
        float shoreFoamDepth = 0.6f;    // m, water shallower than this foams (surf line)
        float shoalDepth = 3.0f;        // m, amplitude scale = clamp(depth / shoalDepth, 0.15, 1)
        float waterColor[3] = { 0.03f, 0.18f, 0.24f };
        float foamColor[3] = { 0.92f, 0.95f, 0.96f };
    };

    // Returns the sea-bed height at a point; the water's rest level is y = 0, so depth = -height.
    using SeaBedFunction = float (*)(float x, float z);

    struct OceanSurfaceStats
    {
        uint32_t vertices = 0;
        uint32_t foamVertices = 0;   // foam > 0.5
        float minJacobian = 0.0f;
        float maxHeight = 0.0f;
        float meanGust = 1.0f;       // mean wind gust factor over the vertices (1 without wind)
    };

    // CPU grid of Gerstner-displaced vertices with a per-vertex foam amount carried between frames.
    // Foam source: the crest Jacobian (Tessendorf 2001 sec.4.6, fold-over -> whitecap), plus a shore
    // band where the water is shallower than shoreFoamDepth. Foam is kept with an exponential decay
    // (foam = max(foam * exp(-dt/decay), source)), as in common Jacobian-foam implementations. The
    // colour is a lerp of water and foam colour written into the vertex colour (GBufferEncode
    // multiplies albedo by it). Vertices over dry sea bed are pushed below it so the beach hides them.
    class OceanSurfaceMesh
    {
    public:
        void Configure(const OceanSurfaceSettings& settings, SeaBedFunction seaBed);
        const OceanSurfaceSettings& Settings() const { return m_settings; }
        OceanSurfaceSettings& MutableSettings() { return m_settings; }

        // Re-evaluates every vertex at `time` and advances foam by dt. With `wind`, each vertex passes
        // the local gust factor to the waves (gust-responsive ripples), sampled at the same time.
        void Update(const GerstnerWaveSet& waves, float time, float dt, const CoastalWindField* wind = nullptr);

        const Mesh& GetMesh() const { return m_mesh; }
        const OceanSurfaceStats& Stats() const { return m_stats; }

    private:
        OceanSurfaceSettings m_settings;
        SeaBedFunction m_seaBed = nullptr;
        Mesh m_mesh;
        std::vector<float> m_foam;
        OceanSurfaceStats m_stats;
    };
}
