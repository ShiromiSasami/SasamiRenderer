#include "Renderer/Passes/Core/RenderPassBuilder.h"

#include "Renderer/Passes/Debug/DebugProbeGridRenderPass.h"
#include "Renderer/Passes/Fluid/FluidSurfaceRenderPass.h"
#include "Renderer/Passes/Lighting/DeferredLightingRenderPass.h"
#include "Renderer/Passes/Geometry/GBufferRenderPass.h"
#include "Renderer/Passes/Particles/ParticleRenderPass.h"
#include "Renderer/Passes/PostProcess/PostProcessRenderPass.h"
#include "Renderer/Passes/Sky/ProceduralSkyRenderPass.h"
#include "Renderer/Passes/RayTracing/RayTracingRenderPass.h"
#include "Renderer/Passes/Geometry/ShadowRenderPass.h"
#include "Renderer/Passes/Sky/SkyboxRenderPass.h"
#include "Renderer/Passes/Reflections/ScreenSpaceReflectionRenderPass.h"
#include "Renderer/Passes/Reflections/ScreenSpaceReflectionCompositeRenderPass.h"
#include "Renderer/Passes/Reflections/SoftwareReflectionCompositeRenderPass.h"
#include "Renderer/Passes/Reflections/SoftwareReflectionRenderPass.h"
#include "Renderer/Passes/Lighting/SSAORenderPass.h"
#include "Renderer/Passes/Transparency/TransparentBackfaceDistanceRenderPass.h"
#include "Renderer/Passes/Transparency/TransparentCompositeRenderPass.h"
#include "Renderer/Passes/Transparency/TransparentLightingRenderPass.h"
#include "Renderer/Passes/Transparency/TransparentSceneColorCopyRenderPass.h"
#include "Renderer/Passes/Sky/VolumetricCloudRenderPass.h"

namespace SasamiRenderer
{
    bool IRenderPassBuilder::IsAvailable(const RenderPassBuildContext& context) const
    {
        return context.capabilities.supportsFeatureRenderPasses;
    }

    namespace
    {
        template<typename TPass>
        class StatelessRenderPassBuilder : public IRenderPassBuilder
        {
        public:
            StatelessRenderPassBuilder(RenderPassBuilderId id, std::string_view displayName)
                : m_id(id)
                , m_displayName(displayName)
            {
            }

            RenderPassBuilderId Id() const override { return m_id; }
            std::string_view DisplayName() const override { return m_displayName; }

            std::shared_ptr<IRenderPass> Build(const RenderPassBuildContext& context) const override
            {
                if (!IsAvailable(context)) {
                    return nullptr;
                }
                return std::make_shared<TPass>();
            }

        private:
            RenderPassBuilderId m_id;
            std::string_view m_displayName;
        };

        class HardwareRayTracingPassBuilder final : public StatelessRenderPassBuilder<RayTracingRenderPass>
        {
        public:
            HardwareRayTracingPassBuilder()
                : StatelessRenderPassBuilder<RayTracingRenderPass>(RenderPassBuilderId::RayTracing,
                                                                    "Hardware Ray Tracing")
            {
            }

            bool IsAvailable(const RenderPassBuildContext& context) const override
            {
                return StatelessRenderPassBuilder<RayTracingRenderPass>::IsAvailable(context) &&
                       context.capabilities.supportsHardwareRayTracing;
            }
        };
    }

