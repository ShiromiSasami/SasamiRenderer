#include "AppFramework/Editor/EditorPlaySession.h"

namespace SasamiEngine::App
{
    bool EditorPlaySession::Play(const ApplicationCore& app)
    {
        if (m_playing)
        {
            return false;
        }
        m_snapshot.Capture(app);
        m_playing = true;
        return true;
    }

    bool EditorPlaySession::Stop(ApplicationCore& app, EditorSceneRestoreReport* outReport)
    {
        if (!m_playing)
        {
            return false;
        }
        const EditorSceneRestoreReport report = m_snapshot.Restore(app);
        m_snapshot.Clear();
        m_playing = false;
        if (outReport != nullptr)
        {
            *outReport = report;
        }
        return true;
    }
}
