#pragma once

#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"
#include "Renderer/Voxel/WindVelocityField.h"

#include <cstdint>

namespace SasamiEngine::Render::Voxel
{
    class SignedDistanceGrid;

    // Divergence-free wind that slides around the solids of a voxel SDF: curl-noise with
    // boundaries (Bridson, Hourihan & Nordenstam 2007, "Curl-Noise for Procedural Fluid Flow").
    //   psi(x)  = 0.5 * wind x (x - x0)           laminar part, curl = wind (cf. eq. 6 with omega = 0)
    //           + turbulence * |wind| * noiseScale * N((x - t * wind) / noiseScale)   scrolls downwind
    //   psi_c   = a * psi + (1 - a) * n * (n . psi),  a = |ramp(d / rampWidth)|   eq. (5)
    //   ramp(r) = 15/8 r - 10/8 r^3 + 3/8 r^5 on (-1, 1), clamped to +-1 outside    eq. (4)
    //   v       = curl psi_c, by central differences
    // d is the SDF in world units and n its normalized gradient. Near sharp voxel edges n is
    // discontinuous and the paper falls back to eq. (3); there is no sharp-edge flagging here,
    // so velocity spikes there are clamped to maxSpeedScale * |wind| instead (known deviation).
    struct SdfWindSettings
    {
        float wind[3] = { 3.0f, 0.0f, 0.0f };  // m/s, far-field velocity
        float rampWidth = 2.0f;                // m, d0
        float turbulence = 0.0f;               // noise potential amplitude relative to |wind|
        float noiseScale = 4.0f;               // m, noise length scale L
        float time = 0.0f;                     // s, scrolls the noise downwind
        float maxSpeedScale = 4.0f;            // clamp on |v| / |wind|
        // False drops the laminar term, leaving only the ramped noise (wind still sets its scroll and
        // amplitude). GustedWindField adds that to a projected base wind.
        bool laminar = true;
    };

    class SdfWindField final : public IWindVelocityField
    {
    public:
        // Keeps references; field and desc must outlive this object. x0 is the grid centre.
        SdfWindField(const SignedDistanceGrid& field, const VoxelGridDesc& desc, const SdfWindSettings& settings);

        void SetTime(float time) { m_settings.time = time; }
        const SdfWindSettings& Settings() const { return m_settings; }
        const VoxelGridDesc& Desc() const override { return m_desc; }

        // Signed distance in world units at a world point (trilinear, clamped to the grid).
        float Distance(const float position[3]) const override;
        // Normalized SDF gradient (outward normal) at a world point; zero if degenerate.
        void Normal(const float position[3], float outNormal[3]) const;
        // Constrained vector potential psi_c at a world point.
        void Potential(const float position[3], float outPsi[3]) const;
        // v = curl psi_c at a world point (m/s), clamped as described above.
        void Velocity(const float position[3], float outVelocity[3]) const override;

        // True if the point is inside the grid AABB.
        bool Contains(const float position[3]) const override;

    private:
        // Origin x0 of the linear potential 0.5 * wind x (x - x0). Eq. (5) adds ramp' * psi near walls, so a far x0
        // (the grid centre) makes a large tangential jet there. Inside the ramp band x0 is the closest surface
        // point p - d n, which keeps psi ~ d; outside it ramp' = 0 and the gauge has no effect.
        void Gauge(const float position[3], float outOrigin[3]) const;
        void PotentialWithGauge(const float position[3], const float origin[3], float outPsi[3]) const;

        const SignedDistanceGrid& m_field;
        VoxelGridDesc m_desc;
        SdfWindSettings m_settings;
        float m_center[3];
    };

    // Eq. (4) of Bridson et al. 2007.
    float CurlNoiseRamp(float r);
}

namespace SasamiEngine { using namespace Render; }
