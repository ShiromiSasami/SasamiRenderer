// OpenGLGraphicsDeviceSsao.cpp
// The native path's SSAO generation and blur: the OpenGL counterpart of SSAORenderPass /
// SSAOBlurRenderPass plus the PSOs in RenderPipelineStateCacheSsao.
//
// DX12 runs both stages as full-screen raster passes (SSAO_VS.hlsl drawing three vertices, then
// SSAO_PS.hlsl and SSAO_Blur_PS.hlsl), not as compute dispatches, so this backend's missing compute
// path costs nothing here. What it does cost is the shaders themselves: DX11 and Vulkan compile the
// very same HLSL files DX12 uses, while GL consumes neither HLSL nor SPIR-V on this context, so
// kOpenGLSsaoFS / kOpenGLSsaoBlurFS are hand translations that have to be kept in step by hand.
//
// For the inputs these passes read, see OpenGLGraphicsDeviceAoPrepass.cpp. The output is a
// single-channel visibility texture, exactly what DX12's DeferredLighting_PS.hlsl samples as
// RuntimeAOTex; the native mesh fragment shader reads it under the same name.
#include "Renderer/Backends/OpenGL/OpenGLGraphicsDevice.h"
#include "Renderer/Backends/OpenGL/OpenGLProcs.h"
#include "Renderer/Backends/OpenGL/OpenGLProgramBuilder.h"
#include "Renderer/Backends/OpenGL/OpenGLShaderSources.h"

#if RHI_OPENGL

#include "Foundation/Tools/DebugOutput.h"

namespace SasamiEngine::Render
{
    namespace
    {
        // Attaches `texture` as the sole colour target of `fbo` and reports completeness.
        bool AttachColorTarget(unsigned int fbo, unsigned int texture)
        {
            auto bindFramebuffer        = LoadGlProc<GlBindFramebufferFn>("glBindFramebuffer");
            auto framebufferTexture2D   = LoadGlProc<GlFramebufferTexture2DFn>("glFramebufferTexture2D");
            auto checkFramebufferStatus = LoadGlProc<GlCheckFramebufferStatusFn>("glCheckFramebufferStatus");
            auto drawBuffers            = LoadGlProc<GlDrawBuffersFn>("glDrawBuffers");
            if (!bindFramebuffer || !framebufferTexture2D || !checkFramebufferStatus) {
                return false;
            }
            bindFramebuffer(GL_FRAMEBUFFER, fbo);
            framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            if (drawBuffers) {
                const GLenum targets[1] = { GL_COLOR_ATTACHMENT0 };
                drawBuffers(1, targets);
            }
            const GLenum status = checkFramebufferStatus(GL_FRAMEBUFFER);
            bindFramebuffer(GL_FRAMEBUFFER, 0);
            return status == GL_FRAMEBUFFER_COMPLETE;
        }

        // One 8-bit channel, matching the single visibility value DX12's SSAO targets carry. Linear
        // filtering because the consumer samples it per pixel at the same resolution, where
        // filtering only costs nothing and gains a little smoothing.
        void AllocateAoTexture(unsigned int texture, uint32_t width, uint32_t height)
        {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D,
                         0,
                         GL_R8,
                         static_cast<GLsizei>(width),
                         static_cast<GLsizei>(height),
                         0,
                         GL_RED,
                         GL_UNSIGNED_BYTE,
                         nullptr);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }

    bool OpenGLGraphicsDevice::EnsureSsaoTargets(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0) {
            return false;
        }
        // Tracked separately from the prepass targets rather than sharing their size: reusing their
        // bookkeeping would let a resize they already recorded convince this pass that its own
        // textures are still the right size.
        if (m_ssaoFbo != 0 && m_ssaoBlurFbo != 0 &&
            m_ssaoTargetWidth == width && m_ssaoTargetHeight == height) {
            return true;
        }

        auto genFramebuffers = LoadGlProc<GlGenFramebuffersFn>("glGenFramebuffers");
        if (!genFramebuffers) {
            DebugLog("OpenGLGraphicsDevice::EnsureSsaoTargets: framebuffer entry points are unavailable.\n");
            return false;
        }

