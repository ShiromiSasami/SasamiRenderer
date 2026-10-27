// OpenGLGraphicsDeviceMesh.cpp
// The native mesh drawing path, static and skinned: program and buffer setup, then the draw
// loops that consume RhiBackendMeshFrameDesc.
//
// Both variants live together because they are the same path -- the skinned one differs only by
// its bone-matrix uniforms and a second vertex program.
#include "Renderer/Backends/OpenGL/OpenGLGraphicsDevice.h"
#include "Renderer/Backends/OpenGL/OpenGLProcs.h"
#include "Renderer/Backends/OpenGL/OpenGLShaderSources.h"

#if RHI_OPENGL

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Structures/Skeleton.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace SasamiEngine::Render
{
    bool OpenGLGraphicsDevice::EnsureMeshFrameResources()
    {
        if (m_meshProgram != 0) {
            return true;
        }
        if (!m_hdc || !m_context || !wglMakeCurrent(m_hdc, m_context)) {
            return false;
        }

        auto createShader = LoadGlProc<GlCreateShaderFn>("glCreateShader");
        auto shaderSource = LoadGlProc<GlShaderSourceFn>("glShaderSource");
        auto compileShader = LoadGlProc<GlCompileShaderFn>("glCompileShader");
        auto getShaderiv = LoadGlProc<GlGetShaderivFn>("glGetShaderiv");
        auto getShaderInfoLog = LoadGlProc<GlGetShaderInfoLogFn>("glGetShaderInfoLog");
        auto deleteShader = LoadGlProc<GlDeleteShaderFn>("glDeleteShader");
        auto createProgram = LoadGlProc<GlCreateProgramFn>("glCreateProgram");
        auto attachShader = LoadGlProc<GlAttachShaderFn>("glAttachShader");
        auto bindAttribLocation = LoadGlProc<GlBindAttribLocationFn>("glBindAttribLocation");
        auto linkProgram = LoadGlProc<GlLinkProgramFn>("glLinkProgram");
        auto getProgramiv = LoadGlProc<GlGetProgramivFn>("glGetProgramiv");
        auto getProgramInfoLog = LoadGlProc<GlGetProgramInfoLogFn>("glGetProgramInfoLog");
        auto getUniformLocation = LoadGlProc<GlGetUniformLocationFn>("glGetUniformLocation");
        if (!createShader || !shaderSource || !compileShader || !getShaderiv || !deleteShader ||
            !createProgram || !attachShader || !bindAttribLocation || !linkProgram ||
            !getProgramiv || !getUniformLocation) {
            DebugLog("OpenGLGraphicsDevice::EnsureMeshFrameResources: required GL shader functions are unavailable.\n");
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
                    char log[1024] = {};
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

        const GLuint vs = compile(GL_VERTEX_SHADER, kOpenGLNativeMeshVS);
        const GLuint ps = compile(GL_FRAGMENT_SHADER, kOpenGLNativeMeshPS);
        if (vs == 0 || ps == 0) {
            if (vs != 0) deleteShader(vs);
            if (ps != 0) deleteShader(ps);
            return false;
        }

        const GLuint program = createProgram();
        attachShader(program, vs);
        attachShader(program, ps);
        bindAttribLocation(program, 0, "a_position");
        bindAttribLocation(program, 1, "a_normal");
        bindAttribLocation(program, 2, "a_color");
        bindAttribLocation(program, 3, "a_uv");
        linkProgram(program);
        deleteShader(ps);
        deleteShader(vs);

        GLint linked = GL_FALSE;
        getProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked == GL_FALSE) {
            if (getProgramInfoLog) {
                char log[1024] = {};
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

        m_meshProgram = program;
        m_meshMvpLocation = getUniformLocation(program, "u_modelViewProjection");
        m_meshBaseColorLocation = getUniformLocation(program, "u_baseColor");
        m_meshLightDirIntensityLocation = getUniformLocation(program, "u_lightDirIntensity");
        m_meshLightColorLocation = getUniformLocation(program, "u_lightColor");
        m_meshEmissiveRoughnessLocation = getUniformLocation(program, "u_emissiveRoughness");
        m_meshAlbedoTextureLocation = getUniformLocation(program, "u_albedoTexture");
        m_meshHasAlbedoTextureLocation = getUniformLocation(program, "u_hasAlbedoTexture");
        m_meshOcclusionTextureLocation = getUniformLocation(program, "u_occlusionTexture");
        m_meshHasOcclusionTextureLocation = getUniformLocation(program, "u_hasOcclusionTexture");
        m_meshAmbientCubeLocation = getUniformLocation(program, "u_ambientCube");
        m_meshHasAmbientCubeLocation = getUniformLocation(program, "u_hasAmbientCube");
        m_meshRuntimeAoTextureLocation = getUniformLocation(program, "u_runtimeAoTexture");
        m_meshHasRuntimeAoTextureLocation = getUniformLocation(program, "u_hasRuntimeAoTexture");
        m_meshRenderSizeLocation = getUniformLocation(program, "u_renderSize");
        m_meshExposureLocation = getUniformLocation(program, "u_exposure");
        m_meshModelLocation = getUniformLocation(program, "u_model");
        m_meshCameraPosLocation = getUniformLocation(program, "u_cameraPos");
        m_meshMetallicLocation = getUniformLocation(program, "u_metallic");
        m_meshEnableIblLocation = getUniformLocation(program, "u_enableIbl");
        return true;
    }

    bool OpenGLGraphicsDevice::EnsureSkinnedMeshFrameResources()
    {
        if (m_skinnedMeshProgram != 0) {
            return true;
        }
        if (!m_hdc || !m_context || !wglMakeCurrent(m_hdc, m_context)) {
            return false;
        }

        auto createShader = LoadGlProc<GlCreateShaderFn>("glCreateShader");
        auto shaderSource = LoadGlProc<GlShaderSourceFn>("glShaderSource");
        auto compileShader = LoadGlProc<GlCompileShaderFn>("glCompileShader");
        auto getShaderiv = LoadGlProc<GlGetShaderivFn>("glGetShaderiv");
        auto getShaderInfoLog = LoadGlProc<GlGetShaderInfoLogFn>("glGetShaderInfoLog");
        auto deleteShader = LoadGlProc<GlDeleteShaderFn>("glDeleteShader");
        auto createProgram = LoadGlProc<GlCreateProgramFn>("glCreateProgram");
        auto attachShader = LoadGlProc<GlAttachShaderFn>("glAttachShader");
        auto bindAttribLocation = LoadGlProc<GlBindAttribLocationFn>("glBindAttribLocation");
        auto linkProgram = LoadGlProc<GlLinkProgramFn>("glLinkProgram");
        auto getProgramiv = LoadGlProc<GlGetProgramivFn>("glGetProgramiv");
        auto getProgramInfoLog = LoadGlProc<GlGetProgramInfoLogFn>("glGetProgramInfoLog");
        auto getUniformLocation = LoadGlProc<GlGetUniformLocationFn>("glGetUniformLocation");
        if (!createShader || !shaderSource || !compileShader || !getShaderiv || !deleteShader ||
            !createProgram || !attachShader || !bindAttribLocation || !linkProgram ||
            !getProgramiv || !getUniformLocation) {
            DebugLog("OpenGLGraphicsDevice::EnsureSkinnedMeshFrameResources: required GL shader functions are unavailable.\n");
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
                    char log[1024] = {};
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

        const GLuint vs = compile(GL_VERTEX_SHADER, kOpenGLNativeSkinnedMeshVS);
        const GLuint ps = compile(GL_FRAGMENT_SHADER, kOpenGLNativeMeshPS);
        if (vs == 0 || ps == 0) {
            if (vs != 0) deleteShader(vs);
            if (ps != 0) deleteShader(ps);
            return false;
        }

        const GLuint program = createProgram();
        attachShader(program, vs);
        attachShader(program, ps);
        bindAttribLocation(program, 0, "a_position");
        bindAttribLocation(program, 1, "a_normal");
        bindAttribLocation(program, 2, "a_color");
        bindAttribLocation(program, 3, "a_uv");
        bindAttribLocation(program, 4, "a_boneIndices");
        bindAttribLocation(program, 5, "a_boneWeights");
        linkProgram(program);
        deleteShader(ps);
        deleteShader(vs);

        GLint linked = GL_FALSE;
        getProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked == GL_FALSE) {
            if (getProgramInfoLog) {
                char log[1024] = {};
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

        m_skinnedMeshProgram = program;
        m_skinnedMeshMvpLocation = getUniformLocation(program, "u_modelViewProjection");
        m_skinnedMeshBaseColorLocation = getUniformLocation(program, "u_baseColor");
        m_skinnedMeshLightDirIntensityLocation = getUniformLocation(program, "u_lightDirIntensity");
        m_skinnedMeshLightColorLocation = getUniformLocation(program, "u_lightColor");
        m_skinnedMeshEmissiveRoughnessLocation = getUniformLocation(program, "u_emissiveRoughness");
        m_skinnedMeshAlbedoTextureLocation = getUniformLocation(program, "u_albedoTexture");
        m_skinnedMeshHasAlbedoTextureLocation = getUniformLocation(program, "u_hasAlbedoTexture");
        m_skinnedMeshOcclusionTextureLocation = getUniformLocation(program, "u_occlusionTexture");
        m_skinnedMeshHasOcclusionTextureLocation = getUniformLocation(program, "u_hasOcclusionTexture");
        m_skinnedMeshAmbientCubeLocation = getUniformLocation(program, "u_ambientCube");
        m_skinnedMeshHasAmbientCubeLocation = getUniformLocation(program, "u_hasAmbientCube");
        m_skinnedMeshBoneMatricesLocation = getUniformLocation(program, "u_boneMatrices[0]");
        m_skinnedMeshRuntimeAoTextureLocation = getUniformLocation(program, "u_runtimeAoTexture");
        m_skinnedMeshHasRuntimeAoTextureLocation = getUniformLocation(program, "u_hasRuntimeAoTexture");
        m_skinnedMeshRenderSizeLocation = getUniformLocation(program, "u_renderSize");
        m_skinnedMeshExposureLocation = getUniformLocation(program, "u_exposure");
        m_skinnedMeshModelLocation = getUniformLocation(program, "u_model");
        m_skinnedMeshCameraPosLocation = getUniformLocation(program, "u_cameraPos");
        m_skinnedMeshMetallicLocation = getUniformLocation(program, "u_metallic");
        m_skinnedMeshEnableIblLocation = getUniformLocation(program, "u_enableIbl");
        return true;
    }


    bool OpenGLGraphicsDevice::RenderMeshFrame(const RhiBackendMeshFrameDesc& desc)
    {
        // Sky first: it is the background, so every mesh drawn below covers it.
        RenderNativeSky(desc);

        if (!desc.draws || desc.drawCount == 0 || !EnsureMeshFrameResources()) {
            return false;
        }

        auto useProgram = LoadGlProc<GlUseProgramFn>("glUseProgram");
        auto bindBuffer = LoadGlProc<GlBindBufferFn>("glBindBuffer");
        auto enableVertexAttribArray = LoadGlProc<GlEnableVertexAttribArrayFn>("glEnableVertexAttribArray");
        auto disableVertexAttribArray = LoadGlProc<GlDisableVertexAttribArrayFn>("glDisableVertexAttribArray");
        auto vertexAttribPointer = LoadGlProc<GlVertexAttribPointerFn>("glVertexAttribPointer");
        auto uniformMatrix4fv = LoadGlProc<GlUniformMatrix4fvFn>("glUniformMatrix4fv");
        auto uniform4fv = LoadGlProc<GlUniform4fvFn>("glUniform4fv");
        auto uniform1i = LoadGlProc<GlUniform1iFn>("glUniform1i");
        auto uniform2f = LoadGlProc<GlUniform2fFn>("glUniform2f");
        auto uniform1f = LoadGlProc<GlUniform1fFn>("glUniform1f");
        auto uniform3fv = LoadGlProc<GlUniform3fvFn>("glUniform3fv");
        auto activeTexture = LoadGlProc<GlActiveTextureFn>("glActiveTexture");
        if (!useProgram || !bindBuffer || !enableVertexAttribArray || !disableVertexAttribArray ||
            !vertexAttribPointer || !uniformMatrix4fv || !uniform4fv || !uniform1i) {
            return false;
        }

        glViewport(0, 0, static_cast<GLsizei>(desc.renderWidth), static_cast<GLsizei>(desc.renderHeight));
        glEnable(GL_DEPTH_TEST);
        // reverse-Z: 近が1.0なので奥ではなく手前が勝つようにGEQUAL
        glDepthFunc(GL_GEQUAL);
        useProgram(m_meshProgram);
        if (activeTexture) {
            activeTexture(GL_TEXTURE0);
        }
        uniform1i(m_meshAlbedoTextureLocation, 0);
        // Occlusion lives on a separate texture unit so it doesn't clobber the albedo binding
        // above; the sampler-to-unit mapping only needs to be set once per program use.
        uniform1i(m_meshOcclusionTextureLocation, 1);
        // Ambient cube gets its own unit (2) for the same reason occlusion does not share unit 0.
        uniform1i(m_meshAmbientCubeLocation, 2);

        // desc.ambientCubeSrv is a per-frame descriptor, not per-draw (unlike albedoSrv/
        // occlusionSrv above), so it is resolved and bound once here instead of inside drawOne.
        GLenum ambientCubeTarget = GL_TEXTURE_CUBE_MAP;
        GLuint ambientCubeTexture = 0;
        if (desc.ambientCubeSrv != 0) {
            const auto ambientIt = m_rhiTextureViews.find(desc.ambientCubeSrv);
            if (ambientIt != m_rhiTextureViews.end()) {
                ambientCubeTexture = ambientIt->second.texture;
                ambientCubeTarget = ambientIt->second.target;
            }
        }
        if (activeTexture) {
            activeTexture(GL_TEXTURE0 + 2);
        }
        glBindTexture(ambientCubeTarget, ambientCubeTexture);
        // Written unconditionally, including the 0 case, rather than relying on GL's default
        // uniform value of 0: a prior instance of that assumption here was flagged as an implicit
        // dependency and had to be made explicit.
        uniform1i(m_meshHasAmbientCubeLocation, ambientCubeTexture != 0 ? 1 : 0);

        // Screen-space ambient occlusion from the native SSAO pass, on texture unit 3 (0 albedo,
        // 1 occlusion, 2 ambient cube). Per-frame rather than per-draw, so it is bound once here
        // like the ambient cube above. m_ssaoValid is false on any frame
        // RenderNativeAmbientOcclusion skipped, and u_hasRuntimeAoTexture = 0 then makes the
        // shader fall back to runtimeAo = 1.0 instead of reading a stale buffer.
        const GLuint runtimeAoTexture = (m_ssaoValid && m_runtimeAoTexture != 0) ? m_runtimeAoTexture : 0;
        if (activeTexture) {
            activeTexture(GL_TEXTURE0 + 3);
        }
        glBindTexture(GL_TEXTURE_2D, runtimeAoTexture);
        uniform1i(m_meshRuntimeAoTextureLocation, 3);
        uniform1i(m_meshHasRuntimeAoTextureLocation, runtimeAoTexture != 0 ? 1 : 0);
        // The fragment shader has no screen UV of its own, so it divides gl_FragCoord by this.
        if (uniform2f && m_meshRenderSizeLocation >= 0) {
            uniform2f(m_meshRenderSizeLocation,
                      desc.renderWidth > 0.0f ? desc.renderWidth : 1.0f,
                      desc.renderHeight > 0.0f ? desc.renderHeight : 1.0f);
        }
        if (uniform1f && m_meshExposureLocation >= 0) {
            uniform1f(m_meshExposureLocation, desc.exposure);
        }
        if (uniform3fv && m_meshCameraPosLocation >= 0) {
            uniform3fv(m_meshCameraPosLocation, 1, desc.cameraPos);
        }
        if (uniform1f && m_meshEnableIblLocation >= 0) {
            uniform1f(m_meshEnableIblLocation, desc.enableIbl ? 1.0f : 0.0f);
        }
        if (activeTexture) {
            activeTexture(GL_TEXTURE0);
        }

        // Per-draw submission logic is shared by the opaque and transparent passes below;
        // only the GL blend/depth-mask state and submission order differ between the two.
        auto drawOne = [&](const RhiBackendMeshDrawDesc& draw) {
            const auto vbIt = m_rhiBuffers.find(draw.vertexBufferHandle);
            if (vbIt == m_rhiBuffers.end()) {
                return;
            }

            float mvp[16] = {};
            Math::Mul4x4(draw.model, desc.viewProjection, mvp);
            uniformMatrix4fv(m_meshMvpLocation, 1, GL_FALSE, mvp);
            if (m_meshModelLocation >= 0) {
                uniformMatrix4fv(m_meshModelLocation, 1, GL_FALSE, draw.model);
            }
            if (uniform1f && m_meshMetallicLocation >= 0) {
                uniform1f(m_meshMetallicLocation, draw.metallic);
            }
            uniform4fv(m_meshBaseColorLocation, 1, draw.baseColor);

            const float lightDirIntensity[4] = {
                desc.sunDir[0], desc.sunDir[1], desc.sunDir[2], desc.sunIntensity
            };
            const float lightColor[4] = {
                desc.sunColor[0], desc.sunColor[1], desc.sunColor[2], 1.0f
            };
            const float emissiveRoughness[4] = {
                draw.emissive[0], draw.emissive[1], draw.emissive[2], draw.roughness
            };
            uniform4fv(m_meshLightDirIntensityLocation, 1, lightDirIntensity);
            uniform4fv(m_meshLightColorLocation, 1, lightColor);
            uniform4fv(m_meshEmissiveRoughnessLocation, 1, emissiveRoughness);

            GLuint albedoTexture = 0;
            GLenum albedoTarget = GL_TEXTURE_2D;
            const auto textureIt = m_rhiTextureViews.find(draw.albedoSrv);
            if (textureIt != m_rhiTextureViews.end()) {
                albedoTexture = textureIt->second.texture;
                albedoTarget = textureIt->second.target;
            }
            glBindTexture(albedoTarget, albedoTexture);
            uniform1i(m_meshHasAlbedoTextureLocation, albedoTexture != 0 ? 1 : 0);

            // draw.occlusionSrv is not always a pure AO texture: the raster material layout has
            // a single texture slot, so a non-ORM material reuses this slot for metallic-roughness
            // instead. Only treat it as AO when occlusionSlotHasAo says the R channel really is
            // occlusion (mirrors the DX11/Vulkan native paths' gating). Otherwise leave the
            // occlusion unit unbound and tell the shader via u_hasOcclusionTexture=0 to fall back
            // to ao=1.0, matching pre-AO behavior exactly.
            GLuint occlusionTexture = 0;
            GLenum occlusionTarget = GL_TEXTURE_2D;
            if (draw.occlusionSlotHasAo && draw.occlusionSrv != 0) {
                const auto occlusionIt = m_rhiTextureViews.find(draw.occlusionSrv);
                if (occlusionIt != m_rhiTextureViews.end()) {
                    occlusionTexture = occlusionIt->second.texture;
                    occlusionTarget = occlusionIt->second.target;
                }
            }
            if (activeTexture) {
                activeTexture(GL_TEXTURE0 + 1);
            }
            glBindTexture(occlusionTarget, occlusionTexture);
            uniform1i(m_meshHasOcclusionTextureLocation, occlusionTexture != 0 ? 1 : 0);
            if (activeTexture) {
                activeTexture(GL_TEXTURE0);
            }

            bindBuffer(GL_ARRAY_BUFFER, vbIt->second);
            enableVertexAttribArray(0);
            enableVertexAttribArray(1);
            enableVertexAttribArray(2);
            enableVertexAttribArray(3);
            vertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(draw.vertexStride), reinterpret_cast<const void*>(0));
            vertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(draw.vertexStride), reinterpret_cast<const void*>(12));
            vertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(draw.vertexStride), reinterpret_cast<const void*>(24));
            vertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(draw.vertexStride), reinterpret_cast<const void*>(40));

            const auto ibIt = m_rhiBuffers.find(draw.indexBufferHandle);
            if (draw.indexCount > 0 && ibIt != m_rhiBuffers.end()) {
                bindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibIt->second);
                glDrawElements(GL_TRIANGLES,
                               static_cast<GLsizei>(draw.indexCount),
                               draw.index32Bit ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                               nullptr);
            } else if (draw.vertexCount > 0) {
                bindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(draw.vertexCount));
            }
        };

        // Reuse member scratch buffers across frames instead of reallocating a std::vector
        // per call; only clear()+refill so capacity is amortized once steady state is reached.
        m_meshOpaqueDrawOrder.clear();
        m_meshTransparentDrawOrder.clear();
        for (uint32_t i = 0; i < desc.drawCount; ++i) {
            if (desc.draws[i].transparent) {
                m_meshTransparentDrawOrder.push_back(i);
            } else {
                m_meshOpaqueDrawOrder.push_back(i);
            }
        }

        // Far-to-near sort key: squared distance from the camera to the draw's world-space
        // translation. The renderer's model matrices are row-vector convention (VS does
        // mul(float4(pos,1.0), u_model), same layout as the DX11 backend's draw.model), so the
        // translation lives in the last row: draw.model[12], [13], [14].
        std::sort(m_meshTransparentDrawOrder.begin(), m_meshTransparentDrawOrder.end(),
                  [&](uint32_t lhs, uint32_t rhs) {
                      const auto distanceSq = [&](uint32_t index) {
                          const float* model = desc.draws[index].model;
                          const float dx = model[12] - desc.cameraPos[0];
                          const float dy = model[13] - desc.cameraPos[1];
                          const float dz = model[14] - desc.cameraPos[2];
                          return dx * dx + dy * dy + dz * dz;
                      };
                      return distanceSq(lhs) > distanceSq(rhs);
                  });

        // Pass 1: opaque, submitted front-to-back in the caller's order. Blending was
        // previously left enabled for opaque draws unconditionally, which is incorrect and is
        // fixed here.
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        for (uint32_t index : m_meshOpaqueDrawOrder) {
            drawOne(desc.draws[index]);
        }

        // Pass 2: transparent, far-to-near with depth writes disabled so overlapping
        // transparent surfaces don't occlude geometry behind them via the depth buffer, while
        // depth testing stays on so opaque geometry still occludes transparent draws correctly.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        for (uint32_t index : m_meshTransparentDrawOrder) {
            drawOne(desc.draws[index]);
        }

        // Restore depth writes so subsequent passes (e.g. UI overlay) are not affected by the
        // transparent pass leaving depth writes disabled.
        glDepthMask(GL_TRUE);

        disableVertexAttribArray(3);
        disableVertexAttribArray(2);
        disableVertexAttribArray(1);
        disableVertexAttribArray(0);
        bindBuffer(GL_ARRAY_BUFFER, 0);
        bindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        // drawOne leaves unit 1 (occlusion) bound from its last iteration; unbind it too so this
        // pass doesn't leave a stray texture bound on a unit later code doesn't expect.
        if (activeTexture) {
            activeTexture(GL_TEXTURE0 + 1);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        // Unbind unit 2 (ambient cube), bound once above outside the draw loop, for the same
        // reason.
        if (activeTexture) {
            activeTexture(GL_TEXTURE0 + 2);
        }
        glBindTexture(ambientCubeTarget, 0);
        // Unbind unit 3 (screen-space AO) for the same reason: the SSAO target is reallocated on
        // a resize, and a stale binding here would outlive it.
        if (activeTexture) {
            activeTexture(GL_TEXTURE0 + 3);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        if (activeTexture) {
            activeTexture(GL_TEXTURE0);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        useProgram(0);
        return true;
    }

    bool OpenGLGraphicsDevice::RenderSkinnedMeshFrame(const RhiBackendMeshFrameDesc& desc)
    {
        if (!desc.skinnedDraws || desc.skinnedDrawCount == 0 || !EnsureSkinnedMeshFrameResources()) {
            return false;
        }

        auto useProgram = LoadGlProc<GlUseProgramFn>("glUseProgram");
        auto bindBuffer = LoadGlProc<GlBindBufferFn>("glBindBuffer");
        auto enableVertexAttribArray = LoadGlProc<GlEnableVertexAttribArrayFn>("glEnableVertexAttribArray");
        auto disableVertexAttribArray = LoadGlProc<GlDisableVertexAttribArrayFn>("glDisableVertexAttribArray");
        auto vertexAttribPointer = LoadGlProc<GlVertexAttribPointerFn>("glVertexAttribPointer");
        auto uniformMatrix4fv = LoadGlProc<GlUniformMatrix4fvFn>("glUniformMatrix4fv");
        auto uniform4fv = LoadGlProc<GlUniform4fvFn>("glUniform4fv");
        auto uniform1i = LoadGlProc<GlUniform1iFn>("glUniform1i");
        auto uniform2f = LoadGlProc<GlUniform2fFn>("glUniform2f");
        auto uniform1f = LoadGlProc<GlUniform1fFn>("glUniform1f");
        auto uniform3fv = LoadGlProc<GlUniform3fvFn>("glUniform3fv");
        auto activeTexture = LoadGlProc<GlActiveTextureFn>("glActiveTexture");
        if (!useProgram || !bindBuffer || !enableVertexAttribArray || !disableVertexAttribArray ||
            !vertexAttribPointer || !uniformMatrix4fv || !uniform4fv || !uniform1i) {
            return false;
        }

        glViewport(0, 0, static_cast<GLsizei>(desc.renderWidth), static_cast<GLsizei>(desc.renderHeight));
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GEQUAL);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        useProgram(m_skinnedMeshProgram);
        if (activeTexture) {
            activeTexture(GL_TEXTURE0);
        }
        uniform1i(m_skinnedMeshAlbedoTextureLocation, 0);
        // Occlusion lives on a separate texture unit so it doesn't clobber the albedo binding
        // above; the sampler-to-unit mapping only needs to be set once per program use. Mirrors
        // the static mesh path above.
        uniform1i(m_skinnedMeshOcclusionTextureLocation, 1);
        // Ambient cube gets its own unit (2), mirroring the static mesh path above.
        uniform1i(m_skinnedMeshAmbientCubeLocation, 2);

        // desc.ambientCubeSrv is a per-frame descriptor, not per-draw, so it is resolved and bound
        // once here instead of inside the draw loop below (mirrors the static mesh path above).
        GLenum ambientCubeTarget = GL_TEXTURE_CUBE_MAP;
        GLuint ambientCubeTexture = 0;
        if (desc.ambientCubeSrv != 0) {
            const auto ambientIt = m_rhiTextureViews.find(desc.ambientCubeSrv);
            if (ambientIt != m_rhiTextureViews.end()) {
                ambientCubeTexture = ambientIt->second.texture;
                ambientCubeTarget = ambientIt->second.target;
            }
        }
        if (activeTexture) {
            activeTexture(GL_TEXTURE0 + 2);
        }
        glBindTexture(ambientCubeTarget, ambientCubeTexture);
        // Written unconditionally, including the 0 case, rather than relying on GL's default
        // uniform value of 0 (mirrors the static mesh path above).
        uniform1i(m_skinnedMeshHasAmbientCubeLocation, ambientCubeTexture != 0 ? 1 : 0);

        // Screen-space ambient occlusion from the native SSAO pass, on texture unit 3 (0 albedo,
        // 1 occlusion, 2 ambient cube). Per-frame rather than per-draw, so it is bound once here
        // like the ambient cube above. m_ssaoValid is false on any frame
        // RenderNativeAmbientOcclusion skipped, and u_hasRuntimeAoTexture = 0 then makes the
        // shader fall back to runtimeAo = 1.0 instead of reading a stale buffer.
        const GLuint runtimeAoTexture = (m_ssaoValid && m_runtimeAoTexture != 0) ? m_runtimeAoTexture : 0;
        if (activeTexture) {
            activeTexture(GL_TEXTURE0 + 3);
        }
        glBindTexture(GL_TEXTURE_2D, runtimeAoTexture);
        uniform1i(m_skinnedMeshRuntimeAoTextureLocation, 3);
        uniform1i(m_skinnedMeshHasRuntimeAoTextureLocation, runtimeAoTexture != 0 ? 1 : 0);
        // The fragment shader has no screen UV of its own, so it divides gl_FragCoord by this.
        if (uniform2f && m_skinnedMeshRenderSizeLocation >= 0) {
            uniform2f(m_skinnedMeshRenderSizeLocation,
                      desc.renderWidth > 0.0f ? desc.renderWidth : 1.0f,
                      desc.renderHeight > 0.0f ? desc.renderHeight : 1.0f);
        }
        if (uniform1f && m_skinnedMeshExposureLocation >= 0) {
            uniform1f(m_skinnedMeshExposureLocation, desc.exposure);
        }
        if (uniform3fv && m_skinnedMeshCameraPosLocation >= 0) {
            uniform3fv(m_skinnedMeshCameraPosLocation, 1, desc.cameraPos);
        }
        if (uniform1f && m_skinnedMeshEnableIblLocation >= 0) {
            uniform1f(m_skinnedMeshEnableIblLocation, desc.enableIbl ? 1.0f : 0.0f);
        }
        if (activeTexture) {
            activeTexture(GL_TEXTURE0);
        }

        for (uint32_t i = 0; i < desc.skinnedDrawCount; ++i) {
            const RhiBackendSkinnedMeshDrawDesc& draw = desc.skinnedDraws[i];
            if (!draw.boneMatrices) {
                continue;
            }
            const auto vbIt = m_rhiBuffers.find(draw.vertexBufferHandle);
            if (vbIt == m_rhiBuffers.end()) {
                continue;
            }

            float mvp[16] = {};
            Math::Mul4x4(draw.model, desc.viewProjection, mvp);
            uniformMatrix4fv(m_skinnedMeshMvpLocation, 1, GL_FALSE, mvp);
            if (m_skinnedMeshModelLocation >= 0) {
                uniformMatrix4fv(m_skinnedMeshModelLocation, 1, GL_FALSE, draw.model);
            }
            if (uniform1f && m_skinnedMeshMetallicLocation >= 0) {
                uniform1f(m_skinnedMeshMetallicLocation, draw.metallic);
            }
            uniform4fv(m_skinnedMeshBaseColorLocation, 1, draw.baseColor);
            uniformMatrix4fv(m_skinnedMeshBoneMatricesLocation, static_cast<GLsizei>(Skeleton::kMaxBones), GL_FALSE, draw.boneMatrices);

            const float lightDirIntensity[4] = {
                desc.sunDir[0], desc.sunDir[1], desc.sunDir[2], desc.sunIntensity
            };
            const float lightColor[4] = {
                desc.sunColor[0], desc.sunColor[1], desc.sunColor[2], 1.0f
            };
            const float emissiveRoughness[4] = {
                draw.emissive[0], draw.emissive[1], draw.emissive[2], draw.roughness
            };
            uniform4fv(m_skinnedMeshLightDirIntensityLocation, 1, lightDirIntensity);
            uniform4fv(m_skinnedMeshLightColorLocation, 1, lightColor);
            uniform4fv(m_skinnedMeshEmissiveRoughnessLocation, 1, emissiveRoughness);

            GLuint albedoTexture = 0;
            GLenum albedoTarget = GL_TEXTURE_2D;
            const auto textureIt = m_rhiTextureViews.find(draw.albedoSrv);
            if (textureIt != m_rhiTextureViews.end()) {
                albedoTexture = textureIt->second.texture;
                albedoTarget = textureIt->second.target;
            }
            glBindTexture(albedoTarget, albedoTexture);
            uniform1i(m_skinnedMeshHasAlbedoTextureLocation, albedoTexture != 0 ? 1 : 0);

            // Unlike the static loader, ModelLoaderSkinned.cpp only ever fills occlusionSrv from
            // glTF's dedicated occlusion texture slot and never substitutes metallic-roughness into
            // it, so occlusionSlotHasAo here only reflects the asset's occlusionStrength (mirrors
            // the static mesh path's gating above).
            GLuint occlusionTexture = 0;
            GLenum occlusionTarget = GL_TEXTURE_2D;
            if (draw.occlusionSlotHasAo && draw.occlusionSrv != 0) {
                const auto occlusionIt = m_rhiTextureViews.find(draw.occlusionSrv);
                if (occlusionIt != m_rhiTextureViews.end()) {
                    occlusionTexture = occlusionIt->second.texture;
                    occlusionTarget = occlusionIt->second.target;
                }
            }
            if (activeTexture) {
                activeTexture(GL_TEXTURE0 + 1);
            }
            glBindTexture(occlusionTarget, occlusionTexture);
            uniform1i(m_skinnedMeshHasOcclusionTextureLocation, occlusionTexture != 0 ? 1 : 0);
            if (activeTexture) {
                activeTexture(GL_TEXTURE0);
            }

            bindBuffer(GL_ARRAY_BUFFER, vbIt->second);
            enableVertexAttribArray(0);
            enableVertexAttribArray(1);
            enableVertexAttribArray(2);
            enableVertexAttribArray(3);
            enableVertexAttribArray(4);
            enableVertexAttribArray(5);
            vertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(draw.vertexStride), reinterpret_cast<const void*>(0));
            vertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(draw.vertexStride), reinterpret_cast<const void*>(12));
            vertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(draw.vertexStride), reinterpret_cast<const void*>(24));
            vertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(draw.vertexStride), reinterpret_cast<const void*>(40));
            // Bone indices are unsigned bytes; unnormalized upload converts them to plain
            // integer-valued floats (0..255) for the shader to round back to int and index with.
            vertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_FALSE, static_cast<GLsizei>(draw.vertexStride), reinterpret_cast<const void*>(48));
            vertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(draw.vertexStride), reinterpret_cast<const void*>(52));

            const auto ibIt = m_rhiBuffers.find(draw.indexBufferHandle);
            if (draw.indexCount > 0 && ibIt != m_rhiBuffers.end()) {
                bindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibIt->second);
                glDrawElements(GL_TRIANGLES,
                               static_cast<GLsizei>(draw.indexCount),
                               draw.index32Bit ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                               nullptr);
            } else if (draw.vertexCount > 0) {
                bindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(draw.vertexCount));
            }
        }

        disableVertexAttribArray(5);
        disableVertexAttribArray(4);
        disableVertexAttribArray(3);
        disableVertexAttribArray(2);
        disableVertexAttribArray(1);
        disableVertexAttribArray(0);
        bindBuffer(GL_ARRAY_BUFFER, 0);
        bindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        // The draw loop leaves unit 1 (occlusion) bound from its last iteration; unbind it too so
        // this pass doesn't leave a stray texture bound on a unit later code doesn't expect.
        if (activeTexture) {
            activeTexture(GL_TEXTURE0 + 1);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        // Unbind unit 2 (ambient cube), bound once above outside the draw loop, for the same
        // reason.
        if (activeTexture) {
            activeTexture(GL_TEXTURE0 + 2);
        }
        glBindTexture(ambientCubeTarget, 0);
        // Unbind unit 3 (screen-space AO) for the same reason: the SSAO target is reallocated on
        // a resize, and a stale binding here would outlive it.
        if (activeTexture) {
            activeTexture(GL_TEXTURE0 + 3);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        if (activeTexture) {
            activeTexture(GL_TEXTURE0);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        useProgram(0);
        return true;
    }
} // namespace SasamiEngine::Render
#endif
