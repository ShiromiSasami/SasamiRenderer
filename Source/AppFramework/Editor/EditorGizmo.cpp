#include "AppFramework/Editor/EditorGizmo.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Editor/EditorPicking.h"
#include "AppFramework/Editor/EditorSelection.h"
#include "AppFramework/Editor/EditorTransformMath.h"
#include "AppFramework/Light/DirectionalLight.h"
#include "AppFramework/Object/Camera.h"
#include "AppFramework/Object/PointLight.h"
#include "AppFramework/Object/SkinnedModel.h"
#include "AppFramework/Object/SpotLight.h"
#include "AppFramework/Object/StaticModel.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Scene/RenderCameraProxy.h"

#include "imgui.h"
#include "ImGuizmo/ImGuizmo.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace SasamiEngine::App
{
    namespace
    {
        constexpr float MAX_PITCH = 89.9f * 0.01745329252f;
        constexpr float DIRECTIONAL_GIZMO_DISTANCE = 5.0f;

        using Foundation::Math::Dot3;

        ImGuizmo::OPERATION ToImGuizmo(EditorGizmoOperation operation)
        {
            switch (operation)
            {
            case EditorGizmoOperation::Rotate: return ImGuizmo::ROTATE;
            case EditorGizmoOperation::Scale: return ImGuizmo::SCALE;
            default: return ImGuizmo::TRANSLATE;
            }
        }
    }

    const char* EditorGizmoOperationName(EditorGizmoOperation operation)
    {
        switch (operation)
        {
        case EditorGizmoOperation::Rotate: return "rotate";
        case EditorGizmoOperation::Scale: return "scale";
        default: return "translate";
        }
    }

    const char* EditorGizmoSpaceName(EditorGizmoSpace space)
    {
        return space == EditorGizmoSpace::World ? "world" : "local";
    }

    void BuildEditorGizmoProjection(const Render::RenderCameraProxy& proxy, float out[16])
    {
        // The engine projects reverse-Z (near -> 1, far -> 0), so a point in front of the camera has a
        // negative clip z. ImGuizmo culls on clip z < 0.001 ("behind camera"), so it gets the same
        // projection with forward-Z depth terms instead; x, y and w are unchanged.
        std::memcpy(out, proxy.projection, sizeof(float) * 16);
        const float zn = proxy.nearClip;
        const float zf = proxy.farClip;
        out[10] = zf / (zf - zn);
        out[14] = -zn * zf / (zf - zn);
    }

    void BuildEditorViewMatrix(const Render::RenderCameraProxy& proxy, float out[16])
    {
        const float* r = proxy.cameraRight;
        const float* u = proxy.cameraUp;
        const float* f = proxy.cameraForward;
        const float* p = proxy.cameraPosition;
        const float view[16] = {
            r[0], u[0], f[0], 0.0f,
            r[1], u[1], f[1], 0.0f,
            r[2], u[2], f[2], 0.0f,
            -Dot3(p, r), -Dot3(p, u), -Dot3(p, f), 1.0f,
        };
        std::memcpy(out, view, sizeof(view));
    }

    void GizmoMatrixFromYawPitch(const float position[3], float yaw, float pitch, float out[16])
    {
        EditorTrs trs;
        trs.translation = Vector3(position[0], position[1], position[2]);
        trs.rotation = Rotation3(pitch, yaw, 0.0f);
        ComposeTrs(trs, out);
    }

    void YawPitchFromGizmoMatrix(const float m[16], float& yaw, float& pitch)
    {
        float f[3] = { m[8], m[9], m[10] };
        const float len = std::sqrt(Dot3(f, f));
        if (len <= 1e-8f)
        {
            return;
        }
        for (float& c : f)
        {
            c /= len;
        }
        pitch = std::asin((std::min)((std::max)(-f[1], -1.0f), 1.0f));
        pitch = (std::min)((std::max)(pitch, -MAX_PITCH), MAX_PITCH);
        if (f[0] * f[0] + f[2] * f[2] > 1e-12f)
        {
            yaw = std::atan2(f[0], f[2]);
        }
    }

    bool EditorGizmo::IsUsing() const
    {
        return ImGuizmo::IsUsingAny();
    }

    void EditorGizmo::Draw(ApplicationCore& app, EditorSelection& selection, bool enabled)
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (clicked)
        {
            m_clickGrabbed = false;
        }

        ImGuizmo::BeginFrame();
        Camera* camera = app.GetMainCamera();
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        if (!enabled || camera == nullptr || viewport == nullptr || viewport->Size.x <= 0.0f || viewport->Size.y <= 0.0f
            || selection.IsEmpty() || selection.kind == EditorObjectKind::Camera)
        {
            m_lastInputEnabled = false;
            ImGuizmo::Enable(false);
            return;
        }

        if (!io.WantCaptureKeyboard && !ImGuizmo::IsUsingAny() && ImGui::IsKeyPressed(ImGuiKey_Space, false))
        {
            m_operation = static_cast<EditorGizmoOperation>((static_cast<int>(m_operation) + 1) % 3);
        }

        const Render::RenderCameraProxy proxy = camera->BuildRenderCameraProxy(viewport->Size.x, viewport->Size.y);
        float view[16];
        BuildEditorViewMatrix(proxy, view);
        float projection[16];
        BuildEditorGizmoProjection(proxy, projection);

        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
        ImGuizmo::SetRect(viewport->Pos.x, viewport->Pos.y, viewport->Size.x, viewport->Size.y);
        m_lastInputEnabled = !io.WantCaptureMouse || ImGuizmo::IsUsingAny();
        ImGuizmo::Enable(m_lastInputEnabled);

        const ImGuizmo::MODE mode = m_space == EditorGizmoSpace::World ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
        float matrix[16];
        bool changed = false;
        switch (selection.kind)
        {
        case EditorObjectKind::StaticModel:
        case EditorObjectKind::SkinnedModel:
        {
            const float* model = GetSelectionModelMatrix(selection);
            if (model == nullptr)
            {
                return;
            }
            std::memcpy(matrix, model, sizeof(matrix));
            if (ImGuizmo::Manipulate(view, projection, ToImGuizmo(m_operation), mode, matrix))
            {
                if (selection.kind == EditorObjectKind::StaticModel)
                {
                    static_cast<StaticModel*>(selection.object)->MeshComponentRef().SetModelMatrix(matrix);
                }
                else
                {
                    static_cast<SkinnedModel*>(selection.object)->SkinnedMeshComponentRef().SetModelMatrix(matrix);
                }
                app.InvalidateRenderObjects();
                changed = true;
            }
            break;
        }
        case EditorObjectKind::PointLight:
        {
            TransformComponent& transform = static_cast<PointLight*>(selection.object)->Transform();
            const float position[3] = { transform.position.x, transform.position.y, transform.position.z };
            GizmoMatrixFromYawPitch(position, 0.0f, 0.0f, matrix);
            if (ImGuizmo::Manipulate(view, projection, ImGuizmo::TRANSLATE, ImGuizmo::WORLD, matrix))
            {
                transform.position = Vector3(matrix[12], matrix[13], matrix[14]);
                changed = true;
            }
            break;
        }
        case EditorObjectKind::SpotLight:
        {
            TransformComponent& transform = static_cast<SpotLight*>(selection.object)->Transform();
            const float position[3] = { transform.position.x, transform.position.y, transform.position.z };
            GizmoMatrixFromYawPitch(position, transform.rotation.yaw, transform.rotation.pitch, matrix);
            const ImGuizmo::OPERATION op = m_operation == EditorGizmoOperation::Rotate ? ImGuizmo::ROTATE : ImGuizmo::TRANSLATE;
            if (ImGuizmo::Manipulate(view, projection, op, mode, matrix))
            {
                transform.position = Vector3(matrix[12], matrix[13], matrix[14]);
                YawPitchFromGizmoMatrix(matrix, transform.rotation.yaw, transform.rotation.pitch);
                changed = true;
            }
            break;
        }
        case EditorObjectKind::DirectionalLight:
        {
            DirectionalLight light = app.GetDirectionalLight();
            float position[3];
            for (int i = 0; i < 3; ++i)
            {
                position[i] = proxy.cameraPosition[i] + proxy.cameraForward[i] * DIRECTIONAL_GIZMO_DISTANCE;
            }
            GizmoMatrixFromYawPitch(position, light.yaw, light.pitch, matrix);
            if (ImGuizmo::Manipulate(view, projection, ImGuizmo::ROTATE, mode, matrix))
            {
                YawPitchFromGizmoMatrix(matrix, light.yaw, light.pitch);
                app.SetDirectionalLight(light);
                changed = true;
            }
            break;
        }
        default:
            return;
        }

        if (clicked)
        {
            m_clickGrabbed = ImGuizmo::IsUsingAny();
        }
        if (changed)
        {
            ++selection.revision;
            ++m_editCount;
        }
    }

    int RunEditorGizmoSelfTest(int& checks)
    {
        int failures = 0;
        auto check = [&](bool ok) { ++checks; if (!ok) { ++failures; } };
        auto approx = [](float a, float b, float eps) { return std::fabs(a - b) <= eps; };

        // View matrix: the camera position maps to the origin, a point 3 m ahead to (0, 0, 3).
        Render::RenderCameraProxy proxy;
        const float yaw = 0.6f;
        const float pitch = -0.3f;
        Math::DirectionFromYawPitch(yaw, pitch, proxy.cameraForward);
        const float worldUp[3] = { 0.0f, 1.0f, 0.0f };
        const float* f = proxy.cameraForward;
        float r[3] = { worldUp[1] * f[2] - worldUp[2] * f[1], worldUp[2] * f[0] - worldUp[0] * f[2],
                       worldUp[0] * f[1] - worldUp[1] * f[0] };
        const float rl = std::sqrt(Dot3(r, r));
        for (float& c : r)
        {
            c /= rl;
        }
        std::memcpy(proxy.cameraRight, r, sizeof(r));
        proxy.cameraUp[0] = f[1] * r[2] - f[2] * r[1];
        proxy.cameraUp[1] = f[2] * r[0] - f[0] * r[2];
        proxy.cameraUp[2] = f[0] * r[1] - f[1] * r[0];
        proxy.cameraPosition[0] = 2.0f;
        proxy.cameraPosition[1] = 1.5f;
        proxy.cameraPosition[2] = -4.0f;
        float view[16];
        BuildEditorViewMatrix(proxy, view);
        auto transform = [&](const float p[3], float out[3])
        {
            for (int c = 0; c < 3; ++c)
            {
                out[c] = p[0] * view[c] + p[1] * view[4 + c] + p[2] * view[8 + c] + view[12 + c];
            }
        };
        float v[3];
        transform(proxy.cameraPosition, v);
        check(approx(v[0], 0.0f, 1e-5f) && approx(v[1], 0.0f, 1e-5f) && approx(v[2], 0.0f, 1e-5f));
        const float ahead[3] = { proxy.cameraPosition[0] + f[0] * 3.0f, proxy.cameraPosition[1] + f[1] * 3.0f,
                                 proxy.cameraPosition[2] + f[2] * 3.0f };
        transform(ahead, v);
        check(approx(v[0], 0.0f, 1e-5f) && approx(v[1], 0.0f, 1e-5f) && approx(v[2], 3.0f, 1e-5f));
        const float above[3] = { proxy.cameraPosition[0] + proxy.cameraUp[0], proxy.cameraPosition[1] + proxy.cameraUp[1],
                                 proxy.cameraPosition[2] + proxy.cameraUp[2] };
        transform(above, v);
        check(approx(v[1], 1.0f, 1e-5f));

        // Yaw/pitch round trip; row 2 equals DirectionFromYawPitch.
        const float position[3] = { 1.0f, 2.0f, 3.0f };
        const float cases[][2] = { { 0.0f, 0.0f }, { 0.7f, 1.0f }, { -2.5f, -0.8f }, { 3.0f, 1.5f } };
        for (const auto& c : cases)
        {
            float m[16];
            GizmoMatrixFromYawPitch(position, c[0], c[1], m);
            float dir[3];
            Math::DirectionFromYawPitch(c[0], c[1], dir);
            check(approx(m[8], dir[0], 1e-5f) && approx(m[9], dir[1], 1e-5f) && approx(m[10], dir[2], 1e-5f));
            check(m[12] == position[0] && m[13] == position[1] && m[14] == position[2]);
            float outYaw = 99.0f;
            float outPitch = 99.0f;
            YawPitchFromGizmoMatrix(m, outYaw, outPitch);
            check(approx(outYaw, c[0], 1e-4f) && approx(outPitch, c[1], 1e-4f));
        }

        // A scaled row 2 still decodes, and pitch is clamped short of the pole.
        float m[16];
        GizmoMatrixFromYawPitch(position, 0.4f, 0.2f, m);
        m[8] *= 3.0f;
        m[9] *= 3.0f;
        m[10] *= 3.0f;
        float outYaw = 0.0f;
        float outPitch = 0.0f;
        YawPitchFromGizmoMatrix(m, outYaw, outPitch);
        check(approx(outYaw, 0.4f, 1e-4f) && approx(outPitch, 0.2f, 1e-4f));
        m[8] = 0.0f;
        m[9] = -1.0f;
        m[10] = 0.0f;
        outYaw = 0.4f;
        YawPitchFromGizmoMatrix(m, outYaw, outPitch);
        check(approx(outPitch, MAX_PITCH, 1e-6f) && outYaw == 0.4f);
        // Gizmo projection: view-space depth near -> 0, far -> 1, midway inside (0, 1) (forward-Z).
        {
            Render::RenderCameraProxy cam;
            cam.nearClip = 0.1f;
            cam.farClip = 1000.0f;
            std::memset(cam.projection, 0, sizeof(cam.projection));
            cam.projection[11] = 1.0f;
            float proj[16];
            BuildEditorGizmoProjection(cam, proj);
            auto depthAt = [&](float z) { return (z * proj[10] + proj[14]) / (z * proj[11]); };
            check(approx(depthAt(cam.nearClip), 0.0f, 1e-5f));
            check(approx(depthAt(cam.farClip), 1.0f, 1e-5f));
            const float mid = depthAt(5.0f);
            check(mid > 0.001f && mid < 1.0f);
        }
        return failures;
    }
}
