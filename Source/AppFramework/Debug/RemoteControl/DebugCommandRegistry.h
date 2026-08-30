#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace SasamiRenderer::Debug
{
    // Handles the arguments of a single command (command name already stripped)
    // and returns the line to send back.
    using DebugCommandHandler = std::function<std::string(const std::vector<std::string>& args)>;

    // Maps command names to handlers and dispatches parsed request lines to them.
    class DebugCommandRegistry
    {
    public:
        // Registers a command. Re-registering an existing name overwrites it.
        void Register(std::string name, std::string help, DebugCommandHandler handler);

        // Parses a line such as "camera.setTarget 1 2 3", dispatches it to the
        // matching handler, and returns the reply line. Unknown commands and
        // empty lines return an error string; handler exceptions are caught.
        std::string Execute(std::string_view requestLine) const;

        // A single-line, name-sorted listing of registered commands and their help text.
        std::string BuildHelpText() const;

    private:
        struct Entry
        {
            std::string help;
            DebugCommandHandler handler;
        };

        std::map<std::string, Entry, std::less<>> m_commands;
    };
}
