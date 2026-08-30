// ApplicationCoreRenderPasses.cpp
// ApplicationCore render pass / render node graph management.
#include "ApplicationCore.h"

#include "Renderer/Runtime/Renderer.h"

namespace SasamiRenderer
{
    std::vector<ApplicationCore::RenderPassType> ApplicationCore::GetRenderPassSequence() const
    {
        if (!m_renderer) {
            return {};
        }
        return m_renderer->GetRenderPassSequence();
    }

    void ApplicationCore::SetRenderPassSequence(const std::vector<RenderPassType>& sequence)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetRenderPassSequence(sequence);
    }

    bool ApplicationCore::AddRenderNode(const std::shared_ptr<IRenderNode>& renderNode)
    {
        if (!m_renderer) {
            return false;
        }
        return m_renderer->AddNode(renderNode).IsValid();
    }

    void ApplicationCore::SetRenderNodePreset(const std::shared_ptr<IRenderNode>& renderNode)
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->SetRenderNodePreset(renderNode);
    }

    void ApplicationCore::UseDefaultRenderNodePreset()
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->UseDefaultRenderNodePreset();
    }

    bool ApplicationCore::AddRenderPass(const std::shared_ptr<IRenderPass>& renderPass)
    {
        if (!m_renderer) {
            return false;
        }
        return m_renderer->AddPass(renderPass).IsValid();
    }

    bool ApplicationCore::AddRenderPassBefore(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass)
    {
        if (!m_renderer) {
            return false;
        }
        return m_renderer->AddPassBefore(targetTag, renderPass).IsValid();
    }

    bool ApplicationCore::AddRenderPassAfter(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass)
    {
        if (!m_renderer) {
            return false;
        }
        return m_renderer->AddPassAfter(targetTag, renderPass).IsValid();
    }

    bool ApplicationCore::ReplaceRenderPass(std::string_view targetTag, const std::shared_ptr<IRenderPass>& renderPass)
    {
        if (!m_renderer) {
            return false;
        }
        return m_renderer->ReplacePass(targetTag, renderPass);
    }

    void ApplicationCore::ClearRenderPasses()
    {
        if (!m_renderer) {
            return;
        }
        m_renderer->ClearPasses();
    }

} // namespace SasamiRenderer
