#include "Renderer/Config/BackendFeatureSupport.h"

#include "Foundation/Tools/DebugOutput.h"

#include <iterator>
#include <string>

namespace SasamiEngine::Render
{
    namespace
    {
        constexpr const char* kDirectX12Only = "DirectX 12 only";
        constexpr const char* kNeedsDxr      = "requires DirectX Raytracing Tier 1.0";
        constexpr const char* kNeedsMesh     = "requires Mesh Shader Tier 1";

        // Everything the renderer draws beyond a plain mesh pass is written against the D3D12
        // compatibility surface (GraphicsDevice.h), which the other backends implement as
        // E_NOTIMPL. They short-circuit to ExecuteBackendFrame, which carries a clear colour, one
        // mesh draw and one ray-march desc -- so on those backends every feature below is off.
        constexpr const char* kFeatureNames[] = { "RenderGraph", "VirtualGeometry", "Tessellation",
            "HardwareRayTracing", "SoftwareRayTracing", "ScreenSpaceReflections", "AmbientOcclusion",
            "GlobalIllumination", "Particles", "PostProcess", "GpuTimestamps" };
        static_assert(std::size(kFeatureNames) == static_cast<size_t>(RenderFeature::Count),
                      "kFeatureNames must stay in step with RenderFeature");

        bool IsDirectX12(RhiBackendApi api) { return api == RhiBackendApi::DirectX12; }
    }

    void BackendFeatureSupport::Resolve(const RhiBackendCapabilities& capabilities)
    {
        const bool dx12 = IsDirectX12(capabilities.api);

        for (size_t i = 0; i < kCount; ++i) {
            m_supported[i] = dx12;
            m_reason[i]    = dx12 ? "" : kDirectX12Only;
        }

        if (!dx12) {
            LogResolved();
            return;
        }

        // Within DX12 the differences are per-GPU, so defer to what the device reported.
        const auto set = [this](RenderFeature feature, bool supported, const char* reason) {
            const size_t index = static_cast<size_t>(feature);
            m_supported[index] = supported;
            m_reason[index]    = supported ? "" : reason;
        };

        set(RenderFeature::HardwareRayTracing, capabilities.supportsHardwareRayTracing, kNeedsDxr);
        set(RenderFeature::VirtualGeometry, capabilities.supportsMeshShaders, kNeedsMesh);

        LogResolved();
    }

    // One line naming what is off and why, so a backend build shows its limits in the log
    // without needing the UI.
    void BackendFeatureSupport::LogResolved() const
    {
        std::string line = "BackendFeatureSupport: unsupported =";
        bool any = false;
        for (size_t i = 0; i < kCount; ++i) {
            if (m_supported[i]) {
                continue;
            }
            any = true;
            line += " ";
            line += kFeatureNames[i];
            line += "(";
            line += m_reason[i] != nullptr ? m_reason[i] : "";
            line += ")";
        }
        if (!any) {
            line += " none";
        }
        line += "\n";
        DebugLog(line.c_str());
    }

    bool BackendFeatureSupport::IsSupported(RenderFeature feature) const
    {
        const size_t index = static_cast<size_t>(feature);
        return index < kCount && m_supported[index];
    }

    const char* BackendFeatureSupport::UnsupportedReason(RenderFeature feature) const
    {
        const size_t index = static_cast<size_t>(feature);
        if (index >= kCount || m_reason[index] == nullptr) {
            return "";
        }
        return m_reason[index];
    }
}
