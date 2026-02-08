#include "GraphicsDevice.h"
#include "d3dx12.h"

namespace SasamiRenderer
{
    class RenderLayerConfigurator
    {
    public:
        bool Initialize(GraphicsDevice& device);
        inline PipelineState& GetPipelineState() { return m_pipelineState; }
        inline PipelineState& GetShadowPipelineState() { return m_shadowPipelineState; }
        inline PipelineState& GetTessellationPipelineState() { return m_tessPipelineState; }
        inline PipelineState& GetTessellationShadowPipelineState() { return m_tessShadowPipelineState; }
        inline RootSignature& GetRootSignature() { return m_rootSignature; }

    private:
        PipelineState m_pipelineState;
        PipelineState m_shadowPipelineState;
        PipelineState m_tessPipelineState;
        PipelineState m_tessShadowPipelineState;
        RootSignature m_rootSignature;
    };
}
