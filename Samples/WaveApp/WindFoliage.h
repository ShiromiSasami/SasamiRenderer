#pragma once

#include "CoastalWindField.h"
#include "OceanSurfaceMesh.h"
#include "Renderer/Structures/Mesh.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Sample::Wave
{
    struct WindFoliageSettings
    {
        uint32_t bladeCount = 4000;
        uint32_t seed = 7;
        float areaMin[2] = { 16.0f, -30.0f };   // xz rectangle the blades are scattered over
        float areaMax[2] = { 30.0f, 30.0f };
        float minGroundHeight = 0.15f;          // only above the waterline (sea bed height > this)
        float heightMin = 0.45f;                // m, blade height range (dune grass)
        float heightMax = 0.95f;
        float width = 0.05f;                    // m at the root, tapering to 0 at the tip
        float stiffness = 0.005f;               // bend = clamp(stiffness * speed^2, 0, maxBend): drag ~ U^2; 0.012 saturated at 10 m/s and hid the gusts
        float maxBend = 1.2f;
        float flutter = 0.06f;                  // m of tip flutter at gust 1
        float flutterFrequency = 2.4f;          // Hz
        float rootColor[3] = { 0.30f, 0.36f, 0.14f };
        float tipColor[3] = { 0.78f, 0.74f, 0.42f };
    };

    struct WindFoliageStats
    {
        uint32_t blades = 0;
        uint32_t vertices = 0;
        float meanBend = 0.0f;      // mean of the per-blade bend factor this frame
        float maxBend = 0.0f;
        float meanGust = 0.0f;      // mean gust factor at the blade roots
    };

    // Dune grass on the dry beach, bent by CoastalWindField. Each blade is a 3-segment tapered strip
    // (4 rows x 2 verts) emitted twice, once per side, with opposite winding and normals so it shows
    // from both sides without a cull-none PSO. Bending follows Sousa 2007 (GPU Gems 3 ch.16, "main
    // bending"): offset_xz = wind_dir * bend * t^2 for the row at t = h / H, then (p - root) is
    // rescaled to its rest length so the blade swings instead of stretching. bend comes from the
    // wind sampled at the blade root (same field and time as the water ripples). Flutter is a
    // per-blade sine across the wind, scaled by the gust factor. Vertices are rewritten on the CPU
    // every frame, like the water.
    class WindFoliage
    {
    public:
        // Scatters blades (deterministic PCG-style hash of the seed) where groundHeight(x, z) is above
        // minGroundHeight, and builds the index buffer. Blade roots sit on the ground.
        void Configure(const WindFoliageSettings& settings, SeaBedFunction groundHeight);
        const WindFoliageSettings& Settings() const { return m_settings; }
        WindFoliageSettings& MutableSettings() { return m_settings; }   // bend/flutter/colour: no re-Configure needed

        void Update(const CoastalWindField& wind, float time);

        const Mesh& GetMesh() const { return m_mesh; }
        const WindFoliageStats& Stats() const { return m_stats; }

        static constexpr uint32_t ROWS = 4;              // root + 3 segments
        static constexpr uint32_t VERTS_PER_SIDE = ROWS * 2;
        static constexpr uint32_t VERTS_PER_BLADE = VERTS_PER_SIDE * 2;

    private:
        struct Blade
        {
            float root[3];
            float height;
            float facing[2];   // unit xz direction of the blade's width axis
            float phase;       // flutter phase, radians
            float lean[2];     // rest lean of the tip in xz, m (small, random)
        };

        WindFoliageSettings m_settings;
        std::vector<Blade> m_blades;
        Mesh m_mesh;
        WindFoliageStats m_stats;
    };

    // Checks: no wind -> tip at rest height above the root; wind along +x bends the tip toward +x and
    // lowers it; row distances from the root are preserved (length kept) within 1e-4; stronger wind
    // bends more; blades only sit above minGroundHeight.
    uint32_t RunWindFoliageSelfTest(std::vector<std::string>* failures, uint32_t* total);
}
