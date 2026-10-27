#pragma once

#include "Renderer/RHI/GraphicsDevice.h"

#include <cstdint>

#if RHI_DIRECTX11
#include <d3d11.h>
#endif

namespace SasamiEngine::Render
{
#if RHI_DIRECTX11
    // One shadow caster, already resolved by the caller. The device owns the handle->resource map,
    // so this class never resolves handles itself.
    struct Dx11ShadowCaster
    {
        ID3D11Buffer* vertexBuffer = nullptr;
        ID3D11Buffer* indexBuffer = nullptr;   // null means draw non-indexed with vertexCount
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        uint32_t vertexStride = 0;
        bool index32Bit = true;
        const float* model = nullptr;          // 16 floats, row-major
        const float* boneMatrices = nullptr;   // null for static casters; else 128 row-major 4x4
    };

    // Owns a single-cascade directional shadow map (depth-only render target, comparison sampler,
    // and the depth-only pipeline state) for the DirectX 11 native rendering path.
    class Dx11ShadowMap
    {
    public:
        // `staticVsBytecode` / `skinnedVsBytecode` are compiled depth-only vertex shaders supplied
        // by the device, which owns the shader-compilation helpers. Passing bytecode in keeps this
        // class free of any compile concern. `skinnedVsBytecode` may be null: skinned casters are
        // then skipped.
        bool Initialize(ID3D11Device* device,
                        uint32_t resolution,
                        const void* staticVsBytecode, size_t staticVsSize,
                        const void* skinnedVsBytecode, size_t skinnedVsSize);

        // Renders every caster into the depth map. Binds its own RTV-less target, viewport and
        // states, and restores nothing -- the caller re-binds the back buffer afterwards.
        void Render(ID3D11DeviceContext* context,
                    const Dx11ShadowCaster* casters,
                    uint32_t casterCount,
                    const float lightViewProjection[16]);

        ID3D11ShaderResourceView* Srv() const { return m_srv.Get(); }
        ID3D11SamplerState* ComparisonSampler() const { return m_sampler.Get(); }
        uint32_t Resolution() const { return m_resolution; }
        bool IsValid() const { return m_dsv != nullptr && m_srv != nullptr; }

    private:
        bool CreateDepthResources(ID3D11Device* device, uint32_t resolution);
        bool CreateSampler(ID3D11Device* device);
        bool CreateStaticPipeline(ID3D11Device* device, const void* staticVsBytecode, size_t staticVsSize);
        bool CreateSkinnedPipeline(ID3D11Device* device, const void* skinnedVsBytecode, size_t skinnedVsSize);
        bool CreateSharedStates(ID3D11Device* device);
        bool CreateConstantBuffers(ID3D11Device* device);
        void RenderCaster(ID3D11DeviceContext* context,
                          const Dx11ShadowCaster& caster,
                          const float lightViewProjection[16]);

        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_depthTexture;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> m_dsv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;

        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_staticVertexShader;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> m_staticInputLayout;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_skinnedVertexShader;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> m_skinnedInputLayout;

        Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerState;
        Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthStencilState;

        Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_boneConstantBuffer;

        uint32_t m_resolution = 0;
    };
#endif
}

namespace SasamiEngine { using namespace Render; }