        // Textures are reallocated in place on a resize rather than deleted and regenerated: the
        // framebuffers' attachments name these textures, so keeping the names keeps the attachments.
        if (m_ssaoTexture == 0) {
            GLuint texture = 0;
            glGenTextures(1, &texture);
            m_ssaoTexture = texture;
        }
        if (m_ssaoBlurTexture == 0) {
            GLuint texture = 0;
            glGenTextures(1, &texture);
            m_ssaoBlurTexture = texture;
        }
        if (m_ssaoTexture == 0 || m_ssaoBlurTexture == 0) {
            return false;
        }
        AllocateAoTexture(m_ssaoTexture, width, height);
        AllocateAoTexture(m_ssaoBlurTexture, width, height);

        if (m_ssaoFbo == 0) {
            GLuint fbo = 0;
            genFramebuffers(1, &fbo);
            m_ssaoFbo = fbo;
        }
        if (m_ssaoBlurFbo == 0) {
            GLuint fbo = 0;
            genFramebuffers(1, &fbo);
            m_ssaoBlurFbo = fbo;
        }
        if (m_ssaoFbo == 0 || m_ssaoBlurFbo == 0) {
            return false;
        }
        if (!AttachColorTarget(m_ssaoFbo, m_ssaoTexture) ||
            !AttachColorTarget(m_ssaoBlurFbo, m_ssaoBlurTexture)) {
            DebugLog("OpenGLGraphicsDevice::EnsureSsaoTargets: framebuffer is incomplete.\n");
            return false;
        }

