#pragma once

#include "Renderer/RHI/RhiTypes.h"

#include <array>
#include <cstdint>

namespace SasamiEngine::Render
{
    // A renderer feature as the UI thinks about it: one entry per group of controls that becomes
    // meaningless when the backend cannot run it.
    enum class RenderFeature : uint32_t
    {
        RenderGraph,             // the whole DX12 render-graph frame; without it nothing below runs
        VirtualGeometry,         // amplification/mesh shader cluster path
        Tessellation,
        HardwareRayTracing,      // DXR
        SoftwareRayTracing,      // SWRT shadows / reflections / ReSTIR
        ScreenSpaceReflections,
        AmbientOcclusion,        // SSAO and RTAO
        GlobalIllumination,      // probe grid / DDGI
        Particles,
        PostProcess,
        GpuTimestamps,
        Count
    };

    // Which features the running backend can actually do.
    //
    // Two things decide that, and both are needed: a static per-backend preset (is the feature
    // implemented for this backend at all) and the live device capabilities (does this particular
    // GPU expose the tier it needs). Resolve() combines them once, after the device exists, and
    // everything else — the UI above all — reads the answer from here rather than re-deriving it.
    class BackendFeatureSupport
    {
    public:
        void Resolve(const RhiBackendCapabilities& capabilities);

        bool IsSupported(RenderFeature feature) const;

        // Short phrase for the UI tooltip, e.g. "DirectX 12 only". Empty when supported.
        const char* UnsupportedReason(RenderFeature feature) const;

    private:
        void LogResolved() const;

        static constexpr size_t kCount = static_cast<size_t>(RenderFeature::Count);

        std::array<bool, kCount>        m_supported{};
        std::array<const char*, kCount> m_reason{};
    };
}
namespace SasamiEngine { using namespace Render; }
