// OpenGLGraphicsDeviceAoDebug.cpp
// Draws the finished ambient-occlusion term straight to the back buffer, as grey, when
// SASAMI_GL_AO_DEBUG=1.
//
// Diagnostics only, and deliberately separate from the passes that produce the term: a lit frame
// cannot tell "the occlusion is wrong" apart from "the occlusion is not being read", and a uniform
// dimming looks the same as real occlusion. The DX11 (SASAMI_DX11_AO_DEBUG) and Vulkan
// (SASAMI_VK_AO_DEBUG) native paths carry the same switch for the same reason.
#include "Renderer/Backends/OpenGL/OpenGLGraphicsDevice.h"
#include "Renderer/Backends/OpenGL/OpenGLProcs.h"
#include "Renderer/Backends/OpenGL/OpenGLProgramBuilder.h"
#include "Renderer/Backends/OpenGL/OpenGLShaderSources.h"

#if RHI_OPENGL

namespace SasamiEngine::Render
{
    namespace
    {
        // GetEnvironmentVariableA rather than std::getenv, and resolved once into a static, both
        // matching SASAMI_GPU_VALIDATION and the DX11 path's IsNativeAoDebugViewEnabled.
        bool IsAoDebugViewEnabled()
        {
            static const bool enabled = [] {
                char        value[8] = {};
                const DWORD length   = GetEnvironmentVariableA("SASAMI_GL_AO_DEBUG", value, sizeof(value));
                return length > 0 && length < static_cast<DWORD>(sizeof(value)) && value[0] == '1';
            }();
            return enabled;
        }
    }

    void OpenGLGraphicsDevice::RenderAoDebugView(const RhiBackendMeshFrameDesc& desc)
    {
        if (!IsAoDebugViewEnabled() || !m_ssaoValid || m_runtimeAoTexture == 0 || m_aoDebugFailed) {
            return;
        }

        auto useProgram        = LoadGlProc<GlUseProgramFn>("glUseProgram");
        auto uniform1i         = LoadGlProc<GlUniform1iFn>("glUniform1i");
        auto activeTexture     = LoadGlProc<GlActiveTextureFn>("glActiveTexture");
        auto getUniformLocation = LoadGlProc<GlGetUniformLocationFn>("glGetUniformLocation");
        if (!useProgram || !uniform1i || !activeTexture || !getUniformLocation) {
            m_aoDebugFailed = true;
            return;
        }

        if (m_aoDebugProgram == 0) {
            m_aoDebugProgram = BuildGlProgram(kOpenGLSsaoVS, kOpenGLAoDebugFS, nullptr, 0u, "AO debug view");
            if (m_aoDebugProgram == 0) {
                m_aoDebugFailed = true;
                return;
            }
            m_aoDebugLocAoTexture = getUniformLocation(m_aoDebugProgram, "u_aoTexture");
        }

        // Overwrites the whole back buffer, so it has to run after the mesh passes and before the
        // overlay; the UI stays legible on top of it.
        glViewport(0,
                   0,
                   static_cast<GLsizei>(desc.renderWidth),
                   static_cast<GLsizei>(desc.renderHeight));
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);

        useProgram(m_aoDebugProgram);
        activeTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_runtimeAoTexture);
        if (m_aoDebugLocAoTexture >= 0) {
            uniform1i(m_aoDebugLocAoTexture, 0);
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glBindTexture(GL_TEXTURE_2D, 0);
        useProgram(0);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
    }

    void OpenGLGraphicsDevice::ReleaseAoDebugResources()
    {
        auto deleteProgram = LoadGlProc<GlDeleteProgramFn>("glDeleteProgram");
        if (deleteProgram && m_aoDebugProgram != 0) {
            deleteProgram(m_aoDebugProgram);
        }
        m_aoDebugProgram      = 0;
        m_aoDebugLocAoTexture = -1;
    }
} // namespace SasamiEngine::Render
#endif
