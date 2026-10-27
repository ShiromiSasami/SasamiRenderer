#pragma once

namespace SasamiEngine::App
{
    class ApplicationCore;

    // Editor viewport modes after Unreal's View Mode menu ("Viewport Modes in Unreal Engine"):
    // Lit = the final image, Unlit = base colour only, Wireframe = polygon edges. Unlit reuses the
    // GBuffer Albedo debug view; Wireframe draws the edges over the lit image
    // (WireframeOverlayRenderPass, DX12 only), unlike Unreal's edges-only view.
    enum class EditorViewMode
    {
        Lit,
        Unlit,
        Wireframe,
    };

    const char* EditorViewModeName(EditorViewMode mode);
    bool ParseEditorViewMode(const char* text, EditorViewMode& mode);

    // Applies the mode to the renderer; the mode is read back from the renderer state, so a debug
    // view changed elsewhere (e.g. `render.gbuffer`) shows up here as well.
    void ApplyEditorViewMode(ApplicationCore& app, EditorViewMode mode);
    EditorViewMode CurrentEditorViewMode(ApplicationCore& app);

    // Name/parse round trip; returns the number of failures and adds to `checks`.
    int RunEditorViewModeSelfTest(int& checks);
}
