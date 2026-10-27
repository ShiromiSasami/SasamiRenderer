#pragma once

#include <cstdint>

namespace SasamiEngine::Render
{
    struct RenderCameraProxy;
}

namespace SasamiEngine::App
{
    class ApplicationCore;
    struct EditorSelection;

    enum class EditorGizmoOperation : uint8_t
    {
        Translate,
        Rotate,
        Scale,
    };

    enum class EditorGizmoSpace : uint8_t
    {
        Local,
        World,
    };

    const char* EditorGizmoOperationName(EditorGizmoOperation operation);
    const char* EditorGizmoSpaceName(EditorGizmoSpace space);

    // Row-major, row-vector world-to-view matrix of a camera proxy (rows of the inverse are right, up,
    // forward), the convention ImGuizmo and the proxy's projection use.
    void BuildEditorViewMatrix(const Render::RenderCameraProxy& proxy, float out[16]);

    // The proxy's projection with forward-Z depth (near -> 0, far -> 1). ImGuizmo culls points whose
    // clip z is below 0.001, which is every visible point under the engine's reverse-Z projection.
    void BuildEditorGizmoProjection(const Render::RenderCameraProxy& proxy, float out[16]);

    // Light orientation <-> gizmo matrix: row 2 is Math::DirectionFromYawPitch(yaw, pitch), no roll,
    // unit scale. YawPitchFromGizmoMatrix normalises row 2 and clamps pitch to +-89.9 deg.
    void GizmoMatrixFromYawPitch(const float position[3], float yaw, float pitch, float out[16]);
    void YawPitchFromGizmoMatrix(const float m[16], float& yaw, float& pitch);

    // ImGuizmo transform gizmo on the selected object (ImGuizmo by Cedric Guillemet, MIT,
    // Libraries/ImGuizmo). Models take the chosen operation; point lights always translate; spot
    // lights translate or rotate (Scale falls back to Translate); the directional light is a rotate
    // gizmo 5 m in front of the camera. Cameras and an empty selection draw nothing.
    class EditorGizmo
    {
    public:
        // Call once per frame before the editor's click picking, outside BeginDisabled. A change bumps
        // selection.revision so the inspector reloads its cached TRS.
        void Draw(ApplicationCore& app, EditorSelection& selection, bool enabled);

        // True when this frame's left click went to the gizmo (picking must ignore it).
        bool ConsumedClick() const { return m_clickGrabbed; }
        bool IsUsing() const;

        EditorGizmoOperation Operation() const { return m_operation; }
        void SetOperation(EditorGizmoOperation operation) { m_operation = operation; }
        EditorGizmoSpace Space() const { return m_space; }
        void SetSpace(EditorGizmoSpace space) { m_space = space; }
        uint32_t EditCount() const { return m_editCount; }
        bool LastInputEnabled() const { return m_lastInputEnabled; }  // what Draw passed to ImGuizmo::Enable

    private:
        EditorGizmoOperation m_operation = EditorGizmoOperation::Translate;
        EditorGizmoSpace m_space = EditorGizmoSpace::Local;
        bool m_clickGrabbed = false;
        uint32_t m_editCount = 0;
        bool m_lastInputEnabled = false;
    };

    // Checks of BuildEditorViewMatrix and the yaw/pitch round trip; returns failures, `checks` = run.
    int RunEditorGizmoSelfTest(int& checks);
}
