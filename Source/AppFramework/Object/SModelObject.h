#pragma once

#include "Object/SObject.h"
#include "Object/MeshComponent.h"

namespace SasamiRenderer
{
    class SModelObject : public SObject, public MeshComponent
    {
    public:
        using ModelFormat = MeshComponent::ModelFormat;
    };
}
