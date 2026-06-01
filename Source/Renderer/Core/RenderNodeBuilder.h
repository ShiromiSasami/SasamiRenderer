#pragma once

#include "Renderer/Core/RhiTypes.h"
#include "Renderer/Passes/IRenderNode.h"
#include "Renderer/Structures/RendererEnums.h"

#include <array>
#include <memory>
#include <string_view>
#include <vector>

namespace SasamiRenderer
{
    struct RenderNodeBuildContext
    {
        RhiBackendCapabilities capabilities{};
    };

    enum class RenderNodeBuilderId
    {
        Shadow,
        Opaque,
        Lighting,
        Transparent,
        TransparentLighting,
        Skybox,
        PostProcess,
        RuntimeAO,
        ProceduralSky,
        SdfFluid,
        TransparentBackfaceDistance,
        TransparentComposite,
        RayTracing,
        VolumetricCloud,
        DebugProbeGrid,
    };

    class IRenderNodeBuilder
    {
    public:
        virtual ~IRenderNodeBuilder() = default;

        virtual RenderNodeBuilderId Id() const = 0;
        virtual std::string_view DisplayName() const = 0;
        virtual bool IsAvailable(const RenderNodeBuildContext& context) const;
        virtual std::shared_ptr<IRenderNode> Build(const RenderNodeBuildContext& context) const = 0;
    };

    class RenderNodeBuilderCatalog
    {
    public:
        using RenderNodeType = RendererEnums::RenderNodeType;
        static constexpr size_t kSequenceNodeCount =
            static_cast<size_t>(RendererEnums::RenderNodeType::TransparentComposite) + 1u;

        static RenderNodeBuilderCatalog CreateDefault();

        void Register(std::unique_ptr<IRenderNodeBuilder> builder);
        const IRenderNodeBuilder* Find(RenderNodeBuilderId id) const;
        const std::vector<std::unique_ptr<IRenderNodeBuilder>>& Builders() const { return m_builders; }

        std::array<std::shared_ptr<IRenderNode>, kSequenceNodeCount>
            BuildBuiltinSequenceNodes(const RenderNodeBuildContext& context) const;
        std::shared_ptr<IRenderNode> Build(RenderNodeBuilderId id, const RenderNodeBuildContext& context) const;

    private:
        static RenderNodeBuilderId ToBuilderId(RenderNodeType type);

        std::vector<std::unique_ptr<IRenderNodeBuilder>> m_builders;
    };
}
