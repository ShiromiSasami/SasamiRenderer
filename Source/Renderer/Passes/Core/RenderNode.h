#pragma once

#include "Renderer/Passes/Core/IRenderPass.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SasamiRenderer
{
    class IRenderNode
    {
    public:
        virtual ~IRenderNode() = default;

        virtual std::string_view Tag() const = 0;
        virtual void AppendPasses(std::vector<std::shared_ptr<IRenderPass>>& outPasses) const = 0;
    };

    class RenderPassSequenceNode final : public IRenderNode
    {
    public:
        RenderPassSequenceNode(std::string_view tag,
                               std::vector<std::shared_ptr<IRenderPass>> passes)
            : m_passes(std::move(passes))
        {
            m_tag.assign(tag.begin(), tag.end());
        }

        std::string_view Tag() const override { return m_tag; }

        void AppendPasses(std::vector<std::shared_ptr<IRenderPass>>& outPasses) const override
        {
            for (const auto& pass : m_passes) {
                if (pass) {
                    outPasses.push_back(pass);
                }
            }
        }

    private:
        std::string m_tag;
        std::vector<std::shared_ptr<IRenderPass>> m_passes;
    };

}
