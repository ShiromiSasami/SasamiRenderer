// OpenGLGraphicsDeviceAoPrepass.cpp
// The depth + world-normal prepass that feeds the native SSAO pass.
//
// DX12 gets both inputs for free: SSAORenderPass reads "SceneDepth" and "GBufferNormal" out of the
// deferred GBuffer. The native OpenGL path is forward and draws straight into the default
// framebuffer, whose depth buffer cannot be bound as a texture, and it produces no normal buffer at
// all. So the opaque geometry is drawn a second time here, into an offscreen depth texture plus an
// encoded world-normal texture, which is the only part of the DX12 pipeline the native path has to
// build from scratch rather than translate.
#include "Renderer/Backends/OpenGL/OpenGLGraphicsDevice.h"
#include "Renderer/Backends/OpenGL/OpenGLProcs.h"
#include "Renderer/Backends/OpenGL/OpenGLProgramBuilder.h"
#include "Renderer/Backends/OpenGL/OpenGLShaderSources.h"

#if RHI_OPENGL

#include "Foundation/Math/MathUtil.h"
#include "Foundation/Tools/DebugOutput.h"
#include "Renderer/Structures/Skeleton.h"

#include <array>

namespace SasamiEngine::Render
{
    bool OpenGLGraphicsDevice::EnsureAoPrepassTargets(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0) {
            return false;
        }
        if (m_aoPrepassFbo != 0 && m_aoTargetWidth == width && m_aoTargetHeight == height) {
            return true;
        }

        auto genFramebuffers      = LoadGlProc<GlGenFramebuffersFn>("glGenFramebuffers");
        auto bindFramebuffer      = LoadGlProc<GlBindFramebufferFn>("glBindFramebuffer");
        auto framebufferTexture2D = LoadGlProc<GlFramebufferTexture2DFn>("glFramebufferTexture2D");
        auto checkFramebufferStatus = LoadGlProc<GlCheckFramebufferStatusFn>("glCheckFramebufferStatus");
        auto drawBuffers          = LoadGlProc<GlDrawBuffersFn>("glDrawBuffers");
        if (!genFramebuffers || !bindFramebuffer || !framebufferTexture2D || !checkFramebufferStatus) {
            DebugLog("OpenGLGraphicsDevice::EnsureAoPrepassTargets: framebuffer entry points are unavailable.\n");
            return false;
        }

        // The textures are reallocated in place on a resize rather than deleted and regenerated:
        // the FBO's attachments name these textures, so keeping the names keeps the attachments.
        if (m_aoNormalTexture == 0) {
            GLuint texture = 0;
            glGenTextures(1, &texture);
            m_aoNormalTexture = texture;
        }
        if (m_aoDepthTexture == 0) {
            GLuint texture = 0;
            glGenTextures(1, &texture);
            m_aoDepthTexture = texture;
        }
        if (m_aoNormalTexture == 0 || m_aoDepthTexture == 0) {
            return false;
        }

