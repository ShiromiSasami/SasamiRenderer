// Where this sample's keyboard and mouse events go: the camera, plus F2 for the GBuffer debug
// view cycle. Split out of the lifecycle hooks because this is the only part of the sample
// that touches InputSystem's signals or raw Win32 virtual-key codes, and the lifetime rule for
// these connections (they are scoped_connections owned by RenderingApp) is local to it.

#include "RenderingApp.h"
#include "ApplicationCore.h"
#include "Input/CameraInputBinding.h"
#include "Input/InputSystem.h"
#include "Renderer/Structures/RendererEnums.h"

#include <windows.h>

// windows.h defines min and max as macros, which breaks any std::min / std::max a future
// handler here might reach for. Undefined the same way the renderer's translation units do.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace SasamiEngine::Sample::PBR
{
    void RenderingApp::BindInputEvents(ApplicationCore& app)
    {
        auto& input = InputSystem::Instance();
        m_inputConnections = ConnectCameraInput([this]() { return m_camera; });
        m_inputConnections.emplace_back(input.ConnectOnKeyUp([&app](WPARAM k) {
            if (k == VK_F2 &&
                app.GetRenderPathModeIndex() == static_cast<int>(RendererEnums::RenderPathMode::Raster)) {
                app.CycleGBufferDebugView(1);
            }
        }));
    }
}
