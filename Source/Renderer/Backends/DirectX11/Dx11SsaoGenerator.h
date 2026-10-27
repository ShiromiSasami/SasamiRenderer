#pragma once

#include "Renderer/RHI/GraphicsDevice.h"

#include <cstdint>

#if RHI_DIRECTX11
#include <d3d11.h>
#endif

namespace SasamiEngine::Render
{
#if RHI_DIRECTX11
    // Screen-space ambient occlusion for the DirectX 11 native path: the runtimeAo term the native
    // mesh shader has had no source for until now.
    //
    // This is deliberately not a reimplementation. It drives the very shaders the D3D12 path uses
    // -- Effects/AmbientOcclusion/SSAO/{SSAO_VS, SSAO_PS, SSAO_Blur_PS}.hlsl -- so the sample
    // kernel, the occlusion weighting and the bilateral blur are the same code on both backends,
    // and only the resource binding differs. The constant buffer below therefore mirrors, field
    // for field, what Renderer.cpp pushes for the D3D12 generator.
    //
    // Depth and world normals come from Dx11DepthNormalPrepass, which stands in for the GBuffer
    // attachments the D3D12 pass reads.
    class Dx11SsaoGenerator
    {
    public:
        // Compiles the shaders on first call and (re)allocates the targets whenever the render
        // size changes. Safe to call every frame.
        bool Ensure(ID3D11Device* device, uint32_t width, uint32_t height);

        // Live SSAO tuning, carried on RhiBackendMeshFrameDesc and ultimately the RenderSettings
        // fields of the same names. Grouped rather than passed as five more scalars so the Render
        // signature below stays readable; the defaults match RenderSettings.h.
        struct Tuning
        {
            float radius = 0.5f;
            float bias = 0.025f;
            float intensity = 1.0f;
            float thickness = 0.15f;
            uint32_t quality = 1u;  // clamped to 0..2 by SSAO_PS.hlsl's ResolveSampleCount
        };

        // Runs the SSAO pass and its bilateral blur. Binds its own targets, viewport and states
        // and restores nothing beyond unbinding what it read; the caller re-binds the back buffer.
        void Render(ID3D11DeviceContext* context,
                    ID3D11ShaderResourceView* depthSrv,
                    ID3D11ShaderResourceView* normalSrv,
                    const float viewProjection[16],
                    const float invViewProjection[16],
                    const float cameraPos[3],
                    uint32_t width,
                    uint32_t height,
                    const Tuning& tuning);

        // Paints the blurred visibility term over `target` as greyscale. Opt-in debug aid; see
        // NativeAoDebug_PS.hlsl for why it exists.
        void DrawDebugView(ID3D11DeviceContext* context,
                           ID3D11RenderTargetView* target,
                           uint32_t width,
                           uint32_t height);

        // The blurred visibility term, 1.0 = unoccluded. This is what the mesh shader samples.
        ID3D11ShaderResourceView* Srv() const { return m_blurSrv.Get(); }
        bool IsValid() const { return m_rawRtv != nullptr && m_blurSrv != nullptr && m_ssaoPixelShader != nullptr; }

    private:
        bool CreateShaders(ID3D11Device* device);
        bool CreateStates(ID3D11Device* device);
        bool CreateConstantBuffer(ID3D11Device* device);
        bool CreateTargets(ID3D11Device* device, uint32_t width, uint32_t height);
        void BeginFullscreenPass(ID3D11DeviceContext* context,
                                 ID3D11RenderTargetView* target,
                                 uint32_t width,
                                 uint32_t height);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_rawTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rawRtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_rawSrv;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_blurTexture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_blurRtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_blurSrv;

        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_fullscreenVertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ssaoPixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_blurPixelShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_debugPixelShader;

        Microsoft::WRL::ComPtr<ID3D11SamplerState> m_pointClampSampler;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerState;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStencilState;
        Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendState;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;

        uint32_t m_width = 0;
        uint32_t m_height = 0;
        // Set once a shader compile has failed so the attempt is not repeated every frame.
        bool m_shaderCompileFailed = false;
    };
#endif
}

namespace SasamiEngine { using namespace Render; }
