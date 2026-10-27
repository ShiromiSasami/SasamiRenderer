#include "Renderer/Voxel/ProjectedWindField.h"

#include "Renderer/Voxel/OccupancyBitGrid.h"
#include "Renderer/Voxel/SdfSphereTracer.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"

namespace SasamiEngine::Render::Voxel
{
    ProjectedWindField::ProjectedWindField(const OccupancyBitGrid& occupancy, const SignedDistanceGrid& field,
                                           const VoxelGridDesc& desc, const ProjectedWindSettings& settings,
                                           const std::vector<float>* warmStartPressure)
        : m_field(field)
        , m_desc(desc)
    {
        m_velocity.Reset(field.DimX(), field.DimY(), field.DimZ());
        m_velocity.Fill(settings.wind);
        if (warmStartPressure) {
            m_pressure = *warmStartPressure;
        }
        m_stats = ProjectDivergenceFree(occupancy, settings.projection, m_velocity, &m_pressure);
    }

    float ProjectedWindField::Distance(const float position[3]) const
    {
        const float inv = 1.0f / m_desc.voxelSize;
        const float vx = (position[0] - m_desc.origin[0]) * inv;
        const float vy = (position[1] - m_desc.origin[1]) * inv;
        const float vz = (position[2] - m_desc.origin[2]) * inv;
        return SampleSdfTrilinear(m_field, vx, vy, vz) * m_desc.voxelSize;
    }

    void ProjectedWindField::Velocity(const float position[3], float outVelocity[3]) const
    {
        if (m_desc.voxelSize <= 0.0f) {
            outVelocity[0] = outVelocity[1] = outVelocity[2] = 0.0f;
            return;
        }
        const float inv = 1.0f / m_desc.voxelSize;
        m_velocity.Sample((position[0] - m_desc.origin[0]) * inv, (position[1] - m_desc.origin[1]) * inv,
                          (position[2] - m_desc.origin[2]) * inv, outVelocity);
    }

    bool ProjectedWindField::Contains(const float position[3]) const
    {
        const float dims[3] = { static_cast<float>(m_desc.dimX), static_cast<float>(m_desc.dimY),
                                static_cast<float>(m_desc.dimZ) };
        for (int axis = 0; axis < 3; ++axis) {
            const float low = m_desc.origin[axis];
            const float high = low + dims[axis] * m_desc.voxelSize;
            if (position[axis] < low || position[axis] > high) {
                return false;
            }
        }
        return true;
    }
}
