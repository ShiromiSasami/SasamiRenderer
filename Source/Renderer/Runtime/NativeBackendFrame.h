#pragma once

#include "Renderer/RHI/GraphicsDevice.h"

namespace SasamiEngine::Render
{
    // True when the device draws its whole frame itself through ExecuteBackendFrame instead of
    // running the render-pass graph. Only D3D12 reports supportsFeatureRenderPasses; DirectX 11,
    // Vulkan and OpenGL take the native path.
    //
    // This lives in one place on purpose. Three byte-identical copies used to sit in the anonymous
    // namespaces of Renderer.cpp, RendererFrameGraph.cpp and RendererInitialization.cpp, so any
    // change to the condition meant remembering all three and silently diverged if one was missed.
    inline bool UsesNativeBackendFrame(const IRHIDevice& device)
    {
        const RhiBackendCapabilities& capabilities = device.GetCapabilities();
        return !capabilities.supportsFeatureRenderPasses && capabilities.supportsNativeFrame;
    }
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
