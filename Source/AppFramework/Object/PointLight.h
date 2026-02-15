#pragma once

#include <cassert>

#include "Object/SObject.h"
#include "AppFramework/Component/TransformComponent.h"
#include "Foundation/Math/Color.h"
#include "Renderer/Scene/RenderLightProxy.h"

namespace SasamiRenderer
{
    class PointLight : public SObject
    {
    public:
        PointLight() { AddComponent<TransformComponent>(); }

        TransformComponent& Transform()
        {
            TransformComponent* component = GetComponent<TransformComponent>();
            assert(component && "PointLight requires TransformComponent.");
            return *component;
        }
        const TransformComponent& Transform() const
        {
            const TransformComponent* component = GetComponent<TransformComponent>();
            assert(component && "PointLight requires TransformComponent.");
            return *component;
        }

        float Range() const { return m_range; }
        void SetRange(float value) { m_range = value; }

        float Intensity() const { return m_intensity; }
        void SetIntensity(float value) { m_intensity = value; }

        float* ColorData() { return m_color.Data(); }
        const float* ColorData() const { return m_color.Data(); }

        RenderPointLight BuildRenderLightProxy() const
        {
            const TransformComponent& transform = Transform();
            RenderPointLight out{};
            out.pos[0] = transform.position.x;
            out.pos[1] = transform.position.y;
            out.pos[2] = transform.position.z;
            out.range = m_range;
            out.color[0] = m_color.r;
            out.color[1] = m_color.g;
            out.color[2] = m_color.b;
            out.intensity = m_intensity;
            return out;
        }

    private:
        float m_range = 5.0f;
        Color m_color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        float m_intensity = 1.0f;
    };
}
