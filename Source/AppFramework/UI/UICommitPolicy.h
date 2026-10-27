#pragma once

namespace SasamiEngine::App::UI
{
    // When a UI part reports an edit back to the caller.
    //
    // The parts all default to Deferred: they hold the in-progress value in the EditBuffer and
    // only return true once the edit finishes, so a caller that rebuilds expensive state on every
    // change does not do it sixty times a second while a slider is being dragged.
    //
    // Realtime is for controls whose effect should be visible while the mouse is still down --
    // light direction and intensity above all, where the whole point of dragging is watching the
    // scene follow.
    enum class CommitPolicy
    {
        Deferred,
        Realtime,
    };

    // The policy the UI parts built right now should follow.
    CommitPolicy CurrentCommitPolicy();

    // Applies a policy to every UI part built inside the scope. Restores the previous one on the
    // way out, so scopes nest.
    //
    //   if (UI::Tab lightingTab{"Lighting"}) {
    //       UI::CommitScope realtime{UI::CommitPolicy::Realtime};
    //       ...
    //   }
    class CommitScope
    {
    public:
        explicit CommitScope(CommitPolicy policy);
        ~CommitScope();

        CommitScope(const CommitScope&) = delete;
        CommitScope& operator=(const CommitScope&) = delete;

    private:
        CommitPolicy m_previous;
    };
}

// Same reason as the Foundation headers: the directive sits on SasamiEngine so that anything nested
// under it finds these names by enclosing-namespace lookup, without each layer qualifying App::.
namespace SasamiEngine { using namespace App; }
