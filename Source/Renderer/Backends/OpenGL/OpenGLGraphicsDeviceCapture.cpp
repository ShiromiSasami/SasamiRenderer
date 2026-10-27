#include "Renderer/Backends/OpenGL/OpenGLGraphicsDevice.h"
#include "Renderer/Backends/OpenGL/OpenGLProcs.h"

#if RHI_OPENGL

#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Capture/PngWriter.h"

#include <vector>

namespace SasamiEngine::Render
{
    // Reading the back buffer back to the CPU, kept apart from the drawing paths because it is
    // the one place the OpenGL backend runs the pipeline backwards. Mirrors
    // VulkanGraphicsDeviceCapture.cpp.

    bool OpenGLGraphicsDevice::CaptureBackBufferToPng(const wchar_t* path)
    {
        if (!path || !m_hdc || !m_context) {
            return false;
        }

        // The viewport is the authority on the drawable size here: this backend keeps no
        // back-buffer dimensions of its own, it just draws into whatever the window currently is.
        GLint viewport[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, viewport);
        const uint32_t width = static_cast<uint32_t>(viewport[2]);
        const uint32_t height = static_cast<uint32_t>(viewport[3]);
        if (width == 0u || height == 0u) {
            DebugLog("OpenGLGraphicsDevice::CaptureBackBufferToPng: empty viewport.\n");
            return false;
        }

        std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4u);
        glReadBuffer(GL_BACK);
        // Rows are tightly packed in the destination, so the default 4-byte pack alignment would
        // corrupt any width that is not a multiple of 4.
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        // BGRA, not RGBA: PngWriter encodes with GUID_WICPixelFormat32bppBGRA, so despite the
        // "Rgba8" in its name it reads the bytes as BGRA. The DirectX 11 capture says the same
        // thing the other way round -- it swaps R and B by hand when the back buffer is
        // R8G8B8A8. Asking GL for BGRA lets the driver do that conversion instead.
        glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                     GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());

        // OpenGL's origin is bottom-left and PNG's is top-left, so the rows come out upside down.
        const size_t rowBytes = static_cast<size_t>(width) * 4u;
        std::vector<uint8_t> rowScratch(rowBytes);
        for (uint32_t y = 0; y < height / 2u; ++y) {
            uint8_t* top = pixels.data() + static_cast<size_t>(y) * rowBytes;
            uint8_t* bottom = pixels.data() + static_cast<size_t>(height - 1u - y) * rowBytes;
            std::memcpy(rowScratch.data(), top, rowBytes);
            std::memcpy(top, bottom, rowBytes);
            std::memcpy(bottom, rowScratch.data(), rowBytes);
        }

        return SavePngBgra8(std::wstring(path), pixels.data(), width, height);
    }
}

#endif // RHI_OPENGL
