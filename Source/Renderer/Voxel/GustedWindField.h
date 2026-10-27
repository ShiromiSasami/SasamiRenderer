#pragma once

#include "Renderer/Voxel/WindVelocityField.h"

namespace SasamiEngine::Render::Voxel
{
    // Steady base wind plus curl-noise gusts (6a-4a). base routes the flow around solids (ProjectedWindField),
    // gusts is an SdfWindField with laminar = false, so it only holds the ramped noise of Bridson et al. 2007.
    // Both are divergence-free and tangential at walls, so their sum is too (up to the finite differences).
    // Distance and Desc come from the base field. Gusts also blow inside sealed rooms that the base leaves still.
    class GustedWindField final : public IWindVelocityField
    {
    public:
        // Keeps references; both fields must outlive this object and share one voxel grid.
        GustedWindField(const IWindVelocityField& base, const IWindVelocityField& gusts);

        const VoxelGridDesc& Desc() const override { return m_base.Desc(); }
        float Distance(const float position[3]) const override { return m_base.Distance(position); }
        void Velocity(const float position[3], float outVelocity[3]) const override;
        bool Contains(const float position[3]) const override { return m_base.Contains(position); }

    private:
        const IWindVelocityField& m_base;
        const IWindVelocityField& m_gusts;
    };
}

namespace SasamiEngine { using namespace Render; }
