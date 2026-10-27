#pragma once

#include "Renderer/Backends/DirectX11/Dx11ShadowMap.h"

#include <cstdint>

#if RHI_DIRECTX11
#include <d3d11.h>
#endif

namespace SasamiEngine::Render
{
#if RHI_DIRECTX11
    // The prepass consumes exactly the same already-resolved draw description the shadow pass
    // does -- vertex/index buffer, stride, row-major model matrix, optional bone palette -- so it
    // reuses that struct rather than declaring a second copy of it. The alias exists so the
    // signatures below read as geometry rather than as shadow casting.
    using Dx11PrepassDraw = Dx11ShadowCaster;

    // Renders screen-space depth and world-space normals for the DirectX 11 native path.
    //
    // The native path is forward-shaded, so it has none of the GBuffer attachments the D3D12 SSAO
    // pass reads (SSAORenderPass::Setup Read()s "SceneDepth" and "GBufferNormal"). This produces
    // those two inputs, in the same formats and the same normal encoding the D3D12 render targets
    // use, so Dx11SsaoGenerator can drive the shared SSAO shaders unmodified.
    //
    // Deliberately kept separate from the back-buffer depth the shaded passes own: that one is
    // D32_FLOAT with no shader-resource binding, and sampling a surface while it is also the bound
    // depth target is not legal anyway.
    class Dx11DepthNormalPrepass
    {
    public:
        // Compiles the prepass shaders on first call and (re)allocates the targets whenever the
        // render size changes. Safe to call every frame.
        bool Ensure(ID3D11Device* device, uint32_t width, uint32_t height);

        // Renders every draw into the depth and normal targets. Binds its own targets, viewport
        // and states and restores nothing; the caller re-binds the back buffer afterwards.
        void Render(ID3D11DeviceContext* context,
                    const Dx11PrepassDraw* draws,
                    uint32_t drawCount,
                    const float viewProjection[16]);

        ID3D11ShaderResourceView* DepthSrv() const { return m_depthSrv.Get(); }
        ID3D11ShaderResourceView* NormalSrv() const { return m_normalSrv.Get(); }
        bool IsValid() const { return m_dsv != nullptr && m_depthSrv != nullptr && m_normalRtv != nullptr; }

    private:
        bool CreateShaders(ID3D11Device* device);
        bool CreateStates(ID3D11Device* device);
        bool CreateConstantBuffers(ID3D11Device* device);
        bool CreateTargets(ID3D11Device* device, uint32_t width, uint32_t height);
        void RenderDraw(ID3D11DeviceContext* context,
                        const Dx11PrepassDraw& draw,
                        const float viewProjection[16]);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTexture;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_dsv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_depthSrv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_normalTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_normalRtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_normalSrv;

        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_staticVertexShader;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> m_staticInputLayout;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_skinnedVertexShader;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> m_skinnedInputLayout;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;

        Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerState;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStencilState;
        Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendState;

        Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_boneConstantBuffer;

        uint32_t m_width = 0;
        uint32_t m_height = 0;
        // Set once a shader compile has failed so the attempt is not repeated every frame, the
        // same guard Dx11GraphicsDevice::EnsureSkyResources uses for the same reason.
        bool m_shaderCompileFailed = false;
    };
#endif
}

namespace SasamiEngine { using namespace Render; }
