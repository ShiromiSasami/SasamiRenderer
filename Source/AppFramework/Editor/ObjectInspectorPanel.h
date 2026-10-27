#pragma once

#include "AppFramework/Editor/EditorSelection.h"
#include "AppFramework/Editor/EditorTransformMath.h"

#include <cstdint>

namespace SasamiEngine::App
{
    class ApplicationCore;

    // "Details" window (UE's name for Unity's Inspector): edits the selected object in place (transform, light colour / intensity /
    // range / cone, directional light angles, camera target / orbit / clip planes).
    class ObjectInspectorPanel
    {
    public:
        static constexpr const char* WINDOW_NAME = "Details";

        void Draw(ApplicationCore& app, const EditorSelection& selection, bool* open);

    private:
        void DrawModel(ApplicationCore& app, const EditorSelection& selection);
        void DrawPointLight(const EditorSelection& selection);
        void DrawSpotLight(const EditorSelection& selection);
        void DrawDirectionalLight(ApplicationCore& app);
        void DrawCamera(const EditorSelection& selection);

        // Euler angles are cached per selection: decomposing every frame would make the sliders jump
        // between equivalent angle triples.
        uint32_t m_cachedRevision = UINT32_MAX;
        EditorTrs m_cachedTrs{};
    };
}
