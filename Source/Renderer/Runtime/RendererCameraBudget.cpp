// RendererCameraBudget.cpp
// Sizes the frame's per-draw camera constant buffer from the shadow configuration and the
// scene's static+skinned draw-item counts. Records no commands and touches neither the
// render graph nor any pass, which is why it is separate from RendererFrameGraph.cpp's
// frame-graph code.
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Passes/PostProcess/BloomRenderer.h"

#include <algorithm>
#include <cstddef>
#include <limits>

// windows.h (pulled in transitively by the D3D12 headers) defines min/max as macros, which
// breaks std::min and std::numeric_limits<T>::max(). Same guard as RendererFrameGraph.cpp.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace SasamiEngine::Render
{
    void Renderer::EnsureFrameCameraConstantBuffers(RendererFrameCoordinator::FrameContext& frame)
    {
        const size_t drawItemCount = m_sceneSubmitter.GetDrawItems().size();
        const size_t skinnedDrawItemCount = m_sceneSubmitter.GetSkinnedDrawItems().size();
        // Raster shadow rendering records one draw per directional cascade, then the
        // main scene pass records another draw per item. Reserve the spot-shadow pass
        // as well so CSM4 cannot overwrite later per-draw camera constants. Skinned
        // items go through the same per-cascade + main + spot-shadow push sequence
        // (see drawSkinnedItems / drawSkinnedShadowItems below) so they need the same
        // per-item budget as static items.
        const size_t cameraCbPerItemSlots =
            static_cast<size_t>(LightSystem::kDirectionalCascadeCount) + 2u;
        // Point-light shadows record one draw per cube face (see LightSystemShadow.cpp's
        // "point light #0 cube shadow pass"), for both static and skinned items. VSM
        // directional modes record a second, VSM-only cascade pass, but only for static
        // items (skinned draws have no VSM color/moment pipeline).
        const bool hasPointLightShadow = !m_lightSystem.GetPointLights().empty();
        const size_t cameraCbPointShadowSlots = hasPointLightShadow
            ? (drawItemCount + skinnedDrawItemCount) * static_cast<size_t>(ShadowMapManager::kPointShadowFaceCount)
            : 0u;
        const DirectionalShadowMode shadowMode = m_lightSystem.GetDirectionalLightSettings().shadowMode;
        const bool isVsmShadowMode =
            shadowMode == DirectionalShadowMode::Vsm || shadowMode == DirectionalShadowMode::Vsm4;
        const size_t cameraCbVsmSlots = isVsmShadowMode
            ? drawItemCount * static_cast<size_t>(LightSystem::kDirectionalCascadeCount)
            : 0u;
        const size_t cameraCbDrawSlots =
            (drawItemCount + skinnedDrawItemCount) * cameraCbPerItemSlots +
            cameraCbPointShadowSlots + cameraCbVsmSlots;
        const size_t cameraCbExtraSlots = 9u; // SSAO, sky/fullscreen passes, small pass variations, and the
                                              // virtual-geometry cluster path's one per-frame camera slot.
        // BloomRenderer pushes one CB per down and up step (2 * MAX_MIPS - 1), whether or not it is
        // on this frame, so toggling it never shifts the budget.
        const size_t cameraCbBloomSlots = 2u * static_cast<size_t>(BloomRenderer::MAX_MIPS);
        const size_t cameraCbRequired = cameraCbDrawSlots + cameraCbExtraSlots + cameraCbBloomSlots;
        const UINT cameraCbRequiredClamped = static_cast<UINT>(
            (std::min)(cameraCbRequired, static_cast<size_t>((std::numeric_limits<UINT>::max)())));
        m_frameCoordinator.EnsureCameraBuffers(frame, cameraCbRequiredClamped);
    }
}