        // Point sampling on both: the SSAO shader translates SampleLevel through a point-clamp
        // sampler, and interpolating either a depth or an encoded normal across a silhouette would
        // invent a surface that is in neither of the two it sits between.
        glBindTexture(GL_TEXTURE_2D, m_aoNormalTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA16F,
                     static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height),
                     0,
                     GL_RGBA,
                     GL_HALF_FLOAT,
                     nullptr);

        glBindTexture(GL_TEXTURE_2D, m_aoDepthTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_DEPTH_COMPONENT32F,
                     static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height),
                     0,
                     GL_DEPTH_COMPONENT,
                     GL_FLOAT,
                     nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);

        if (m_aoPrepassFbo == 0) {
            GLuint fbo = 0;
            genFramebuffers(1, &fbo);
            m_aoPrepassFbo = fbo;
        }
        if (m_aoPrepassFbo == 0) {
            return false;
        }

        bindFramebuffer(GL_FRAMEBUFFER, m_aoPrepassFbo);
        framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_aoNormalTexture, 0);
        framebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_aoDepthTexture, 0);
        if (drawBuffers) {
            // Spelled out rather than relying on a non-default framebuffer's implicit
            // COLOR_ATTACHMENT0 draw buffer, so the single GLSL 1.20 gl_FragColor output has an
            // explicit destination.
            const GLenum targets[1] = { GL_COLOR_ATTACHMENT0 };
            drawBuffers(1, targets);
        }
        const GLenum status = checkFramebufferStatus(GL_FRAMEBUFFER);
        bindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            DebugLog("OpenGLGraphicsDevice::EnsureAoPrepassTargets: framebuffer is incomplete.\n");
            return false;
        }

        m_aoTargetWidth  = width;
        m_aoTargetHeight = height;
        return true;
    }

    bool OpenGLGraphicsDevice::EnsureAoPrepassPrograms()
    {
        if (m_aoPrepassProgram != 0 && m_aoPrepassSkinnedProgram != 0) {
            return true;
        }
        if (m_aoPrepassFailed) {
            return false;   // a failed compile is not free; do not retry it every frame
        }
        if (!m_hdc || !m_context || !wglMakeCurrent(m_hdc, m_context)) {
            return false;
        }

        // Indices 0/1/3 only: the prepass needs position, normal and UV, and shares the mesh path's
        // vertex layout, where index 2 is the vertex colour it has no use for.
        const std::array<GlAttributeBinding, 3> staticAttributes = { {
            { 0u, "a_position" },
            { 1u, "a_normal" },
            { 3u, "a_uv" },
        } };
        const std::array<GlAttributeBinding, 5> skinnedAttributes = { {
            { 0u, "a_position" },
            { 1u, "a_normal" },
            { 3u, "a_uv" },
            { 4u, "a_boneIndices" },
            { 5u, "a_boneWeights" },
        } };

        auto getUniformLocation = LoadGlProc<GlGetUniformLocationFn>("glGetUniformLocation");
        if (!getUniformLocation) {
            m_aoPrepassFailed = true;
            return false;
        }

        if (m_aoPrepassProgram == 0) {
            m_aoPrepassProgram = BuildGlProgram(kOpenGLAoPrepassVS,
                                                kOpenGLAoPrepassFS,
                                                staticAttributes.data(),
                                                staticAttributes.size(),
                                                "AO prepass (static)");
            if (m_aoPrepassProgram == 0) {
                m_aoPrepassFailed = true;
                return false;
            }
            m_aoPrepassMvpLocation       = getUniformLocation(m_aoPrepassProgram, "u_modelViewProjection");
            m_aoPrepassModelLocation     = getUniformLocation(m_aoPrepassProgram, "u_model");
            m_aoPrepassAlbedoLocation    = getUniformLocation(m_aoPrepassProgram, "u_albedoTexture");
            m_aoPrepassHasAlbedoLocation = getUniformLocation(m_aoPrepassProgram, "u_hasAlbedoTexture");
        }

        if (m_aoPrepassSkinnedProgram == 0) {
            m_aoPrepassSkinnedProgram = BuildGlProgram(kOpenGLAoPrepassSkinnedVS,
                                                       kOpenGLAoPrepassFS,
                                                       skinnedAttributes.data(),
                                                       skinnedAttributes.size(),
                                                       "AO prepass (skinned)");
            if (m_aoPrepassSkinnedProgram == 0) {
                m_aoPrepassFailed = true;
                return false;
            }
            m_aoPrepassSkinnedMvpLocation          = getUniformLocation(m_aoPrepassSkinnedProgram, "u_modelViewProjection");
            m_aoPrepassSkinnedModelLocation        = getUniformLocation(m_aoPrepassSkinnedProgram, "u_model");
            m_aoPrepassSkinnedAlbedoLocation       = getUniformLocation(m_aoPrepassSkinnedProgram, "u_albedoTexture");
            m_aoPrepassSkinnedHasAlbedoLocation    = getUniformLocation(m_aoPrepassSkinnedProgram, "u_hasAlbedoTexture");
            m_aoPrepassSkinnedBoneMatricesLocation = getUniformLocation(m_aoPrepassSkinnedProgram, "u_boneMatrices[0]");
        }

        return true;
    }

    bool OpenGLGraphicsDevice::RenderAoPrepass(const RhiBackendMeshFrameDesc& desc)
    {
        const uint32_t width  = static_cast<uint32_t>(desc.renderWidth);
        const uint32_t height = static_cast<uint32_t>(desc.renderHeight);
        if (!EnsureAoPrepassPrograms() || !EnsureAoPrepassTargets(width, height)) {
            return false;
        }

        auto bindFramebuffer          = LoadGlProc<GlBindFramebufferFn>("glBindFramebuffer");
        auto useProgram               = LoadGlProc<GlUseProgramFn>("glUseProgram");
        auto bindBuffer               = LoadGlProc<GlBindBufferFn>("glBindBuffer");
        auto enableVertexAttribArray  = LoadGlProc<GlEnableVertexAttribArrayFn>("glEnableVertexAttribArray");
        auto disableVertexAttribArray = LoadGlProc<GlDisableVertexAttribArrayFn>("glDisableVertexAttribArray");
        auto vertexAttribPointer      = LoadGlProc<GlVertexAttribPointerFn>("glVertexAttribPointer");
        auto uniformMatrix4fv         = LoadGlProc<GlUniformMatrix4fvFn>("glUniformMatrix4fv");
        auto uniform1i                = LoadGlProc<GlUniform1iFn>("glUniform1i");
        auto activeTexture            = LoadGlProc<GlActiveTextureFn>("glActiveTexture");
        if (!bindFramebuffer || !useProgram || !bindBuffer || !enableVertexAttribArray ||
            !disableVertexAttribArray || !vertexAttribPointer || !uniformMatrix4fv || !uniform1i) {
            return false;
        }

        bindFramebuffer(GL_FRAMEBUFFER, m_aoPrepassFbo);
        glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        // Reverse-Z, matching the mesh passes: clear depth to the far plane of 0.0 and keep the
        // nearer (greater) fragment. The colour clear value is arbitrary -- the SSAO shader rejects
        // every pixel whose depth is still 0.0 before it ever decodes a normal.
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClearDepth(0.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GEQUAL);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);

        if (activeTexture) {
            activeTexture(GL_TEXTURE0);
        }

        // Only the albedo alpha is read, and only to reproduce the mesh shader's cut-out clip, so
        // one texture unit is enough for both programs.
        const auto bindAlbedo = [&](uint64_t albedoSrv, int samplerLocation, int hasAlbedoLocation) {
            GLuint albedoTexture = 0;
            GLenum albedoTarget  = GL_TEXTURE_2D;
            const auto textureIt = m_rhiTextureViews.find(albedoSrv);
            if (textureIt != m_rhiTextureViews.end()) {
                albedoTexture = textureIt->second.texture;
                albedoTarget  = textureIt->second.target;
            }
            glBindTexture(albedoTarget, albedoTexture);
            if (samplerLocation >= 0) {
                uniform1i(samplerLocation, 0);
            }
            if (hasAlbedoLocation >= 0) {
                uniform1i(hasAlbedoLocation, albedoTexture != 0 ? 1 : 0);
            }
        };

        // Only the three attributes the prepass declares are enabled; the vertex colour at index 2
        // is left alone because neither prepass program consumes it.
        const auto bindMeshAttributes = [&](uint32_t vertexStride) {
            enableVertexAttribArray(0);
            enableVertexAttribArray(1);
            enableVertexAttribArray(3);
            const GLsizei stride = static_cast<GLsizei>(vertexStride);
            vertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(0));
            vertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(12));
            vertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(40));
        };

        const auto submit = [&](uint64_t indexBufferHandle,
                                uint32_t indexCount,
                                uint32_t vertexCount,
                                bool     index32Bit) {
            const auto ibIt = m_rhiBuffers.find(indexBufferHandle);
            if (indexCount > 0 && ibIt != m_rhiBuffers.end()) {
                bindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibIt->second);
                glDrawElements(GL_TRIANGLES,
                               static_cast<GLsizei>(indexCount),
                               index32Bit ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                               nullptr);
            } else if (vertexCount > 0) {
                bindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertexCount));
            }
        };

        // Opaque geometry only, in both loops. A transparent surface writes no depth in the mesh
        // passes either, and recording one here would make it occlude whatever is visible through
        // it -- the same reason the DX12 GBuffer carries opaque draws alone.
        if (desc.draws != nullptr && desc.drawCount > 0) {
            useProgram(m_aoPrepassProgram);
            for (uint32_t i = 0; i < desc.drawCount; ++i) {
                const RhiBackendMeshDrawDesc& draw = desc.draws[i];
                if (draw.transparent) {
                    continue;
                }
                const auto vbIt = m_rhiBuffers.find(draw.vertexBufferHandle);
                if (vbIt == m_rhiBuffers.end()) {
                    continue;
                }

                float mvp[16] = {};
                Math::Mul4x4(draw.model, desc.viewProjection, mvp);
                uniformMatrix4fv(m_aoPrepassMvpLocation, 1, GL_FALSE, mvp);
                uniformMatrix4fv(m_aoPrepassModelLocation, 1, GL_FALSE, draw.model);
                bindAlbedo(draw.albedoSrv, m_aoPrepassAlbedoLocation, m_aoPrepassHasAlbedoLocation);

                bindBuffer(GL_ARRAY_BUFFER, vbIt->second);
                bindMeshAttributes(draw.vertexStride);
                submit(draw.indexBufferHandle, draw.indexCount, draw.vertexCount, draw.index32Bit);
            }
        }

        if (desc.skinnedDraws != nullptr && desc.skinnedDrawCount > 0) {
            useProgram(m_aoPrepassSkinnedProgram);
            for (uint32_t i = 0; i < desc.skinnedDrawCount; ++i) {
                const RhiBackendSkinnedMeshDrawDesc& draw = desc.skinnedDraws[i];
                if (draw.transparent || !draw.boneMatrices) {
                    continue;
                }
                const auto vbIt = m_rhiBuffers.find(draw.vertexBufferHandle);
                if (vbIt == m_rhiBuffers.end()) {
                    continue;
                }

                float mvp[16] = {};
                Math::Mul4x4(draw.model, desc.viewProjection, mvp);
                uniformMatrix4fv(m_aoPrepassSkinnedMvpLocation, 1, GL_FALSE, mvp);
                uniformMatrix4fv(m_aoPrepassSkinnedModelLocation, 1, GL_FALSE, draw.model);
                uniformMatrix4fv(m_aoPrepassSkinnedBoneMatricesLocation,
                                 static_cast<GLsizei>(Skeleton::kMaxBones),
                                 GL_FALSE,
                                 draw.boneMatrices);
                bindAlbedo(draw.albedoSrv,
                           m_aoPrepassSkinnedAlbedoLocation,
                           m_aoPrepassSkinnedHasAlbedoLocation);

                bindBuffer(GL_ARRAY_BUFFER, vbIt->second);
                bindMeshAttributes(draw.vertexStride);
                enableVertexAttribArray(4);
                enableVertexAttribArray(5);
                const GLsizei stride = static_cast<GLsizei>(draw.vertexStride);
                // Bone indices are unsigned bytes uploaded unnormalized, the same convention the
                // skinned mesh path uses, so the shader rounds them back to integers.
                vertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride, reinterpret_cast<const void*>(48));
                vertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(52));
                submit(draw.indexBufferHandle, draw.indexCount, draw.vertexCount, draw.index32Bit);
            }
            disableVertexAttribArray(5);
            disableVertexAttribArray(4);
        }

        disableVertexAttribArray(3);
        disableVertexAttribArray(1);
        disableVertexAttribArray(0);
        bindBuffer(GL_ARRAY_BUFFER, 0);
        bindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        useProgram(0);
        bindFramebuffer(GL_FRAMEBUFFER, 0);
        return true;
    }

    void OpenGLGraphicsDevice::ReleaseAoPrepassResources()
    {
        auto deleteProgram      = LoadGlProc<GlDeleteProgramFn>("glDeleteProgram");
        auto deleteFramebuffers = LoadGlProc<GlDeleteFramebuffersFn>("glDeleteFramebuffers");

        if (deleteProgram) {
            if (m_aoPrepassProgram != 0) {
                deleteProgram(m_aoPrepassProgram);
            }
            if (m_aoPrepassSkinnedProgram != 0) {
                deleteProgram(m_aoPrepassSkinnedProgram);
            }
        }
        m_aoPrepassProgram        = 0;
        m_aoPrepassSkinnedProgram = 0;
        m_aoPrepassMvpLocation       = -1;
        m_aoPrepassModelLocation     = -1;
        m_aoPrepassAlbedoLocation    = -1;
        m_aoPrepassHasAlbedoLocation = -1;
        m_aoPrepassSkinnedMvpLocation          = -1;
        m_aoPrepassSkinnedModelLocation        = -1;
        m_aoPrepassSkinnedAlbedoLocation       = -1;
        m_aoPrepassSkinnedHasAlbedoLocation    = -1;
        m_aoPrepassSkinnedBoneMatricesLocation = -1;

        if (deleteFramebuffers && m_aoPrepassFbo != 0) {
            const GLuint fbo = m_aoPrepassFbo;
            deleteFramebuffers(1, &fbo);
        }
        m_aoPrepassFbo = 0;

        if (m_aoNormalTexture != 0) {
            const GLuint texture = m_aoNormalTexture;
            glDeleteTextures(1, &texture);
            m_aoNormalTexture = 0;
        }
        if (m_aoDepthTexture != 0) {
            const GLuint texture = m_aoDepthTexture;
            glDeleteTextures(1, &texture);
            m_aoDepthTexture = 0;
        }
        m_aoTargetWidth  = 0;
        m_aoTargetHeight = 0;
    }
} // namespace SasamiEngine::Render
#endif
