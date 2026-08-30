#include "AppFramework/Debug/RemoteControl/DebugCommandRegistry.h"

#include <algorithm>

namespace SasamiRenderer::Debug
{
    namespace
    {
        std::vector<std::string> Tokenize(std::string_view line)
        {
            std::vector<std::string> tokens;
            std::size_t pos = 0;
            while (pos < line.size())
            {
                pos = line.find_first_not_of(" \t", pos);
                if (pos == std::string_view::npos)
                    break;

                std::size_t end = line.find_first_of(" \t", pos);
                if (end == std::string_view::npos)
                    end = line.size();

                tokens.emplace_back(line.substr(pos, end - pos));
                pos = end;
            }
            return tokens;
        }

        // Collapses a reply to a single line so it cannot break the
        // one-line-per-response protocol.
        void SanitizeLine(std::string& line)
        {
            std::replace_if(line.begin(), line.end(), [](char c) { return c == '\n' || c == '\r'; }, ' ');
        }
    }

    void DebugCommandRegistry::Register(std::string name, std::string help, DebugCommandHandler handler)
    {
        m_commands.insert_or_assign(std::move(name), Entry { std::move(help), std::move(handler) });
    }

    std::string DebugCommandRegistry::Execute(std::string_view requestLine) const
    {
        const std::vector<std::string> tokens = Tokenize(requestLine);
        if (tokens.empty())
            return "ERR empty command";

        const auto it = m_commands.find(tokens.front());
        if (it == m_commands.end())
            return "ERR unknown command: " + tokens.front();

        const std::vector<std::string> args(tokens.begin() + 1, tokens.end());

        std::string result;
        try
        {
            result = it->second.handler(args);
        }
        catch (const std::exception& e)
        {
            result = std::string("ERR ") + e.what();
        }
        catch (...)
        {
            result = "ERR unknown exception";
        }

        SanitizeLine(result);
        return result;
    }

    std::string DebugCommandRegistry::BuildHelpText() const
    {
        std::string text;
        bool first = true;
        for (const auto& [name, entry] : m_commands)
        {
            if (!first)
                text += " | ";
            first = false;

            text += name;
            text += " - ";
            text += entry.help;
        }

        SanitizeLine(text);
        return text;
    }
}