        m_ssaoTargetWidth  = width;
        m_ssaoTargetHeight = height;
        return true;
    }

    bool OpenGLGraphicsDevice::EnsureSsaoPrograms()
    {
        if (m_ssaoProgram != 0 && m_ssaoBlurProgram != 0) {
            return true;
        }
        if (m_ssaoFailed) {
            return false;   // a failed compile is not free; do not retry it every frame
        }
        if (!m_hdc || !m_context || !wglMakeCurrent(m_hdc, m_context)) {
            return false;
        }

        auto getUniformLocation = LoadGlProc<GlGetUniformLocationFn>("glGetUniformLocation");
        if (!getUniformLocation) {
            m_ssaoFailed = true;
            return false;
        }

        // No attribute bindings for either program: the full-screen triangle comes from
        // gl_VertexID, like the native sky and ray-march programs.
        if (m_ssaoProgram == 0) {
            m_ssaoProgram = BuildGlProgram(kOpenGLSsaoVS, kOpenGLSsaoFS, nullptr, 0u, "SSAO");
            if (m_ssaoProgram == 0) {
                m_ssaoFailed = true;
                return false;
            }
            m_ssaoLocCameraPV      = getUniformLocation(m_ssaoProgram, "u_cameraPV");
            m_ssaoLocCameraInvPV   = getUniformLocation(m_ssaoProgram, "u_cameraInvPV");
            m_ssaoLocParams        = getUniformLocation(m_ssaoProgram, "u_ssaoParams");
            m_ssaoLocCameraPos     = getUniformLocation(m_ssaoProgram, "u_cameraPos");
            m_ssaoLocDepthTexture  = getUniformLocation(m_ssaoProgram, "u_depthTexture");
            m_ssaoLocNormalTexture = getUniformLocation(m_ssaoProgram, "u_normalTexture");
        }

        if (m_ssaoBlurProgram == 0) {
            m_ssaoBlurProgram = BuildGlProgram(kOpenGLSsaoVS, kOpenGLSsaoBlurFS, nullptr, 0u, "SSAO blur");
            if (m_ssaoBlurProgram == 0) {
                m_ssaoFailed = true;
                return false;
            }
            m_ssaoBlurLocScreenSize    = getUniformLocation(m_ssaoBlurProgram, "u_screenSize");
            m_ssaoBlurLocSsaoTexture   = getUniformLocation(m_ssaoBlurProgram, "u_ssaoTexture");
            m_ssaoBlurLocDepthTexture  = getUniformLocation(m_ssaoBlurProgram, "u_depthTexture");
            m_ssaoBlurLocNormalTexture = getUniformLocation(m_ssaoBlurProgram, "u_normalTexture");
        }

        return true;
    }

    void OpenGLGraphicsDevice::RenderNativeAmbientOcclusion(const RhiBackendMeshFrameDesc& desc)
    {
        // Cleared first and only set on success: the mesh shaders fall back to runtimeAo = 1.0 when
        // this is false, so a frame that could not regenerate the texture shows no occlusion rather
        // than the occlusion of whatever the camera was looking at when it last succeeded.
        m_ssaoValid        = false;
        m_runtimeAoTexture = 0;

        if (!desc.enableSsao) {
            return;
        }
        if (!m_depthZeroToOne) {
            // Without glClipControl, clip z lands in depth [0.5, 1.0] instead of [0, 1]. Rendering
            // stays correct -- the mapping is still monotonic -- but ReconstructWorldPos in the
            // shader would feed that depth to the inverse view-projection as if it were NDC z and
            // place every sample at the wrong distance, so there is nothing to salvage here.
            static bool reported = false;
            if (!reported) {
                reported = true;
                DebugLog("OpenGLGraphicsDevice: SSAO disabled, glClipControl(GL_ZERO_TO_ONE) is unavailable.\n");
            }
            return;
        }

        const uint32_t width  = static_cast<uint32_t>(desc.renderWidth);
        const uint32_t height = static_cast<uint32_t>(desc.renderHeight);
        if (width == 0 || height == 0) {
            return;
        }
        const bool hasStaticDraws  = desc.draws != nullptr && desc.drawCount > 0;
        const bool hasSkinnedDraws = desc.skinnedDraws != nullptr && desc.skinnedDrawCount > 0;
        if (!hasStaticDraws && !hasSkinnedDraws) {
            return;
        }

        if (!RenderAoPrepass(desc) || !EnsureSsaoPrograms() || !EnsureSsaoTargets(width, height)) {
            return;
        }

        auto bindFramebuffer  = LoadGlProc<GlBindFramebufferFn>("glBindFramebuffer");
        auto useProgram       = LoadGlProc<GlUseProgramFn>("glUseProgram");
        auto uniformMatrix4fv = LoadGlProc<GlUniformMatrix4fvFn>("glUniformMatrix4fv");
        auto uniform4fv       = LoadGlProc<GlUniform4fvFn>("glUniform4fv");
        auto uniform1i        = LoadGlProc<GlUniform1iFn>("glUniform1i");
        auto activeTexture    = LoadGlProc<GlActiveTextureFn>("glActiveTexture");
        if (!bindFramebuffer || !useProgram || !uniformMatrix4fv || !uniform4fv || !uniform1i ||
            !activeTexture) {
            return;
        }

        // Depth off for both stages, for the same reason the sky pass turns it off: a full-screen
        // triangle at z = 0 has no business being depth-tested, and neither target has a depth
        // attachment anyway.
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));

        const float screenSize[4] = {
            static_cast<float>(width),
            static_cast<float>(height),
            1.0f / static_cast<float>(width),
            1.0f / static_cast<float>(height)
        };

        // --- Stage 1: raw SSAO ---
        bindFramebuffer(GL_FRAMEBUFFER, m_ssaoFbo);
        useProgram(m_ssaoProgram);
        // Row-major data uploaded un-transposed: GLSL reads it column-major, which is the transpose,
        // so M * v here reproduces the HLSL shaders' mul(v, M). Same convention as the sky and
        // ray-march programs.
        if (m_ssaoLocCameraPV >= 0) {
            uniformMatrix4fv(m_ssaoLocCameraPV, 1, GL_FALSE, desc.viewProjection);
        }
        if (m_ssaoLocCameraInvPV >= 0) {
            uniformMatrix4fv(m_ssaoLocCameraInvPV, 1, GL_FALSE, desc.invViewProjection);
        }
        if (m_ssaoLocParams >= 0) {
            // The live RenderSettings tuning DX12 pushes through its SSAO constant buffer,
            // carried on the frame description rather than duplicated here as defaults.
            const float params[4] = {
                desc.runtimeAoRadius, desc.runtimeAoBias, desc.runtimeAoIntensity, desc.runtimeAoThickness
            };
            uniform4fv(m_ssaoLocParams, 1, params);
        }
        if (m_ssaoLocCameraPos >= 0) {
            const float cameraPos[4] = {
                desc.cameraPos[0], desc.cameraPos[1], desc.cameraPos[2],
                static_cast<float>(desc.runtimeAoQuality)
            };
            uniform4fv(m_ssaoLocCameraPos, 1, cameraPos);
        }
        activeTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_aoDepthTexture);
        activeTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, m_aoNormalTexture);
        if (m_ssaoLocDepthTexture >= 0) {
            uniform1i(m_ssaoLocDepthTexture, 0);
        }
        if (m_ssaoLocNormalTexture >= 0) {
            uniform1i(m_ssaoLocNormalTexture, 1);
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // --- Stage 2: depth- and normal-aware blur ---
        // The per-pixel kernel rotation in stage 1 leaves fixed-pattern noise; this is the same
        // stage DX12, DX11 and Vulkan all run to resolve it.
        bindFramebuffer(GL_FRAMEBUFFER, m_ssaoBlurFbo);
        useProgram(m_ssaoBlurProgram);
        if (m_ssaoBlurLocScreenSize >= 0) {
            uniform4fv(m_ssaoBlurLocScreenSize, 1, screenSize);
        }
        activeTexture(GL_TEXTURE0 + 2);
        glBindTexture(GL_TEXTURE_2D, m_ssaoTexture);
        if (m_ssaoBlurLocSsaoTexture >= 0) {
            uniform1i(m_ssaoBlurLocSsaoTexture, 2);
        }
        if (m_ssaoBlurLocDepthTexture >= 0) {
            uniform1i(m_ssaoBlurLocDepthTexture, 0);
        }
        if (m_ssaoBlurLocNormalTexture >= 0) {
            uniform1i(m_ssaoBlurLocNormalTexture, 1);
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);

        activeTexture(GL_TEXTURE0 + 2);
        glBindTexture(GL_TEXTURE_2D, 0);
        activeTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, 0);
        activeTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        useProgram(0);
        bindFramebuffer(GL_FRAMEBUFFER, 0);

        // The mesh passes set their own viewport and depth state, but the back buffer is the target
        // again from here on, so leave the depth state as they expect to find it.
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));

        m_runtimeAoTexture = m_ssaoBlurTexture;
        m_ssaoValid        = true;
    }

    void OpenGLGraphicsDevice::ReleaseSsaoResources()
    {
        auto deleteProgram      = LoadGlProc<GlDeleteProgramFn>("glDeleteProgram");
        auto deleteFramebuffers = LoadGlProc<GlDeleteFramebuffersFn>("glDeleteFramebuffers");

        if (deleteProgram) {
            if (m_ssaoProgram != 0) {
                deleteProgram(m_ssaoProgram);
            }
            if (m_ssaoBlurProgram != 0) {
                deleteProgram(m_ssaoBlurProgram);
            }
        }
        m_ssaoProgram              = 0;
        m_ssaoBlurProgram          = 0;
        m_ssaoLocCameraPV          = -1;
        m_ssaoLocCameraInvPV       = -1;
        m_ssaoLocParams            = -1;
        m_ssaoLocCameraPos         = -1;
        m_ssaoLocDepthTexture      = -1;
        m_ssaoLocNormalTexture     = -1;
        m_ssaoBlurLocScreenSize    = -1;
        m_ssaoBlurLocSsaoTexture   = -1;
        m_ssaoBlurLocDepthTexture  = -1;
        m_ssaoBlurLocNormalTexture = -1;

        if (deleteFramebuffers) {
            if (m_ssaoFbo != 0) {
                const GLuint fbo = m_ssaoFbo;
                deleteFramebuffers(1, &fbo);
            }
            if (m_ssaoBlurFbo != 0) {
                const GLuint fbo = m_ssaoBlurFbo;
                deleteFramebuffers(1, &fbo);
            }
        }
        m_ssaoFbo     = 0;
        m_ssaoBlurFbo = 0;

        if (m_ssaoTexture != 0) {
            const GLuint texture = m_ssaoTexture;
            glDeleteTextures(1, &texture);
            m_ssaoTexture = 0;
        }
        if (m_ssaoBlurTexture != 0) {
            const GLuint texture = m_ssaoBlurTexture;
            glDeleteTextures(1, &texture);
            m_ssaoBlurTexture = 0;
        }
        m_runtimeAoTexture = 0;
        m_ssaoTargetWidth  = 0;
        m_ssaoTargetHeight = 0;
        m_ssaoValid        = false;
    }
} // namespace SasamiEngine::Render
#endif
