#pragma once

namespace SasamiEngine::App::UI
{
    // "Gate" part: greys out the widgets in its scope and explains why on hover.
    //
    // Used for controls whose feature the running backend cannot do. The controls stay visible so
    // the panel keeps its shape and the reader can still see what the renderer offers -- only the
    // interaction is taken away, with the reason one hover click away.
    class Gate
    {
    public:
        // reason is shown as the tooltip while disabled; it is ignored when supported is true.
        Gate(bool supported, const char* reason);
        ~Gate();

        Gate(const Gate&)            = delete;
        Gate& operator=(const Gate&) = delete;

        // True when the scope is interactive, for callers that also want to skip work.
        explicit operator bool() const { return m_supported; }

    private:
        bool        m_supported = true;
        const char* m_reason    = nullptr;
    };
}
namespace SasamiEngine { using namespace App; }
