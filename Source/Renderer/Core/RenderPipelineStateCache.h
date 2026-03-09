#pragma once

#include "Renderer/Core/GraphicsDevice.h"
#include "d3dx12.h"

namespace SasamiRenderer
{
    class RenderPipelineStateCache
    {
    public:
        bool Initialize(GraphicsDevice& device);
        inline PipelineState& GetPipelineState() { return m_pipelineState; }
        inline PipelineState& GetBasicPipelineState() { return m_basicPipelineState; }
        inline PipelineState& GetShadowPipelineState() { return m_shadowPipelineState; }
        inline PipelineState& GetTessellationPipelineState() { return m_tessPipelineState; }
        inline PipelineState& GetTessellationShadowPipelineState() { return m_tessShadowPipelineState; }
        // Backward compatibility: default skybox pipeline is HDR path.
        inline PipelineState& GetSkyboxPipelineState() { return m_skyboxHdrPipelineState; }
        inline PipelineState& GetSkyboxHdrPipelineState() { return m_skyboxHdrPipelineState; }
        inline PipelineState& GetSkyboxLdrPipelineState() { return m_skyboxLdrPipelineState; }
        inline RootSignature& GetRootSignature() { return m_rootSignature; }

    private:
        PipelineState m_pipelineState;
        PipelineState m_basicPipelineState;
        PipelineState m_shadowPipelineState;
        PipelineState m_tessPipelineState;
        PipelineState m_tessShadowPipelineState;
        PipelineState m_skyboxHdrPipelineState;
        PipelineState m_skyboxLdrPipelineState;
        RootSignature m_rootSignature;
    };
}
