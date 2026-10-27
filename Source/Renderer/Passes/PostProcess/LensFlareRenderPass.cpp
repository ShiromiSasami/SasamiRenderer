#include "Renderer/Passes/PostProcess/LensFlareRenderPass.h"
#include "Renderer/RenderGraph/RenderGraph.h"
#include "Renderer/Scene/LightSystem.h"
#include "Foundation/Math/MathUtil.h"
#include "Foundation/Tools/DebugOutput.h"
#include "d3dx12.h"

#include <cstring>
#include <vector>

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

    bool LensFlareRenderPass::EnsureGridGeometry(GraphicsDevice& device)
    {
        if (m_gridIndexBuffer.IsValid()) {
            return true;
        }

        // A regular grid over the front element. The vertex shader traces each point through the
        // lens; what arrives at the sensor is a distorted version of this grid, and the triangles
        // between the traced points are the ghost. That is the whole idea of the method -- the
        // grid here is deliberately uniform, because every deformation should come from the optics.
        const uint32_t n = kGridResolution;
        std::vector<float> vertices;
        vertices.reserve(static_cast<size_t>(n) * n * 2u);
        for (uint32_t y = 0; y < n; ++y) {
            for (uint32_t x = 0; x < n; ++x) {
                vertices.push_back(static_cast<float>(x) / static_cast<float>(n - 1u));
                vertices.push_back(static_cast<float>(y) / static_cast<float>(n - 1u));
            }
        }

        std::vector<uint32_t> indices;
        indices.reserve(static_cast<size_t>(n - 1u) * (n - 1u) * 6u);
        for (uint32_t y = 0; y + 1u < n; ++y) {
            for (uint32_t x = 0; x + 1u < n; ++x) {
                const uint32_t i0 = y * n + x;
                const uint32_t i1 = i0 + 1u;
                const uint32_t i2 = i0 + n;
                const uint32_t i3 = i2 + 1u;
                indices.insert(indices.end(), { i0, i2, i1, i1, i2, i3 });
            }
        }
        m_gridIndexCount = static_cast<uint32_t>(indices.size());

        const UINT64 vbBytes = vertices.size() * sizeof(float);
        const UINT64 ibBytes = indices.size() * sizeof(uint32_t);

        // Upload heap rather than a default heap with a staging copy: the geometry is a few tens of
        // kilobytes, written once and read once per frame, so the copy would cost more than the
        // slower reads ever will.
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

        const auto uploadBuffer = [&](UINT64 bytes, const void* src, Resource& out) -> bool {
            const D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
            if (FAILED(device.CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, out))) {
                return false;
            }
            void* mapped = nullptr;
            const D3D12_RANGE noRead{ 0, 0 };
            if (FAILED(out.Get()->Map(0, &noRead, &mapped)) || mapped == nullptr) {
                return false;
            }
            std::memcpy(mapped, src, static_cast<size_t>(bytes));
            out.Get()->Unmap(0, nullptr);
            return true;
        };

        if (!uploadBuffer(vbBytes, vertices.data(), m_gridVertexBuffer) ||
            !uploadBuffer(ibBytes, indices.data(), m_gridIndexBuffer)) {
            DebugLog("LensFlareRenderPass::EnsureGridGeometry: buffer creation failed.\n");
            m_gridVertexBuffer.Reset();
            m_gridIndexBuffer.Reset();
            return false;
        }

        m_gridVbv.BufferLocation = m_gridVertexBuffer.GetGPUVirtualAddress();
        m_gridVbv.SizeInBytes = static_cast<UINT>(vbBytes);
        m_gridVbv.StrideInBytes = sizeof(float) * 2u;

        m_gridIbv.BufferLocation = m_gridIndexBuffer.GetGPUVirtualAddress();
        m_gridIbv.SizeInBytes = static_cast<UINT>(ibBytes);
        m_gridIbv.Format = DXGI_FORMAT_R32_UINT;
        return true;
    }

    bool LensFlareRenderPass::EnsureResources(GraphicsDevice& device, const SrvAllocFn& allocateSrv, UINT width, UINT height)
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

        // Built here rather than lazily in Execute(): Execute has no device, exactly as the copy
        // texture above does not.
        EnsureGridGeometry(device);
        return true;
    }

    void LensFlareRenderPass::BuildRequirements(RenderPassRequirementBuilder& builder) const
    {
        builder.RequireGraphicsBase();
        builder.RequireFrameCoordinator();
        builder.RequireFrame();
        builder.RequireLightSystem();
    }

    void LensFlareRenderPass::Setup(RenderGraphBuilder& builder) const
    {
        // As in FxaaRenderPass, the private copy is filled by a manual CopyResource inside
        // Execute(), outside the graph's tracking, so only the BackBuffer write is declared.
        builder.Write("BackBuffer");
        builder.UseColorTarget("BackBuffer");
        builder.DependsOnPrevious();
    }

    bool LensFlareRenderPass::Execute(const RenderPassContextView& context) const
    {
        if (!context.IsSatisfied()) {
            DebugLog("LensFlareRenderPass::Execute: runtime context is invalid.\n");
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
                OutputDebugStringA("LensFlareRenderPass: back buffer format has no matching PSO variant; pass skipped.\n");
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
            // A light behind the camera produces no flare, and skipping the copy keeps the cost
            // at zero rather than paying for a full-screen copy to output black.
            return true;
        }

        // Copy the tone-mapped image out before writing to it: the shader probes it around the
        // light's own screen position to decide whether the light is visible, and the back
        // buffer cannot be sampled while it is bound as a render target.
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
        const bool rayTraced = (m_mode == LensFlareMode::RayTraced);
        cmdList->SetPipelineState(rayTraced
            ? (hdr10 ? pipelineStateCache->GetLensFlareRayTracedHdr10PipelineState()
                     : pipelineStateCache->GetLensFlareRayTracedPipelineState())
            : (hdr10 ? pipelineStateCache->GetLensFlareHdr10PipelineState()
                     : pipelineStateCache->GetLensFlarePipelineState()));
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
        // Aspect ratio reaches the shader so ghosts stay round on a wide viewport; the shader
        // has no viewport size otherwise.
        const float aspect = (m_height > 0u) ? (static_cast<float>(m_width) / static_cast<float>(m_height)) : 1.0f;
        const float flareParams[4] = { sunU, sunV, m_intensity * light.intensity, aspect };
        const float flareTint[4] = {
            light.color[0], light.color[1], light.color[2], static_cast<float>(m_bladeCount),
        };
        // The shader only needs to know whether it is drawing the authored table or the procedural
        // set; the ray-traced mode is a different pipeline, not a branch in this shader.
        const float flareMode[4] = {
            static_cast<float>(static_cast<int>(m_mode)),
            static_cast<float>(kGridResolution),
            static_cast<float>(kGhostPathCount),
            0.0f,
        };
        const D3D12_GPU_VIRTUAL_ADDRESS flareCbGpu = inputs.execution.frameCoordinator->PushCameraCB(
            *inputs.execution.frame, kIdentity, kIdentity, flareParams, flareTint, flareMode);
        if (flareCbGpu != 0) {
            cmdList->SetGraphicsRootConstantBufferView(2, flareCbGpu);
        }

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
#if defined(_DEBUG)
        DebugIncrementDrawCount();
#endif
        if (m_mode == LensFlareMode::RayTraced) {
            // One instance per surface pair: each ghost is its own traced grid, and they overlap
            // freely because the blend is additive.
            cmdList->IASetVertexBuffers(0, 1, &m_gridVbv);
            cmdList->IASetIndexBuffer(&m_gridIbv);
            cmdList->DrawIndexedInstanced(m_gridIndexCount, kGhostPathCount, 0u, 0, 0u);
        } else {
            cmdList->IASetVertexBuffers(0, 0, nullptr);
            cmdList->IASetIndexBuffer(nullptr);
            cmdList->DrawInstanced(3u, 1u, 0u, 0u);
        }
        return true;
    }
}
