#include "Renderer/Core/RenderNodeBuilder.h"

#include "Renderer/Passes/DebugProbeGridRenderNode.h"
#include "Renderer/Passes/LightingRenderNode.h"
#include "Renderer/Passes/OpaqueRenderNode.h"
#include "Renderer/Passes/PostProcessRenderNode.h"
#include "Renderer/Passes/ProceduralSkyRenderNode.h"
#include "Renderer/Passes/RayTracingRenderNode.h"
#include "Renderer/Passes/SdfFluidRenderNode.h"
#include "Renderer/Passes/ShadowRenderNode.h"
#include "Renderer/Passes/SkyboxRenderNode.h"
#include "Renderer/Passes/SSAORenderNode.h"
#include "Renderer/Passes/TransparentBackfaceDistanceRenderNode.h"
#include "Renderer/Passes/TransparentCompositeRenderNode.h"
#include "Renderer/Passes/TransparentLightingRenderNode.h"
#include "Renderer/Passes/TransparentRenderNode.h"
#include "Renderer/Passes/VolumetricCloudRenderNode.h"

namespace SasamiRenderer
{
    bool IRenderNodeBuilder::IsAvailable(const RenderNodeBuildContext& context) const
    {
        return context.capabilities.supportsFeatureRenderPasses;
    }

    namespace
    {
        template<typename TNode>
        class StatelessRenderNodeBuilder : public IRenderNodeBuilder
        {
        public:
            StatelessRenderNodeBuilder(RenderNodeBuilderId id, std::string_view displayName)
                : m_id(id)
                , m_displayName(displayName)
            {
            }

            RenderNodeBuilderId Id() const override { return m_id; }
            std::string_view DisplayName() const override { return m_displayName; }

            std::shared_ptr<IRenderNode> Build(const RenderNodeBuildContext& context) const override
            {
                if (!IsAvailable(context)) {
                    return nullptr;
                }
                return std::make_shared<TNode>();
            }

        private:
            RenderNodeBuilderId m_id;
            std::string_view m_displayName;
        };

        class HardwareRayTracingNodeBuilder final : public StatelessRenderNodeBuilder<RayTracingRenderNode>
        {
        public:
            HardwareRayTracingNodeBuilder()
                : StatelessRenderNodeBuilder<RayTracingRenderNode>(RenderNodeBuilderId::RayTracing,
                                                                    "Hardware Ray Tracing")
            {
            }

            bool IsAvailable(const RenderNodeBuildContext& context) const override
            {
                return StatelessRenderNodeBuilder<RayTracingRenderNode>::IsAvailable(context) &&
                       context.capabilities.supportsHardwareRayTracing;
            }
        };
    }

    RenderNodeBuilderCatalog RenderNodeBuilderCatalog::CreateDefault()
    {
        RenderNodeBuilderCatalog catalog;
        catalog.Register(std::make_unique<StatelessRenderNodeBuilder<ShadowRenderNode>>(
            RenderNodeBuilderId::Shadow, "Shadow"));
        catalog.Register(std::make_unique<StatelessRenderNodeBuilder<OpaqueRenderNode>>(
            RenderNodeBuilderId::Opaque, "Opaque"));
        catalog.Register(std::make_unique<StatelessRenderNodeBuilder<LightingRenderNode>>(
            RenderNodeBuilderId::Lighting, "Lighting"));
        catalog.Register(std::make_unique<StatelessRenderNodeBuilder<TransparentRenderNode>>(
            RenderNodeBuilderId::Transparent, "Transparent"));
        catalog.Register(std::make_unique<StatelessRenderNodeBuilder<TransparentLightingRenderNode>>(
            RenderNodeBuilderId::TransparentLighting, "Transparent Lighting"));
        catalog.Register(std::make_unique<StatelessRenderNodeBuilder<SkyboxRenderNode>>(
            RenderNodeBuilderId::Skybox, "Skybox"));
        catalog.Register(std::make_unique<StatelessRenderNodeBuilder<PostProcessRenderNode>>(
            RenderNodeBuilderId::PostProcess, "Post Process"));
        catalog.Register(std::make_unique<StatelessRenderNodeBuilder<SSAORenderNode>>(
            RenderNodeBuilderId::RuntimeAO, "Runtime AO"));
        catalog.Register(std::make_unique<StatelessRenderNodeBuilder<ProceduralSkyRenderNode>>(
            RenderNodeBuilderId::ProceduralSky, "Procedural Sky"));
        catalog.Register(std::make_unique<StatelessRenderNodeBuilder<SdfFluidRenderNode>>(
            RenderNodeBuilderId::SdfFluid, "SDF Fluid"));
        catalog.Register(std::make_unique<StatelessRenderNodeBuilder<TransparentBackfaceDistanceRenderNode>>(
            RenderNodeBuilderId::TransparentBackfaceDistance, "Transparent Backface Distance"));
        catalog.Register(std::make_unique<StatelessRenderNodeBuilder<TransparentCompositeRenderNode>>(
            RenderNodeBuilderId::TransparentComposite, "Transparent Composite"));
        catalog.Register(std::make_unique<HardwareRayTracingNodeBuilder>());
        catalog.Register(std::make_unique<StatelessRenderNodeBuilder<VolumetricCloudRenderNode>>(
            RenderNodeBuilderId::VolumetricCloud, "Volumetric Cloud"));
        catalog.Register(std::make_unique<StatelessRenderNodeBuilder<DebugProbeGridRenderNode>>(
            RenderNodeBuilderId::DebugProbeGrid, "Debug Probe Grid"));
        return catalog;
    }

