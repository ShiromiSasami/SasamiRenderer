#pragma once

#include <functional>
#include <memory>

namespace SasamiRenderer
{
    class DebugProbeGridRenderPass;
    class IRHIDevice;
    class IrradianceProbeGrid;

    // Owns the debug probe-grid visualization pass and its enable/radius state,
    // mirroring the "System" ownership pattern used by LightSystem.
    class DebugVisualizationSystem
    {
    public:
        using EnsureInsertedCallback = std::function<void()>;

        void Bind(std::shared_ptr<DebugProbeGridRenderPass> pass, EnsureInsertedCallback onEnable);
        bool Initialize(IRHIDevice& device, const IrradianceProbeGrid& probeGrid);

        bool IsEnabled() const;
        void SetEnabled(bool enabled);
        float GetProbeRadius() const;
        void SetProbeRadius(float radius);

        std::shared_ptr<DebugProbeGridRenderPass> GetPass() const { return m_pass; }

    private:
        std::shared_ptr<DebugProbeGridRenderPass> m_pass;
        EnsureInsertedCallback m_onEnable;
    };
}
