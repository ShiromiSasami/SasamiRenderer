#include "Renderer/Voxel/GustedWindField.h"

namespace SasamiEngine::Render::Voxel
{
    GustedWindField::GustedWindField(const IWindVelocityField& base, const IWindVelocityField& gusts)
        : m_base(base)
        , m_gusts(gusts)
    {
    }

    void GustedWindField::Velocity(const float position[3], float outVelocity[3]) const
    {
        float baseVelocity[3];
        float gustVelocity[3];
        m_base.Velocity(position, baseVelocity);
        m_gusts.Velocity(position, gustVelocity);
        for (int axis = 0; axis < 3; ++axis) {
            outVelocity[axis] = baseVelocity[axis] + gustVelocity[axis];
        }
    }
}
