#include "Renderer/Backends/OpenGL/OpenGLGraphicsDevice.h"
#include "Renderer/Backends/OpenGL/OpenGLProcs.h"
#include "Renderer/Backends/OpenGL/OpenGLShaderSources.h"

#if RHI_OPENGL

#include "Foundation/Tools/DebugOutput.h"

#include <string>

namespace SasamiEngine::Render
{
    // The ray-march background. It shares kOpenGLSkyFunctionGLSL with the native sky pass, which
    // is why that constant is a separate shared symbol rather than living in either file.

    bool OpenGLGraphicsDevice::EnsureRayMarchFrameResources()
    {
        if (m_rayMarchProgram != 0) {
            return true;
        }
        if (!m_hdc || !m_context || !wglMakeCurrent(m_hdc, m_context)) {
            return false;
        }

        auto createShader     = LoadGlProc<GlCreateShaderFn>("glCreateShader");
        auto shaderSource     = LoadGlProc<GlShaderSourceFn>("glShaderSource");
        auto compileShader    = LoadGlProc<GlCompileShaderFn>("glCompileShader");
        auto getShaderiv      = LoadGlProc<GlGetShaderivFn>("glGetShaderiv");
        auto getShaderInfoLog = LoadGlProc<GlGetShaderInfoLogFn>("glGetShaderInfoLog");
        auto deleteShader     = LoadGlProc<GlDeleteShaderFn>("glDeleteShader");
        auto createProgram    = LoadGlProc<GlCreateProgramFn>("glCreateProgram");
        auto attachShader     = LoadGlProc<GlAttachShaderFn>("glAttachShader");
        auto linkProgram      = LoadGlProc<GlLinkProgramFn>("glLinkProgram");
        auto getProgramiv     = LoadGlProc<GlGetProgramivFn>("glGetProgramiv");
        auto getProgramInfoLog= LoadGlProc<GlGetProgramInfoLogFn>("glGetProgramInfoLog");
        auto getUniformLocation = LoadGlProc<GlGetUniformLocationFn>("glGetUniformLocation");
        if (!createShader || !shaderSource || !compileShader || !getShaderiv || !deleteShader ||
            !createProgram || !attachShader || !linkProgram ||
            !getProgramiv || !getUniformLocation) {
            DebugLog("OpenGLGraphicsDevice::EnsureRayMarchFrameResources: required GL shader functions unavailable.\n");
            return false;
        }

        auto compile = [&](GLenum stage, const char* source) -> GLuint {
            GLuint shader = createShader(stage);
            shaderSource(shader, 1, &source, nullptr);
            compileShader(shader);
            GLint ok = GL_FALSE;
            getShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (ok == GL_FALSE) {
                if (getShaderInfoLog) {
                    char log[2048] = {};
                    GLsizei len = 0;
                    getShaderInfoLog(shader, static_cast<GLsizei>(sizeof(log)), &len, log);
                    DebugLog(log);
                    DebugLog("\n");
                }
                deleteShader(shader);
                return 0;
            }
            return shader;
        };

        const GLuint vs = compile(GL_VERTEX_SHADER,   kOpenGLRayMarchVS);
        // Spliced rather than one literal so the sky function has a single copy; see
        // kOpenGLSkyFunctionGLSL.
        const std::string rayMarchFsSource =
            std::string(kOpenGLRayMarchFS_Part1) + kOpenGLSkyFunctionGLSL + kOpenGLRayMarchFS_Part2;
        const GLuint fs = compile(GL_FRAGMENT_SHADER, rayMarchFsSource.c_str());
        if (vs == 0 || fs == 0) {
            if (vs != 0) deleteShader(vs);
            if (fs != 0) deleteShader(fs);
            return false;
        }

        const GLuint program = createProgram();
        attachShader(program, vs);
        attachShader(program, fs);
        linkProgram(program);
        deleteShader(fs);
        deleteShader(vs);

        GLint linked = GL_FALSE;
        getProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked == GL_FALSE) {
            if (getProgramInfoLog) {
                char log[2048] = {};
                GLsizei len = 0;
                getProgramInfoLog(program, static_cast<GLsizei>(sizeof(log)), &len, log);
                DebugLog(log);
                DebugLog("\n");
            }
            auto deleteProgram = LoadGlProc<GlDeleteProgramFn>("glDeleteProgram");
            if (deleteProgram) {
                deleteProgram(program);
            }
            return false;
        }

