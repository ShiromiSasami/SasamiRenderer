#pragma once

#include "Renderer/Voxel/CpuSurfaceVoxelizer.h"

namespace SasamiEngine::Render::Voxel
{
    // What WindParticleSystem needs from a wind model placed on a voxel grid: the grid box, the
    // signed distance to the solids (world units, negative inside) and the velocity (m/s), all at
    // world points. SdfWindField (curl noise) and ProjectedWindField (pressure projection) implement it.
    class IWindVelocityField
    {
    public:
        virtual ~IWindVelocityField() = default;

        virtual const VoxelGridDesc& Desc() const = 0;
        virtual float Distance(const float position[3]) const = 0;
        virtual void Velocity(const float position[3], float outVelocity[3]) const = 0;
        // True if the point is inside the grid AABB.
        virtual bool Contains(const float position[3]) const = 0;
    };
}

namespace SasamiEngine { using namespace Render; }
