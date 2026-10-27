#include "UICommitPolicy.h"

namespace SasamiEngine::App::UI
{
    namespace
    {
        // A single variable is enough for the same reason EditBuffer gets away with one slot: the
        // UI is built from the main thread only, one part at a time.
        CommitPolicy g_currentPolicy = CommitPolicy::Deferred;
    }

    CommitPolicy CurrentCommitPolicy()
    {
        return g_currentPolicy;
    }

    CommitScope::CommitScope(CommitPolicy policy)
        : m_previous(g_currentPolicy)
    {
        g_currentPolicy = policy;
    }

    CommitScope::~CommitScope()
    {
        g_currentPolicy = m_previous;
    }
}
