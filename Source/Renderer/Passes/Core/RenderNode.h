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

    class RenderPassNode final : public IRenderNode
    {
    public:
        explicit RenderPassNode(std::shared_ptr<IRenderPass> pass,
                                std::string_view tag = {})
            : m_pass(std::move(pass))
        {
            if (!tag.empty()) {
                m_tag.assign(tag.begin(), tag.end());
            } else if (m_pass) {
                const std::string_view passTag = m_pass->Tag();
                m_tag.assign(passTag.begin(), passTag.end());
            }
        }

        std::string_view Tag() const override { return m_tag; }

        void AppendPasses(std::vector<std::shared_ptr<IRenderPass>>& outPasses) const override
        {
            if (m_pass) {
                outPasses.push_back(m_pass);
            }
        }

    private:
        std::string m_tag;
        std::shared_ptr<IRenderPass> m_pass;
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

    class CompositeRenderNode final : public IRenderNode
    {
    public:
        CompositeRenderNode(std::string_view tag,
                            std::vector<std::shared_ptr<IRenderNode>> nodes)
            : m_nodes(std::move(nodes))
        {
            m_tag.assign(tag.begin(), tag.end());
        }

        std::string_view Tag() const override { return m_tag; }

        void AppendPasses(std::vector<std::shared_ptr<IRenderPass>>& outPasses) const override
        {
            for (const auto& node : m_nodes) {
                if (node) {
                    node->AppendPasses(outPasses);
                }
            }
        }

    private:
        std::string m_tag;
        std::vector<std::shared_ptr<IRenderNode>> m_nodes;
    };
}
