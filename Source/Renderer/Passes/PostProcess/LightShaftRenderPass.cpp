#include "Renderer/Passes/PostProcess/LightShaftRenderPass.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/Scene/LightSystem.h"
#include "Foundation/Math/MathUtil.h"
#include "Foundation/Tools/DebugOutput.h"
#include "d3dx12.h"

namespace SasamiEngine::Render
{
    namespace
    {
        // Far enough that the projected point is the direction, not a position: the sun is
        // directional, so any large distance along it lands on the same pixel.
        constexpr float kSunDistance = 100000.0f;

        // clip = v * M, matching the shaders' mul(float4(pos, 1), matrix) with row_major
        // storage (see Skybox_VS.hlsl). Returns false when the sun is behind the camera,
        // where the projected position is a mirrored ghost rather than the real one.
        bool ProjectSunToUv(const float* pv, const float* cameraPos, const float (&towardSun)[3], float& outU, float& outV)
        {
            if (pv == nullptr || cameraPos == nullptr) {
                return false;
            }

            const float world[4] = {
                cameraPos[0] + towardSun[0] * kSunDistance,
                cameraPos[1] + towardSun[1] * kSunDistance,
                cameraPos[2] + towardSun[2] * kSunDistance,
                1.0f,
            };

            float clip[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    clip[column] += world[row] * pv[row * 4 + column];
                }
            }
            if (clip[3] <= 0.0f) {
                return false;
            }