    RenderPassBuilderCatalog RenderPassBuilderCatalog::CreateDefault()
    {
        RenderPassBuilderCatalog catalog;
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<ShadowRenderPass>>(
            RenderPassBuilderId::Shadow, "Shadow"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<GBufferRenderPass>>(
            RenderPassBuilderId::OpaqueGBuffer, "Opaque GBuffer"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<DeferredLightingRenderPass>>(
            RenderPassBuilderId::Lighting, "Lighting"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<TransparentLightingRenderPass>>(
            RenderPassBuilderId::TransparentLighting, "Transparent Lighting"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<SkyboxRenderPass>>(
            RenderPassBuilderId::Skybox, "Skybox"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<PostProcessRenderPass>>(
            RenderPassBuilderId::PostProcess, "Post Process"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<SSAORenderPass>>(
            RenderPassBuilderId::RuntimeAO, "Runtime AO"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<SSAOBlurRenderPass>>(
            RenderPassBuilderId::RuntimeAOBlur, "Runtime AO Blur"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<ProceduralSkyRenderPass>>(
            RenderPassBuilderId::ProceduralSky, "Procedural Sky"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<TransparentBackfaceDistanceRenderPass>>(
            RenderPassBuilderId::TransparentBackfaceDistance, "Transparent Backface Distance"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<TransparentSceneColorCopyRenderPass>>(
            RenderPassBuilderId::TransparentSceneColorCopy, "Transparent Scene Color Copy"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<TransparentCompositeRenderPass>>(
            RenderPassBuilderId::TransparentComposite, "Transparent Composite"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<ScreenSpaceReflectionRenderPass>>(
            RenderPassBuilderId::ScreenSpaceReflection, "Screen Space Reflection"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<ScreenSpaceReflectionCompositeRenderPass>>(
            RenderPassBuilderId::ScreenSpaceReflectionComposite, "Screen Space Reflection Composite"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<SoftwareReflectionRenderPass>>(
            RenderPassBuilderId::SoftwareReflection, "Software Reflection"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<SoftwareReflectionCompositeRenderPass>>(
            RenderPassBuilderId::SoftwareReflectionComposite, "Software Reflection Composite"));
        catalog.Register(std::make_unique<HardwareRayTracingPassBuilder>());
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<VolumetricCloudRenderPass>>(
            RenderPassBuilderId::VolumetricCloud, "Volumetric Cloud"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<DebugProbeGridRenderPass>>(
            RenderPassBuilderId::DebugProbeGrid, "Debug Probe Grid"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<FluidSurfaceRenderPass>>(
            RenderPassBuilderId::FluidSurface, "Fluid Surface"));
        catalog.Register(std::make_unique<StatelessRenderPassBuilder<ParticleRenderPass>>(
            RenderPassBuilderId::Particles, "Particles"));
        return catalog;
    }

    void RenderPassBuilderCatalog::Register(std::unique_ptr<IRenderPassBuilder> builder)
    {
        if (builder) {
            m_builders.push_back(std::move(builder));
        }
    }

    const IRenderPassBuilder* RenderPassBuilderCatalog::Find(RenderPassBuilderId id) const
    {
        for (const auto& builder : m_builders) {
            if (builder && builder->Id() == id) {
                return builder.get();
            }
        }
        return nullptr;
    }

    std::shared_ptr<IRenderPass> RenderPassBuilderCatalog::Build(RenderPassBuilderId id,
                                                                 const RenderPassBuildContext& context) const
    {
        const IRenderPassBuilder* builder = Find(id);
        return builder ? builder->Build(context) : nullptr;
    }

    std::array<std::shared_ptr<IRenderPass>, RenderPassBuilderCatalog::kSequencePassCount>
    RenderPassBuilderCatalog::BuildBuiltinSequencePasses(const RenderPassBuildContext& context) const
    {
        std::array<std::shared_ptr<IRenderPass>, kSequencePassCount> passes{};
        for (size_t i = 0; i < passes.size(); ++i) {
            const auto passType = static_cast<RenderPassType>(i);
            if (const auto builderId = ToBuilderId(passType)) {
                passes[i] = Build(*builderId, context);
            }
            // nullopt = retired slot with no builder; leave passes[i] as the value-initialized nullptr.
        }
        return passes;
    }

    std::optional<RenderPassBuilderId> RenderPassBuilderCatalog::ToBuilderId(RenderPassType type)
    {
        switch (type) {
        case RenderPassType::Shadow: return RenderPassBuilderId::Shadow;
        case RenderPassType::OpaqueGBuffer: return RenderPassBuilderId::OpaqueGBuffer;
        case RenderPassType::Lighting: return RenderPassBuilderId::Lighting;
        case RenderPassType::TransparentLighting: return RenderPassBuilderId::TransparentLighting;
        case RenderPassType::Skybox: return RenderPassBuilderId::Skybox;
        case RenderPassType::PostProcess: return RenderPassBuilderId::PostProcess;
        case RenderPassType::RuntimeAO: return RenderPassBuilderId::RuntimeAO;
        case RenderPassType::RuntimeAOBlur: return RenderPassBuilderId::RuntimeAOBlur;
        case RenderPassType::ProceduralSky: return RenderPassBuilderId::ProceduralSky;
        case RenderPassType::TransparentBackfaceDistance: return RenderPassBuilderId::TransparentBackfaceDistance;
        case RenderPassType::TransparentSceneColorCopy: return RenderPassBuilderId::TransparentSceneColorCopy;
        case RenderPassType::TransparentComposite: return RenderPassBuilderId::TransparentComposite;
        case RenderPassType::ScreenSpaceReflection: return RenderPassBuilderId::ScreenSpaceReflection;
        case RenderPassType::ScreenSpaceReflectionComposite: return RenderPassBuilderId::ScreenSpaceReflectionComposite;
        case RenderPassType::SoftwareReflection: return RenderPassBuilderId::SoftwareReflection;
        case RenderPassType::SoftwareReflectionComposite: return RenderPassBuilderId::SoftwareReflectionComposite;
        default: return std::nullopt;
        }
    }
}
