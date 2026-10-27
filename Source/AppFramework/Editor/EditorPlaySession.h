#pragma once

#include "AppFramework/Editor/EditorSceneSnapshot.h"

namespace SasamiEngine::App
{
    class ApplicationCore;

    // Play / Stop of the editor (a minimal "play in editor"). Play captures the editable scene state
    // (EditorSceneSnapshot); while playing the editor panels are read-only and picking is off, and the
    // application keeps updating as usual. Stop writes the captured state back. There is no separate
    // game world: whatever the app changes outside the snapshot (particles, animation time, objects
    // created while playing) stays.
    class EditorPlaySession
    {
    public:
        // False when already playing.
        bool Play(const ApplicationCore& app);
        // False when not playing; `outReport` is filled on success.
        bool Stop(ApplicationCore& app, EditorSceneRestoreReport* outReport = nullptr);
        bool IsPlaying() const { return m_playing; }

    private:
        EditorSceneSnapshot m_snapshot;
        bool m_playing = false;
    };
}
