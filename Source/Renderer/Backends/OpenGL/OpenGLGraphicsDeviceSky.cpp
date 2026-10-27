#include "Renderer/Backends/OpenGL/OpenGLGraphicsDevice.h"
#include "Renderer/Backends/OpenGL/OpenGLProcs.h"
#include "Renderer/Backends/OpenGL/OpenGLShaderSources.h"

#if RHI_OPENGL

#include "Foundation/Tools/DebugOutput.h"

#include <string>

namespace SasamiEngine::Render
{
    // The analytic sky on the OpenGL native path. The canonical shader is HLSL
    // (Shaders/Effects/Sky/ProceduralSky/ProceduralSky.hlsli); kOpenGLSkyFunctionGLSL is its one
    // GLSL translation, spliced in here and by the ray-march program alike.

    bool OpenGLGraphicsDevice::EnsureNativeSkyProgram()
    {
        if (m_nativeSkyProgram != 0) {
            return true;
        }
        if (m_nativeSkyFailed) {
            return false;   // a failed compile is not free; do not retry it every frame
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
            !createProgram || !attachShader || !linkProgram || !getProgramiv || !getUniformLocation) {
            m_nativeSkyFailed = true;
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

        // Same splice as the ray-march program: one copy of the sky function, two users.
        const std::string fsSource = std::string(kOpenGLNativeSkyFS_Part1) +
                                     kOpenGLSkyFunctionGLSL + kOpenGLNativeSkyFS_Part2;
        const GLuint vs = compile(GL_VERTEX_SHADER, kOpenGLNativeSkyVS);
        const GLuint fs = compile(GL_FRAGMENT_SHADER, fsSource.c_str());
        if (vs == 0 || fs == 0) {
            if (vs != 0) deleteShader(vs);
            if (fs != 0) deleteShader(fs);
            m_nativeSkyFailed = true;
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
            m_nativeSkyFailed = true;
            return false;
        }

        m_nativeSkyProgram = program;
        m_skyLocInvVP      = getUniformLocation(program, "u_invViewProjection");
        m_skyLocSunDir     = getUniformLocation(program, "u_sunDir");
        m_skyLocSunColor   = getUniformLocation(program, "u_sunColor");
        m_skyLocSunI       = getUniformLocation(program, "u_sunI");
        m_skyLocExposure   = getUniformLocation(program, "u_exposure");
        m_skyLocRenderSize = getUniformLocation(program, "u_renderSize");
        m_skyLocEnvCube    = getUniformLocation(program, "u_envCube");
        m_skyLocHasEnvCube = getUniformLocation(program, "u_hasEnvCube");
        return true;
    }

    void OpenGLGraphicsDevice::RenderNativeSky(const RhiBackendMeshFrameDesc& desc)
    {
        if (!EnsureNativeSkyProgram()) {
            return;
        }

        auto useProgram        = LoadGlProc<GlUseProgramFn>("glUseProgram");
        auto uniformMatrix4fv  = LoadGlProc<GlUniformMatrix4fvFn>("glUniformMatrix4fv");
        auto uniform3fv        = LoadGlProc<GlUniform3fvFn>("glUniform3fv");
        auto uniform2f         = LoadGlProc<GlUniform2fFn>("glUniform2f");
        auto uniform1f         = LoadGlProc<GlUniform1fFn>("glUniform1f");
        if (!useProgram || !uniformMatrix4fv || !uniform3fv || !uniform1f) {
            return;
        }

        // Depth off: the sky is the background, so it must not be rejected by depth the meshes have
        // not written yet, nor write depth that would then reject them.
        const GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);

        useProgram(m_nativeSkyProgram);
        // Row-major data read as column-major is the transpose, which makes GLSL's M * v equal the
        // HLSL shaders' mul(v, M) -- the same convention the ray-march path documents.
        if (m_skyLocInvVP >= 0)      { uniformMatrix4fv(m_skyLocInvVP, 1, GL_FALSE, desc.invViewProjection); }
        if (m_skyLocSunDir >= 0)     { uniform3fv(m_skyLocSunDir, 1, desc.sunDir); }
        if (m_skyLocSunColor >= 0)   { uniform3fv(m_skyLocSunColor, 1, desc.sunColor); }
        if (m_skyLocSunI >= 0)       { uniform1f(m_skyLocSunI, desc.sunIntensity); }
        if (m_skyLocExposure >= 0)   { uniform1f(m_skyLocExposure, desc.exposure); }
        if (m_skyLocRenderSize >= 0 && uniform2f) {
            uniform2f(m_skyLocRenderSize,
                      desc.renderWidth  > 0.0f ? desc.renderWidth  : 1.0f,
                      desc.renderHeight > 0.0f ? desc.renderHeight : 1.0f);
        }
        // desc.environmentCubeSrv stays 0/unresolved until Skybox::BuildNativeCubemap's async HDR
        // load finishes, so the shader keeps falling back to the procedural sky until then. Unit 0
        // is free here: the sky pass binds no other texture.
        unsigned int envCubeTexture = 0;
        unsigned int envCubeTarget = GL_TEXTURE_CUBE_MAP;
        if (desc.environmentCubeSrv != 0) {
            const auto it = m_rhiTextureViews.find(desc.environmentCubeSrv);
            if (it != m_rhiTextureViews.end()) {
                envCubeTexture = it->second.texture;
                envCubeTarget  = it->second.target;
            }
        }
        auto activeTexture = LoadGlProc<GlActiveTextureFn>("glActiveTexture");
        auto uniform1i     = LoadGlProc<GlUniform1iFn>("glUniform1i");
        if (activeTexture) {
            activeTexture(GL_TEXTURE0);
        }
        glBindTexture(envCubeTarget, envCubeTexture);
        if (uniform1i) {
            if (m_skyLocEnvCube >= 0)    { uniform1i(m_skyLocEnvCube, 0); }
            // Written explicitly in both directions rather than relying on GL's default uniform
            // value, so a program that once had a cube cannot keep claiming one.
            if (m_skyLocHasEnvCube >= 0) { uniform1i(m_skyLocHasEnvCube, envCubeTexture != 0 ? 1 : 0); }
        }

        glDrawArrays(GL_TRIANGLES, 0, 3);

        // Leave no dangling reference to the Skybox cube: it is released and rebuilt when the HDR
        // source changes, and a stale binding here would outlive it.
        glBindTexture(envCubeTarget, 0);
        useProgram(0);

        glDepthMask(GL_TRUE);
        if (depthWasEnabled) {
            glEnable(GL_DEPTH_TEST);
        }
    }
}

#endif // RHI_OPENGL
