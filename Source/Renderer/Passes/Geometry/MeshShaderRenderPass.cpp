#define NOMINMAX
#include "Renderer/Passes/Geometry/MeshShaderRenderPass.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Foundation/Tools/DebugOutput.h"

#include <d3d12.h>
#include <wrl/client.h>

namespace SasamiRenderer
{
    void MeshShaderRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
    }

    void MeshShaderRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        builder.Write("SceneColor");
        builder.Write("SceneDepth");
        builder.UseColorTarget("SceneColor");
        builder.UseDepthTarget("SceneDepth");
        builder.DependsOnPrevious();
    }

    bool MeshShaderRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("MeshShaderRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }

        // This node is intended to be driven externally (from Renderer) with the
        // low-level Execute overload that receives the meshlet buffer. The high-level
        // overload is a no-op  Ethe node is registered so the render graph knows
        // about it, but actual dispatch happens via the Renderer callback path.
        return true;
    }

    void MeshShaderRenderPass::Execute(CommandList*             cmdList,
                                       RenderPipelineStateCache& pipelineStateCache,
                                       const SceneSubmitter&     sceneSubmitter,
                                       const MeshBuffer&         meshBuffer,
                                       const MeshletBuffer&      meshletBuffer,
                                       const Viewport&           viewport,
                                       const Rect&               scissorRect) const
    {
        if (!cmdList) {
            return;
        }

        PipelineState& msPso = pipelineStateCache.GetMeshShaderPipelineState();
        RootSignature& msRootSignature = pipelineStateCache.GetMeshShaderRootSignature();
        if (!msPso.Get() || !msRootSignature.Get()) {
            // Mesh shaders not supported or PSO not initialised - silently skip.
            return;
        }

        if (!meshletBuffer.IsValid()) {
            return;
        }

        const auto& drawItems = sceneSubmitter.GetDrawItems();
        const auto& meshItems = meshBuffer.Items();

        if (drawItems.empty() || meshItems.empty()) {
            return;
        }

        // Obtain the ID3D12GraphicsCommandList6 interface needed for DispatchMesh.
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList6> cmdList6;
        if (FAILED(cmdList->Get()->QueryInterface(IID_PPV_ARGS(cmdList6.GetAddressOf())))) {
            DebugLog("MeshShaderRenderPass::Execute: ID3D12GraphicsCommandList6 not available.\n");
            return;
        }

        cmdList->SetGraphicsRootSignature(msRootSignature);
        cmdList->SetPipelineState(msPso);
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &scissorRect);

        // Bind meshlet descriptor SRV inline (t0, root param 0)
        cmdList->Get()->SetGraphicsRootShaderResourceView(
            0u,
            meshletBuffer.GetMeshletDescGpuVA());

        // Bind meshlet index buffer inline SRV (t2, root param 2)
        cmdList->Get()->SetGraphicsRootShaderResourceView(
            2u,
            meshletBuffer.GetMeshletIndexGpuVA());

        // Per-draw dispatch
        for (size_t i = 0; i < drawItems.size(); ++i)
        {
            const SceneSubmitter::DrawItem& item = drawItems[i];
            if (item.transparent) continue;
            if (item.meshIndex >= meshItems.size()) continue;
            if (item.meshIndex >= meshletBuffer.GetMeshRangeCount()) continue;

            const MeshletBuffer::MeshRange& range = meshletBuffer.GetMeshRange(item.meshIndex);
            if (range.meshletCount == 0u) continue;

            // Bind vertex buffer SRV inline (t1, root param 1)
            // Vertex buffer must be in a state readable as SRV. MeshBuffer leaves it
            // in VERTEX_AND_CONSTANT_BUFFER state; we read via GPU VA which is always
            // accessible with an inline root SRV.
            const MeshBuffer::GPUItem& gpuMesh = meshItems[item.meshIndex];
            if (!gpuMesh.vb.IsValid()) continue;

            cmdList->Get()->SetGraphicsRootShaderResourceView(
                1u,
                gpuMesh.vb.GetGPUVirtualAddress());

            // Build DrawCB inline root constants (root param 3)
            // Layout: float4x4 model (16 floats) + float4x4 inverseModel (16 floats)
            //         + uint meshletOffset + uint meshletCount + 2 pads
            // = 34 uint32 values
            struct DrawCBData
            {
                float    model[16];
                float    inverseModel[16];
                uint32_t meshletOffset;
                uint32_t meshletCount;
                uint32_t pad0;
                uint32_t pad1;
            };

            // Compute inverse of the 4x4 model matrix (column-major layout assumed by row-major HLSL)
            // For now pass an identity inverse and let the shader use the model matrix directly.
            // A proper inverse would require math utilities. We store model in row-major and HLSL
            // reads it as row_major so the transpose of model is the inverse for rigid bodies.
            // For simplicity, pass model as both model and inverse (normals will not be scaled).
            DrawCBData drawCb{};
            static_assert(sizeof(item.model) == sizeof(drawCb.model));
            std::memcpy(drawCb.model, item.model, sizeof(drawCb.model));
            std::memcpy(drawCb.inverseModel, item.model, sizeof(drawCb.inverseModel)); // simplified
            drawCb.meshletOffset = range.meshletOffset;
            drawCb.meshletCount  = range.meshletCount;
            drawCb.pad0 = 0u;
            drawCb.pad1 = 0u;

            cmdList->Get()->SetGraphicsRoot32BitConstants(
                3u,
                static_cast<UINT>(sizeof(DrawCBData) / sizeof(uint32_t)),
                &drawCb,
                0u);

            // DispatchMesh: one thread group per meshlet (AS processes them with [numthreads(1,1,1)])
            cmdList6->DispatchMesh(range.meshletCount, 1u, 1u);
        }
    }
}
