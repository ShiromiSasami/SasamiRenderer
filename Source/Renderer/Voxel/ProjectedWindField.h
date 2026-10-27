#pragma once

#include "Renderer/Voxel/PressureProjector.h"
#include "Renderer/Voxel/StaggeredVelocityGrid.h"
#include "Renderer/Voxel/WindVelocityField.h"

namespace SasamiEngine::Render::Voxel
{
    class OccupancyBitGrid;
    class SignedDistanceGrid;

    struct ProjectedWindSettings
    {
        float wind[3] = { 3.0f, 0.0f, 0.0f };  // m/s, the uniform wind before projection
        PressureProjectionSettings projection;
    };

    // Steady inviscid wind that goes around the solids (6a-3): the uniform wind is put on a MAC grid
    // matching the voxel grid and made divergence-free by one pressure projection, so flow blocked by
    // a building is pushed over and around it instead of only bending inside SdfWindField's ramp band.
    // No turbulence and no time dependence (it is a potential-flow-like steady state, not a simulation).
    class ProjectedWindField final : public IWindVelocityField
    {
    public:
        // occupancy, field and desc must have the same dims. Keeps a reference to field, which must
        // outlive this object; builds and projects the velocity in the constructor. warmStartPressure, when
        // given, is the initial pressure guess (e.g. Pressure() of an earlier field on the same grid).
        ProjectedWindField(const OccupancyBitGrid& occupancy, const SignedDistanceGrid& field, const VoxelGridDesc& desc,
                           const ProjectedWindSettings& settings, const std::vector<float>* warmStartPressure = nullptr);

        const VoxelGridDesc& Desc() const override { return m_desc; }
        // Same as SdfWindField::Distance: trilinear SDF, world units.
        float Distance(const float position[3]) const override;
        // Trilinear MAC sample (StaggeredVelocityGrid::Sample), m/s.
        void Velocity(const float position[3], float outVelocity[3]) const override;
        bool Contains(const float position[3]) const override;

        const PressureProjectionStats& Stats() const { return m_stats; }
        const StaggeredVelocityGrid& Grid() const { return m_velocity; }
        // Solved pressure, one float per cell (dt / (rho * dx) folded in).
        const std::vector<float>& Pressure() const { return m_pressure; }

    private:
        const SignedDistanceGrid& m_field;
        VoxelGridDesc m_desc;
        StaggeredVelocityGrid m_velocity;
        PressureProjectionStats m_stats;
        std::vector<float> m_pressure;
    };
}

namespace SasamiEngine { using namespace Render; }
