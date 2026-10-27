#pragma once

#include "Object/StaticModel.h"

namespace SasamiEngine::App
{
    // Assets/-relative path of the Lumberyard Bistro exterior used by several samples.
    constexpr const char* BISTRO_EXTERIOR_PATH = "Models/Bistro/BistroExterior.fbx";

    // The 12 x 0.2 x 12 m floor slab of the sample scenes; its top face sits exactly at y = 0.
    inline void AddSampleFloorBox(StaticModel& model, const SurfaceMaterial& material)
    {
        StaticModel::BoxDesc desc{};
        desc.width = 12.0f;
        desc.height = 0.2f;
        desc.depth = 12.0f;
        desc.material = material;
        model.AddBox(desc);
        model.SetTranslation(0.0f, -0.1f, 0.5f);
    }
}
