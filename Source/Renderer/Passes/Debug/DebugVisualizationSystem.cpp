#include "Renderer/Passes/Debug/DebugVisualizationSystem.h"

#include "Renderer/Passes/Debug/DebugProbeGridRenderPass.h"

namespace SasamiRenderer
{
    void DebugVisualizationSystem::Bind(std::shared_ptr<DebugProbeGridRenderPass> pass, EnsureInsertedCallback onEnable)
    {
        m_pass = std::move(pass);
        m_onEnable = std::move(onEnable);
    }

    bool DebugVisualizationSystem::Initialize(IRHIDevice& device, const IrradianceProbeGrid& probeGrid)
    {
        if (!m_pass) {
            return false;
        }
        if (!m_pass->Initialize(device)) {
            return false;
        }
        m_pass->SetProbeGrid(&probeGrid);
        return true;
    }

    bool DebugVisualizationSystem::IsEnabled() const
    {
        return m_pass ? m_pass->IsEnabled() : false;
    }

    void DebugVisualizationSystem::SetEnabled(bool enabled)
    {
        if (!m_pass) {
            return;
        }
        m_pass->SetEnabled(enabled);
        if (enabled && m_onEnable) {
            m_onEnable();
        }
    }

    float DebugVisualizationSystem::GetProbeRadius() const
    {
        return m_pass ? m_pass->GetProbeRadius() : 0.2f;
    }

    void DebugVisualizationSystem::SetProbeRadius(float radius)
    {
        if (m_pass) {
            m_pass->SetProbeRadius(radius);
        }
    }
}
