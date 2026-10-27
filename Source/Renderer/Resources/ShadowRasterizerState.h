#pragma once

#include <d3d12.h>

#include "d3dx12.h"

namespace SasamiEngine::Render
{
    // Rasterizer state of the directional cascaded shadow depth pass. One definition shared by the
    // vertex-shader shadow PSO (RenderPipelineStateCache.cpp) and the virtual-geometry mesh-shader
    // shadow PSO (RenderPipelineStateCacheMeshShader.cpp) so both bake identical depth bias.
    inline D3D12_RASTERIZER_DESC MakeDirectionalShadowRasterizerDesc()
    {
        auto rast = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        rast.CullMode = D3D12_CULL_MODE_NONE;

        auto shadowRast = rast;
        // DepthClipEnable is deliberately left at the D3D12 default (TRUE).
        //
        // It is tempting to disable it: the CPU side keeps casters that sit between the light
        // and the cascade box on purpose (RendererFrameGraph culls with
        // Culling::IsAabbVisibleIgnoringNearPlane, because such an object is outside the box
        // yet still casts INTO it), and clipping throws those same casters away at the near
        // plane -- so CPU intent and GPU behaviour genuinely disagree here.
        //
        // But disabling clip is the wrong repair, and was measured to be actively worse:
        // clamping puts an out-of-range caster at the near plane's depth, i.e. the closest
        // value possible, so it then occludes everything behind it. An A/B at camera height 20
        // showed large lit regions going fully black with clip disabled
        // (Tools/Debug/Screenshots/height/h_20.png vs h_20_clipTRUE.png).
        //
        // The real fix is to make the light-space slab actually contain those casters --
        // LightSystemShadow sizes it from frustumRadius + margin while offsetting the light by
        // max(m_lightDistance, frustumRadius + margin), so the two disagree. Until that is
        // corrected, clipping (losing a distant caster's shadow) is the lesser artifact than
        // clamping (a single caster blacking out everything behind it).
        // Reverse-Z: NDC now maps near=1.0, far=0.0, so the constant-bias push toward the
        // light must flip sign to still move the caster's depth away from the receiver.
        // These magnitudes are carried over unchanged from forward-Z and will need
        // re-tuning -- a sign flip alone does not make them optimal for reverse-Z.
        shadowRast.DepthBias = -1000;
        shadowRast.DepthBiasClamp = 0.0f;
        shadowRast.SlopeScaledDepthBias = -2.0f;

        return shadowRast;
    }
}
