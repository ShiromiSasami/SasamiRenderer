#include "IUIRenderBackend.h"

#if RHI_DIRECTX12
#include "UIRenderBackendDx12.h"
#elif RHI_DIRECTX11
#include "UIRenderBackendDx11.h"
#elif RHI_OPENGL
#include "UIRenderBackendOpenGL.h"
#elif RHI_VULKAN
#include "UIRenderBackendVulkan.h"
#endif

namespace SasamiEngine::App::UI
{
    // RhiBuild.props makes graphics backends mutually exclusive, so exactly one of these
    // branches -- or none, for a build with no ImGui renderer backend yet -- is compiled in.
    std::unique_ptr<IUIRenderBackend> CreateUIRenderBackend()
    {
#if RHI_DIRECTX12
        return std::make_unique<UIRenderBackendDx12>();
#elif RHI_DIRECTX11
        return std::make_unique<UIRenderBackendDx11>();
#elif RHI_OPENGL
        return std::make_unique<UIRenderBackendOpenGL>();
#elif RHI_VULKAN
        return std::make_unique<UIRenderBackendVulkan>();
#else
        return nullptr;
#endif
    }
}
