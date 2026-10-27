#include "AppFramework/Editor/EditorViewMode.h"

#include "AppFramework/ApplicationCore.h"

#include <cstring>

namespace SasamiEngine::App
{
    namespace
    {
        using GBufferDebugView = Render::RendererEnums::GBufferDebugView;
    }

    const char* EditorViewModeName(EditorViewMode mode)
    {
        switch (mode)
        {
        case EditorViewMode::Lit:       return "lit";
        case EditorViewMode::Unlit:     return "unlit";
        case EditorViewMode::Wireframe: return "wireframe";
        }
        return "lit";
    }

    bool ParseEditorViewMode(const char* text, EditorViewMode& mode)
    {
        for (EditorViewMode candidate : { EditorViewMode::Lit, EditorViewMode::Unlit, EditorViewMode::Wireframe })
        {
            if (std::strcmp(text, EditorViewModeName(candidate)) == 0)
            {
                mode = candidate;
                return true;
            }
        }
        return false;
    }

    void ApplyEditorViewMode(ApplicationCore& app, EditorViewMode mode)
    {
        const GBufferDebugView view = mode == EditorViewMode::Unlit ? GBufferDebugView::Albedo : GBufferDebugView::FinalLit;
        app.SetGBufferDebugViewIndex(static_cast<int>(view));
        app.GetRenderer().SetWireframeOverlayEnabled(mode == EditorViewMode::Wireframe);
    }

    EditorViewMode CurrentEditorViewMode(ApplicationCore& app)
    {
        if (app.GetRenderer().GetWireframeOverlayEnabled())
        {
            return EditorViewMode::Wireframe;
        }
        if (app.GetGBufferDebugViewIndex() == static_cast<int>(GBufferDebugView::Albedo))
        {
            return EditorViewMode::Unlit;
        }
        return EditorViewMode::Lit;
    }

    int RunEditorViewModeSelfTest(int& checks)
    {
        int failures = 0;
        for (EditorViewMode mode : { EditorViewMode::Lit, EditorViewMode::Unlit, EditorViewMode::Wireframe })
        {
            EditorViewMode parsed = EditorViewMode::Lit;
            ++checks;
            if (!ParseEditorViewMode(EditorViewModeName(mode), parsed) || parsed != mode)
            {
                ++failures;
            }
        }
        EditorViewMode untouched = EditorViewMode::Unlit;
        ++checks;
        if (ParseEditorViewMode("shaded", untouched) || untouched != EditorViewMode::Unlit)
        {
            ++failures;
        }
        return failures;
    }
}
