#pragma once

#include "Renderer/RHI/RhiTypes.h"
#include "Renderer/Passes/Core/IRenderPass.h"
#include "Renderer/Structures/RendererEnums.h"

#include <array>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace SasamiRenderer
{
    struct RenderPassBuildContext
    {
        RhiBackendCapabilities capabilities{};
    };

    enum class RenderPassBuilderId
    {
        Shadow,
        OpaqueGBuffer,
        Lighting,
        TransparentLighting,
        Skybox,
        PostProcess,
        RuntimeAO,
        RuntimeAOBlur,
        ProceduralSky,
        TransparentBackfaceDistance,
        TransparentSceneColorCopy,
        TransparentComposite,
        ScreenSpaceReflection,
        ScreenSpaceReflectionComposite,
        SoftwareReflection,
        SoftwareReflectionComposite,
        RayTracing,
        VolumetricCloud,
        DebugProbeGrid,
        FluidSurface,
        Particles,
    };

    class IRenderPassBuilder
    {
    public:
        virtual ~IRenderPassBuilder() = default;

        virtual RenderPassBuilderId Id() const = 0;
        virtual std::string_view DisplayName() const = 0;
        virtual bool IsAvailable(const RenderPassBuildContext& context) const;
        virtual std::shared_ptr<IRenderPass> Build(const RenderPassBuildContext& context) const = 0;
    };

    class RenderPassBuilderCatalog
    {
    public:
        using RenderPassType = RendererEnums::RenderPassType;
        static constexpr size_t kSequencePassCount =
            static_cast<size_t>(RendererEnums::RenderPassType::ScreenSpaceReflectionComposite) + 1u;

        static RenderPassBuilderCatalog CreateDefault();

        void Register(std::unique_ptr<IRenderPassBuilder> builder);
        const IRenderPassBuilder* Find(RenderPassBuilderId id) const;
        const std::vector<std::unique_ptr<IRenderPassBuilder>>& Builders() const { return m_builders; }

        std::array<std::shared_ptr<IRenderPass>, kSequencePassCount>
            BuildBuiltinSequencePasses(const RenderPassBuildContext& context) const;
        std::shared_ptr<IRenderPass> Build(RenderPassBuilderId id, const RenderPassBuildContext& context) const;

    private:
        // RenderPassType has retired gaps (values with no corresponding builder), so there is no
        // valid RenderPassBuilderId for every RenderPassType. This used to be papered over by a
        // `default:` case that silently returned Shadow, which stuffed a spurious ShadowRenderPass
        // into the retired slots and relied on the caller overwriting them with nullptr afterward.
        static std::optional<RenderPassBuilderId> ToBuilderId(RenderPassType type);

        std::vector<std::unique_ptr<IRenderPassBuilder>> m_builders;
    };
}