        m_rayMarchProgram    = program;
        m_rmLocInvVP         = getUniformLocation(program, "u_invVP");
        m_rmLocCamPos        = getUniformLocation(program, "u_camPos");
        m_rmLocTime          = getUniformLocation(program, "u_time");
        m_rmLocSunDir        = getUniformLocation(program, "u_sunDir");
        m_rmLocSunI          = getUniformLocation(program, "u_sunI");
        m_rmLocSunColor      = getUniformLocation(program, "u_sunColor");
        m_rmLocCloudCover    = getUniformLocation(program, "u_cloudCover");
        m_rmLocRenderW       = getUniformLocation(program, "u_renderW");
        m_rmLocRenderH       = getUniformLocation(program, "u_renderH");
        m_rmLocFluidMode     = getUniformLocation(program, "u_fluidMode");
        m_rmLocCloudDensity  = getUniformLocation(program, "u_cloudDensity");
        m_rmLocExtra0        = getUniformLocation(program, "u_extra0");
        m_rmLocExtra1        = getUniformLocation(program, "u_extra1");
        m_rmLocExtra2        = getUniformLocation(program, "u_extra2");
        return true;
    }

    bool OpenGLGraphicsDevice::RenderRayMarchFrame(const RhiBackendRayMarchFrameDesc& desc)
    {
        if (!EnsureRayMarchFrameResources()) {
            return false;
        }

        auto useProgram         = LoadGlProc<GlUseProgramFn>("glUseProgram");
        auto uniformMatrix4fv   = LoadGlProc<GlUniformMatrix4fvFn>("glUniformMatrix4fv");
        auto uniform4fv         = LoadGlProc<GlUniform4fvFn>("glUniform4fv");
        auto uniform3fv         = LoadGlProc<GlUniform3fvFn>("glUniform3fv");
        auto uniform1f          = LoadGlProc<GlUniform1fFn>("glUniform1f");
        if (!useProgram || !uniformMatrix4fv || !uniform4fv || !uniform3fv || !uniform1f) {
            return false;
        }

        const GLsizei w = desc.renderWidth  > 0.0f ? static_cast<GLsizei>(desc.renderWidth)  : 1;
        const GLsizei h = desc.renderHeight > 0.0f ? static_cast<GLsizei>(desc.renderHeight) : 1;
        glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST);

        useProgram(m_rayMarchProgram);

        // Upload matrix — row-major data; GL reads as col-major = transposed;
        // GLSL M*v then == HLSL mul(v, M_rowmajor)
        if (m_rmLocInvVP >= 0) {
            uniformMatrix4fv(m_rmLocInvVP, 1, GL_FALSE, desc.invViewProjection);
        }
        if (m_rmLocCamPos >= 0)     { uniform3fv(m_rmLocCamPos,    1, desc.cameraPos); }
        if (m_rmLocTime   >= 0)     { uniform1f (m_rmLocTime,         desc.sceneTimeSec); }
        if (m_rmLocSunDir >= 0)     { uniform3fv(m_rmLocSunDir,    1, desc.sunDir); }
        if (m_rmLocSunI   >= 0)     { uniform1f (m_rmLocSunI,         desc.sunIntensity); }
        if (m_rmLocSunColor >= 0)   { uniform3fv(m_rmLocSunColor,  1, desc.sunColor); }
        if (m_rmLocCloudCover >= 0) { uniform1f (m_rmLocCloudCover,   desc.cloudCover); }
        if (m_rmLocRenderW >= 0)    { uniform1f (m_rmLocRenderW,      desc.renderWidth); }
        if (m_rmLocRenderH >= 0)    { uniform1f (m_rmLocRenderH,      desc.renderHeight); }
        if (m_rmLocFluidMode >= 0)  { uniform1f (m_rmLocFluidMode,    desc.fluidMode); }
        if (m_rmLocCloudDensity >= 0) { uniform1f(m_rmLocCloudDensity, desc.cloudDensity); }

        const float extra0[4] = {
            desc.debugMode,
            desc.tanHalfFovY * desc.aspectRatio,
            desc.tanHalfFovY,
            desc.explicitCameraBasis ? 1.0f : 0.0f,
        };
        const float extra1[4] = { desc.cameraRight[0], desc.cameraRight[1], desc.cameraRight[2], 0.0f };
        const float extra2[4] = { desc.cameraUp[0],    desc.cameraUp[1],    desc.cameraUp[2],    0.0f };
        if (m_rmLocExtra0 >= 0) { uniform4fv(m_rmLocExtra0, 1, extra0); }
        if (m_rmLocExtra1 >= 0) { uniform4fv(m_rmLocExtra1, 1, extra1); }
        if (m_rmLocExtra2 >= 0) { uniform4fv(m_rmLocExtra2, 1, extra2); }

        glDrawArrays(GL_TRIANGLES, 0, 3);
        useProgram(0);
        return true;
    }
}

#endif // RHI_OPENGL
