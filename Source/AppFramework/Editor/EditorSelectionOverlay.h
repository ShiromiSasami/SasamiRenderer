#pragma once

#include <cstdint>

namespace SasamiEngine::App
{
    class ApplicationCore;
    struct EditorSelection;

    // Outlines the selection in the 3D view: the transformed local bounds of a model as 12 lines,
    // a circle for a point/spot light. Drawn into ImGui's background draw list, so it is not depth
    // tested and hides with the rest of ImGui. Local bounds are cached per selection revision; the
    // model matrix is read every frame so the box follows Inspector edits.
    class EditorSelectionOverlay
    {
    public:
        void Draw(ApplicationCore& app, const EditorSelection& selection);

    private:
        uint32_t m_cachedRevision = UINT32_MAX;
        bool m_hasBox = false;
        float m_boxMin[3] = {};
        float m_boxMax[3] = {};
    };
}
