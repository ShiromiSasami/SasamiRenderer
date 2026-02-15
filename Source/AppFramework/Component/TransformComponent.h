#pragma once

#include "AppFramework/Component/IComponent.h"
#include "Foundation/Math/Rotation3.h"
#include "Foundation/Math/Vector3.h"

namespace SasamiRenderer
{
    struct TransformComponent : public IComponent
    {
        Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
        Rotation3 rotation = Rotation3(0.0f, 0.0f, 0.0f); // pitch, yaw, roll
        Vector3 scale = Vector3(1.0f, 1.0f, 1.0f);
    };
}
