#include "UIFrameOverlay.h"

#include "IUIRenderBackend.h"

namespace SasamiEngine::App::UI
{
    void UIFrameOverlay::RecordOverlay(const Render::RhiOverlayFrameContext& context)
    {
        if (!m_visible || !m_backend || !m_backend->IsInitialized()) {
            return;
        }
        m_backend->RenderDrawData(context);
    }
}