    void RenderNodeBuilderCatalog::Register(std::unique_ptr<IRenderNodeBuilder> builder)
    {
        if (builder) {
            m_builders.push_back(std::move(builder));
        }
    }

    const IRenderNodeBuilder* RenderNodeBuilderCatalog::Find(RenderNodeBuilderId id) const
    {
        for (const auto& builder : m_builders) {
            if (builder && builder->Id() == id) {
                return builder.get();
            }
        }
        return nullptr;
    }

    std::shared_ptr<IRenderNode> RenderNodeBuilderCatalog::Build(RenderNodeBuilderId id,
                                                                 const RenderNodeBuildContext& context) const
    {
        const IRenderNodeBuilder* builder = Find(id);
        return builder ? builder->Build(context) : nullptr;
    }

    std::array<std::shared_ptr<IRenderNode>, RenderNodeBuilderCatalog::kSequenceNodeCount>
    RenderNodeBuilderCatalog::BuildBuiltinSequenceNodes(const RenderNodeBuildContext& context) const
    {
        std::array<std::shared_ptr<IRenderNode>, kSequenceNodeCount> nodes{};
        for (size_t i = 0; i < nodes.size(); ++i) {
            const auto nodeType = static_cast<RenderNodeType>(i);
            nodes[i] = Build(ToBuilderId(nodeType), context);
        }
        return nodes;
    }

    RenderNodeBuilderId RenderNodeBuilderCatalog::ToBuilderId(RenderNodeType type)
    {
        switch (type) {
        case RenderNodeType::Shadow: return RenderNodeBuilderId::Shadow;
        case RenderNodeType::Opaque: return RenderNodeBuilderId::Opaque;
        case RenderNodeType::Lighting: return RenderNodeBuilderId::Lighting;
        case RenderNodeType::Transparent: return RenderNodeBuilderId::Transparent;
        case RenderNodeType::TransparentLighting: return RenderNodeBuilderId::TransparentLighting;
        case RenderNodeType::Skybox: return RenderNodeBuilderId::Skybox;
        case RenderNodeType::PostProcess: return RenderNodeBuilderId::PostProcess;
        case RenderNodeType::RuntimeAO: return RenderNodeBuilderId::RuntimeAO;
        case RenderNodeType::ProceduralSky: return RenderNodeBuilderId::ProceduralSky;
        case RenderNodeType::SdfFluid: return RenderNodeBuilderId::SdfFluid;
        case RenderNodeType::TransparentBackfaceDistance: return RenderNodeBuilderId::TransparentBackfaceDistance;
        case RenderNodeType::TransparentComposite: return RenderNodeBuilderId::TransparentComposite;
        default: return RenderNodeBuilderId::Opaque;
        }
    }
}
