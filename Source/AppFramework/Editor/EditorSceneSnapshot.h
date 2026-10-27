#pragma once

#include "AppFramework/Light/DirectionalLight.h"
#include "Foundation/Math/Rotation3.h"
#include "Foundation/Math/Vector3.h"

#include <array>
#include <cstdint>
#include <vector>

namespace SasamiEngine::App
{
    class ApplicationCore;
    class Camera;
    class PointLight;
    class SkinnedModel;
    class SpotLight;
    class StaticModel;

    struct EditorSceneRestoreReport
    {
        uint32_t restored = 0;  // objects written back
        uint32_t missing = 0;   // captured objects that no longer exist (skipped)
        uint32_t added = 0;     // objects that exist now but were not captured (left as they are)
    };

    // Copy of the editable scene state (what the Inspector edits): model matrices, point/spot light
    // transform/colour/intensity/range/angles, the directional light and every camera's orbit state.
    // Restore writes it back to the objects that still exist; objects are matched by pointer, and the
    // pointer is only dereferenced after it is found in the application's current object lists.
    // Objects created after Capture are not removed (known limitation of the play session).
    class EditorSceneSnapshot
    {
    public:
        void Capture(const ApplicationCore& app);
        EditorSceneRestoreReport Restore(ApplicationCore& app) const;
        bool IsEmpty() const { return !m_captured; }
        void Clear();

    private:
        struct ModelState
        {
            const void* object = nullptr;
            std::array<float, 16> matrix{};
        };
        struct LightState
        {
            const void* object = nullptr;
            Vector3 position;
            Rotation3 rotation;
            std::array<float, 4> color{};
            float intensity = 0.0f;
            float range = 0.0f;
            float innerAngle = 0.0f;
            float outerAngle = 0.0f;
        };
        struct CameraState
        {
            const Camera* object = nullptr;
            Vector3 target;
            float yaw = 0.0f;
            float pitch = 0.0f;
            float distance = 0.0f;
            float nearClip = 0.0f;
            float farClip = 0.0f;
            float moveSpeed = 0.0f;
        };

        bool m_captured = false;
        std::vector<ModelState> m_staticModels;
        std::vector<ModelState> m_skinnedModels;
        std::vector<LightState> m_pointLights;
        std::vector<LightState> m_spotLights;
        std::vector<CameraState> m_cameras;
        DirectionalLight m_directionalLight;
    };
}