            const float ndcX = clip[0] / clip[3];
            const float ndcY = clip[1] / clip[3];
            outU = ndcX * 0.5f + 0.5f;
            outV = -ndcY * 0.5f + 0.5f;   // NDC is y-up, UV is y-down
            return true;
        }
    }

    bool LightShaftRenderPass::EnsureResources(GraphicsDevice& device, const SrvAllocFn& allocateSrv, UINT width, UINT height)
    {
        if (width == 0u || height == 0u) {
            return false;
        }
        // CopyResource needs identical formats, so the copy follows the back buffer's own format.
        const DXGI_FORMAT inputFormat = m_backBufferResource
            ? m_backBufferResource->GetDesc().Format
            : DXGI_FORMAT_R8G8B8A8_UNORM;
        if (m_inputCopyTexture.IsValid() && m_width == width && m_height == height && m_inputFormat == inputFormat) {
            return true;
        }

        m_inputCopyTexture.Reset();
        m_inputFormat = DXGI_FORMAT_UNKNOWN;
        m_width = 0;
        m_height = 0;

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        const D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
            inputFormat, width, height, 1, 1);

        if (FAILED(device.CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                   nullptr, m_inputCopyTexture))) {
            return false;
        }

        if (!m_srvAllocated) {
            if (!allocateSrv || !allocateSrv(1u, m_inputCopySrvCpu, m_inputCopySrv)) {
                m_inputCopyTexture.Reset();
                return false;
            }
            m_srvAllocated = true;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = inputFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device.CreateShaderResourceView(m_inputCopyTexture, &srvDesc, m_inputCopySrvCpu);

        m_width = width;
        m_height = height;
        m_inputFormat = inputFormat;
        return true;
    }

    void LightShaftRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
        builder.RequireLightSystem();
    }

    void LightShaftRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        // As in FxaaRenderPass, the private copy is filled by a manual CopyResource inside
        // Execute(), outside the graph's tracking, so only the BackBuffer write is declared.
        builder.Write("BackBuffer");
        builder.UseColorTarget("BackBuffer");
        builder.DependsOnPrevious();
    }

    bool LightShaftRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("LightShaftRenderPass::Execute: runtime context is invalid.\n");
            return false;
        }
        if (!m_enabled) {
            // BackBuffer already holds the tone-mapped image; contributing nothing is exactly
            // what a disabled additive pass should do.
            return true;
        }
        if (!m_inputCopyTexture.IsValid() || !m_backBufferResource) {
            return true;
        }
        // A PSO's RTV format must match the bound RTV, so the PSO is picked per back-buffer
        // format; any format without a variant skips. Known limitation: in HDR10 mode the effect
        // operates on PQ-encoded values.
        const bool hdr10 = (m_inputFormat == DXGI_FORMAT_R10G10B10A2_UNORM);
        if (!hdr10 && m_inputFormat != DXGI_FORMAT_R8G8B8A8_UNORM) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                OutputDebugStringA("LightShaftRenderPass: back buffer format has no matching PSO variant; pass skipped.\n");
            }
            return true;
        }

        const RenderPassFrameInputs& inputs = context.Inputs();
        CommandList* cmdList = inputs.execution.cmdList;
        RenderPipelineStateCache* pipelineStateCache = inputs.execution.pipelineStateCache;
        if (!cmdList || !pipelineStateCache || !inputs.execution.viewport || !inputs.execution.scissorRect ||
            !inputs.execution.frameCoordinator || !inputs.execution.frame) {
            return false;
        }

        // The light's own direction is the direction light travels; the sun is the other way.
        const RenderDirectionalLight light = inputs.lighting.lightSystem->GetDirectionalLightSettings();
        float lightDirection[3] = { 0.0f, -1.0f, 0.0f };
        Math::DirectionFromYawPitch(light.yaw, light.pitch, lightDirection);
        const float towardSun[3] = { -lightDirection[0], -lightDirection[1], -lightDirection[2] };

        float sunU = 0.0f;
        float sunV = 0.0f;
        const bool sunInFront = ProjectSunToUv(inputs.camera.pv, inputs.camera.pos, towardSun, sunU, sunV);
        if (!sunInFront) {
            // Nothing to add, and skipping the copy keeps the cost at zero while the sun is
            // behind the camera -- which is most of the time in an interior scene.
            return true;
        }

        // Copy the tone-mapped image out before overwriting it: the back buffer is bound as a
        // render target here and cannot also be sampled.
        D3D12_RESOURCE_BARRIER preCopy[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_backBufferResource,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_inputCopyTexture.Get(),
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_COPY_DEST),
        };
        cmdList->ResourceBarrier(_countof(preCopy), preCopy);
        cmdList->Get()->CopyResource(m_inputCopyTexture.Get(), m_backBufferResource);

        D3D12_RESOURCE_BARRIER postCopy[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_inputCopyTexture.Get(),
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            CD3DX12_RESOURCE_BARRIER::Transition(m_backBufferResource,
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET),
        };
        cmdList->ResourceBarrier(_countof(postCopy), postCopy);

        cmdList->SetGraphicsRootSignature(pipelineStateCache->GetRootSignature());
        cmdList->SetPipelineState(hdr10 ? pipelineStateCache->GetLightShaftHdr10PipelineState()
                                        : pipelineStateCache->GetLightShaftPipelineState());
        cmdList->RSSetViewports(1, inputs.execution.viewport);
        cmdList->RSSetScissorRects(1, inputs.execution.scissorRect);

        // No SetDescriptorHeaps: the SRV is in the main heap the frame already has bound.
        cmdList->SetGraphicsRootDescriptorTable(0, m_inputCopySrv);

        static const float kIdentity[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };
        // The sun's own intensity scales the shafts: turning the light down should dim them
        // rather than leaving a bright fan over a dark scene.
        const float shaftParams[4] = { sunU, sunV, m_intensity * light.intensity, 1.0f };
        const D3D12_GPU_VIRTUAL_ADDRESS shaftCbGpu = inputs.execution.frameCoordinator->PushCameraCB(
            *inputs.execution.frame, kIdentity, kIdentity, shaftParams, nullptr, nullptr);
        if (shaftCbGpu != 0) {
            cmdList->SetGraphicsRootConstantBufferView(2, shaftCbGpu);
        }

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->IASetVertexBuffers(0, 0, nullptr);
        cmdList->IASetIndexBuffer(nullptr);
#if defined(_DEBUG)
        DebugIncrementDrawCount();
#endif
        cmdList->DrawInstanced(3u, 1u, 0u, 0u);
        return true;
    }
}
