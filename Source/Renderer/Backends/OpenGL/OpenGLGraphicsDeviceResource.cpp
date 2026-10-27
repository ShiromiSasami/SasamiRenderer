// OpenGLGraphicsDeviceResource.cpp
// The RHI resource surface of the OpenGL backend: textures, buffers, shader modules, pipelines
// and views.
//
// This file used to be empty, with a note saying these definitions had to stay in
// OpenGLGraphicsDevice.cpp "so the OpenGL helper functions stay in one translation unit". That
// constraint is gone: the entry points now live in OpenGLProcs.h, so any translation unit can
// call them.
#include "Renderer/Backends/OpenGL/OpenGLGraphicsDevice.h"
#include "Renderer/Backends/OpenGL/OpenGLProcs.h"

#if RHI_OPENGL

#include "Foundation/Tools/DebugOutput.h"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace SasamiEngine::Render
{
    RhiTextureHandle OpenGLGraphicsDevice::CreateRhiTexture2DFromRgba8(uint32_t width,
                                                                       uint32_t height,
                                                                       const void* pixels,
                                                                       uint32_t rowPitchBytes,
                                                                       RhiFormat format)
    {
        if (format != RhiFormat::R8G8B8A8UNorm && format != RhiFormat::R8G8B8A8UNormSrgb) {
            DebugLog("OpenGLGraphicsDevice::CreateRhiTexture2DFromRgba8: unsupported format, only "
                     "R8G8B8A8UNorm and R8G8B8A8UNormSrgb are accepted.\n");
            return {};
        }
        if (!m_hdc || !m_context || width == 0 || height == 0 || !pixels || rowPitchBytes < width * 4u) {
            return {};
        }
        if (!wglMakeCurrent(m_hdc, m_context)) {
            return {};
        }

        const void* uploadPixels = pixels;
        std::vector<uint8_t> compactPixels;
        const uint32_t tightRowPitch = width * 4u;
        if (rowPitchBytes != tightRowPitch) {
            compactPixels.resize(static_cast<size_t>(tightRowPitch) * static_cast<size_t>(height));
            const auto* src = static_cast<const uint8_t*>(pixels);
            for (uint32_t y = 0; y < height; ++y) {
                std::memcpy(compactPixels.data() + static_cast<size_t>(tightRowPitch) * y,
                            src + static_cast<size_t>(rowPitchBytes) * y,
                            tightRowPitch);
            }
            uploadPixels = compactPixels.data();
        }

        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     ToGlInternalFormat(format),
                     static_cast<GLsizei>(width),
                     static_cast<GLsizei>(height),
                     0,
                     GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     uploadPixels);
        glBindTexture(GL_TEXTURE_2D, 0);

        const uint64_t id = m_nextRhiResourceHandle++;
        m_rhiTextures[id] = texture;
        return RhiTextureHandle{ id };
    }

    RhiTextureHandle OpenGLGraphicsDevice::CreateRhiTextureCubeFromFaces(uint32_t faceSize,
                                                                        const void* const faces[6],
                                                                        uint32_t rowPitchBytes,
                                                                        RhiFormat format)
    {
        // R16G16B16A16Float is accepted alongside the 8-bit formats because the HDR ambient/sky
        // cubemap (RendererMathUtility::GenerateSkyCubemapFromEquirect) uploads float RGBA faces,
        // same as the Vulkan backend's CreateRhiTextureCubeFromFaces.
        if (format != RhiFormat::R8G8B8A8UNorm && format != RhiFormat::R8G8B8A8UNormSrgb &&
            format != RhiFormat::R16G16B16A16Float) {
            DebugLog("OpenGLGraphicsDevice::CreateRhiTextureCubeFromFaces: unsupported format, only "
                     "R8G8B8A8UNorm, R8G8B8A8UNormSrgb and R16G16B16A16Float are accepted.\n");
            return {};
        }
        // R16G16B16A16Float is 8 bytes per texel; treating it as the 4-byte-per-texel formats
        // below would under-validate rowPitchBytes and under-stride the per-row copy, corrupting
        // every row after the first. The Vulkan backend hit exactly this bug for cube uploads.
        const uint32_t bytesPerPixel = format == RhiFormat::R16G16B16A16Float ? 8u : 4u;
        if (!m_hdc || !m_context || faceSize == 0 || !faces ||
            rowPitchBytes < faceSize * bytesPerPixel) {
            return {};
        }
        for (uint32_t face = 0; face < 6u; ++face) {
            if (!faces[face]) {
                return {};
            }
        }
        if (!wglMakeCurrent(m_hdc, m_context)) {
            return {};
        }

        const GLenum uploadFormat = ToGlFormat(format);
        const GLenum uploadType = ToGlType(format);
        const uint32_t tightRowPitch = faceSize * bytesPerPixel;
        std::vector<uint8_t> compactFace;

        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_CUBE_MAP, texture);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Clamping R too, even though a cube face is only ever sampled along S/T, keeps all three
        // wrap axes consistent with how a cube sampler is configured elsewhere (Vulkan/DX11).
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

        for (uint32_t face = 0; face < 6u; ++face) {
            const void* uploadPixels = faces[face];
            if (rowPitchBytes != tightRowPitch) {
                compactFace.resize(static_cast<size_t>(tightRowPitch) * static_cast<size_t>(faceSize));
                const auto* src = static_cast<const uint8_t*>(faces[face]);
                for (uint32_t y = 0; y < faceSize; ++y) {
                    std::memcpy(compactFace.data() + static_cast<size_t>(tightRowPitch) * y,
                                src + static_cast<size_t>(rowPitchBytes) * y,
                                tightRowPitch);
                }
                uploadPixels = compactFace.data();
            }
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                         0,
                         ToGlInternalFormat(format),
                         static_cast<GLsizei>(faceSize),
                         static_cast<GLsizei>(faceSize),
                         0,
                         uploadFormat,
                         uploadType,
                         uploadPixels);
        }
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);

        // Seamless cube filtering is GL 3.2+; this context is at least 3.3 (native sky/ray-march
        // shaders already require GLSL 330), so it should be available, but a driver that somehow
        // lacks it should not fail texture creation over it -- non-seamless cube edges are only a
        // minor visual seam, not a correctness bug.
        glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

        const uint64_t id = m_nextRhiResourceHandle++;
        m_rhiTextures[id] = texture;
        return RhiTextureHandle{ id };
    }

    RhiTextureHandle OpenGLGraphicsDevice::CreateRhiTexture3DFromData(uint32_t width,
                                                                      uint32_t height,
                                                                      uint32_t depth,
                                                                      const void* texels,
                                                                      uint32_t rowPitchBytes,
                                                                      uint32_t slicePitchBytes,
                                                                      RhiFormat format)
    {
        const uint32_t texelBytes = format == RhiFormat::R32Float ? 4u : (format == RhiFormat::R16Float ? 2u : 0u);
        if (texelBytes == 0) {
            DebugLog("OpenGLGraphicsDevice::CreateRhiTexture3DFromData: unsupported format, only R32Float/R16Float are accepted.\n");
            return {};
        }
        if (!m_hdc || !m_context || width == 0 || height == 0 || depth == 0 || !texels ||
            rowPitchBytes < width * texelBytes ||
            static_cast<uint64_t>(slicePitchBytes) < static_cast<uint64_t>(rowPitchBytes) * height) {
            return {};
        }
        if (!wglMakeCurrent(m_hdc, m_context)) {
            return {};
        }
        const auto texImage3D = LoadGlProc<GlTexImage3DFn>("glTexImage3D");
        if (!texImage3D) {
            return {};
        }

        // GL takes a tightly packed volume here (UNPACK_ROW_LENGTH/IMAGE_HEIGHT are left at
        // their defaults elsewhere), so padded rows or slices are compacted first.
        const size_t tightRow = static_cast<size_t>(width) * texelBytes;
        const size_t tightSlice = tightRow * height;
        const void* uploadTexels = texels;
        std::vector<uint8_t> compact;
        if (rowPitchBytes != tightRow || slicePitchBytes != tightSlice) {
            compact.resize(tightSlice * depth);
            const auto* src = static_cast<const uint8_t*>(texels);
            for (uint32_t z = 0; z < depth; ++z) {
                for (uint32_t y = 0; y < height; ++y) {
                    std::memcpy(compact.data() + tightSlice * z + tightRow * y,
                                src + static_cast<size_t>(slicePitchBytes) * z + static_cast<size_t>(rowPitchBytes) * y,
                                tightRow);
                }
            }
            uploadTexels = compact.data();
        }

        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_3D, texture);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        texImage3D(GL_TEXTURE_3D,
                   0,
                   ToGlInternalFormat(format),
                   static_cast<GLsizei>(width),
                   static_cast<GLsizei>(height),
                   static_cast<GLsizei>(depth),
                   0,
                   ToGlFormat(format),
                   ToGlType(format),
                   uploadTexels);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_3D, 0);

        const uint64_t id = m_nextRhiResourceHandle++;
        m_rhiTextures[id] = texture;
        return RhiTextureHandle{ id };
    }

    RhiTextureHandle OpenGLGraphicsDevice::CreateRhiTexture(const RhiTextureDesc& desc)
    {
        if (!m_hdc || !m_context || desc.extent.width == 0 || desc.extent.height == 0 ||
            desc.dimension != RhiResourceDimension::Texture2D) {
            return {};
        }
        if (!wglMakeCurrent(m_hdc, m_context)) {
            return {};
        }

        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     ToGlInternalFormat(desc.format),
                     static_cast<GLsizei>(desc.extent.width),
                     static_cast<GLsizei>(desc.extent.height),
                     0,
                     ToGlFormat(desc.format),
                     ToGlType(desc.format),
                     nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);

        const uint64_t id = m_nextRhiResourceHandle++;
        m_rhiTextures[id] = texture;
        return RhiTextureHandle{ id };
    }

    RhiBufferHandle OpenGLGraphicsDevice::CreateRhiBuffer(const RhiBufferDesc& desc, const void* initialData)
    {
        if (!m_hdc || !m_context || desc.sizeInBytes == 0) {
            return {};
        }
        if (!wglMakeCurrent(m_hdc, m_context)) {
            return {};
        }

        auto genBuffers = LoadGlProc<GlGenBuffersFn>("glGenBuffers");
        auto bindBuffer = LoadGlProc<GlBindBufferFn>("glBindBuffer");
        auto bufferData = LoadGlProc<GlBufferDataFn>("glBufferData");
        if (!genBuffers || !bindBuffer || !bufferData) {
            return {};
        }

        GLuint buffer = 0;
        const GLenum target = ToGlBufferTarget(desc.usage);
        genBuffers(1, &buffer);
        bindBuffer(target, buffer);
        bufferData(target,
                   static_cast<std::ptrdiff_t>(desc.sizeInBytes),
                   initialData,
                   desc.memoryUsage == RhiMemoryUsage::CpuToGpu ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        bindBuffer(target, 0);

        const uint64_t id = m_nextRhiResourceHandle++;
        m_rhiBuffers[id] = buffer;
        m_rhiBufferSizes[id] = desc.sizeInBytes;
        m_rhiBufferTargets[id] = target;
        return RhiBufferHandle{ id };
    }

    bool OpenGLGraphicsDevice::UpdateRhiBuffer(RhiBufferHandle bufferHandle,
                                               uint64_t offsetInBytes,
                                               const void* data,
                                               uint64_t sizeInBytes)
    {
        const auto bufferIt = m_rhiBuffers.find(bufferHandle.id);
        const auto sizeIt = m_rhiBufferSizes.find(bufferHandle.id);
        const auto targetIt = m_rhiBufferTargets.find(bufferHandle.id);
        if (bufferIt == m_rhiBuffers.end() || sizeIt == m_rhiBufferSizes.end() ||
            targetIt == m_rhiBufferTargets.end() || !data || sizeInBytes == 0 ||
            offsetInBytes >= sizeIt->second || sizeInBytes > sizeIt->second - offsetInBytes ||
            !m_hdc || !m_context || !wglMakeCurrent(m_hdc, m_context)) {
            return false;
        }

        auto bindBuffer = LoadGlProc<GlBindBufferFn>("glBindBuffer");
        auto bufferSubData = LoadGlProc<GlBufferSubDataFn>("glBufferSubData");
        if (!bindBuffer || !bufferSubData) {
            return false;
        }
        bindBuffer(targetIt->second, bufferIt->second);
        bufferSubData(targetIt->second,
                      static_cast<std::ptrdiff_t>(offsetInBytes),
                      static_cast<std::ptrdiff_t>(sizeInBytes),
                      data);
        bindBuffer(targetIt->second, 0);
        return true;
    }

    bool OpenGLGraphicsDevice::ReadRhiBuffer(RhiBufferHandle bufferHandle,
                                             uint64_t offsetInBytes,
                                             void* data,
                                             uint64_t sizeInBytes)
    {
        const auto bufferIt = m_rhiBuffers.find(bufferHandle.id);
        const auto sizeIt = m_rhiBufferSizes.find(bufferHandle.id);
        const auto targetIt = m_rhiBufferTargets.find(bufferHandle.id);
        if (bufferIt == m_rhiBuffers.end() || sizeIt == m_rhiBufferSizes.end() ||
            targetIt == m_rhiBufferTargets.end() || !data || sizeInBytes == 0 ||
            offsetInBytes >= sizeIt->second || sizeInBytes > sizeIt->second - offsetInBytes ||
            !m_hdc || !m_context || !wglMakeCurrent(m_hdc, m_context)) {
            return false;
        }

        auto bindBuffer = LoadGlProc<GlBindBufferFn>("glBindBuffer");
        auto getBufferSubData = LoadGlProc<GlGetBufferSubDataFn>("glGetBufferSubData");
        if (!bindBuffer || !getBufferSubData) {
            return false;
        }
        bindBuffer(targetIt->second, bufferIt->second);
        getBufferSubData(targetIt->second,
                         static_cast<std::ptrdiff_t>(offsetInBytes),
                         static_cast<std::ptrdiff_t>(sizeInBytes),
                         data);
        bindBuffer(targetIt->second, 0);
        return true;
    }

    bool OpenGLGraphicsDevice::DestroyRhiResource(RhiResourceHandle resource)
    {
        if (!resource.IsValid() || !m_hdc || !m_context || !wglMakeCurrent(m_hdc, m_context)) {
            return false;
        }

        const auto textureIt = m_rhiTextures.find(resource.id);
        if (textureIt != m_rhiTextures.end()) {
            const GLuint texture = textureIt->second;
            for (auto it = m_rhiTextureViews.begin(); it != m_rhiTextureViews.end();) {
                if (it->second.texture == texture) {
                    m_rhiTextureUavs.erase(it->first);
                    it = m_rhiTextureViews.erase(it);
                } else {
                    ++it;
                }
            }
            glDeleteTextures(1, &texture);
            m_rhiTextures.erase(textureIt);
            return true;
        }

        const auto bufferIt = m_rhiBuffers.find(resource.id);
        if (bufferIt != m_rhiBuffers.end()) {
            auto deleteBuffers = LoadGlProc<GlDeleteBuffersFn>("glDeleteBuffers");
            if (!deleteBuffers) {
                return false;
            }
            const GLuint buffer = bufferIt->second;
            for (auto it = m_rhiBufferViews.begin(); it != m_rhiBufferViews.end();) {
                if (it->second.resourceId == resource.id) {
                    m_rhiBufferUavs.erase(it->first);
                    it = m_rhiBufferViews.erase(it);
                } else {
                    ++it;
                }
            }
            deleteBuffers(1, &buffer);
            m_rhiBuffers.erase(bufferIt);
            m_rhiBufferSizes.erase(resource.id);
            m_rhiBufferTargets.erase(resource.id);
            return true;
        }
        return false;
    }

    RhiShaderHandle OpenGLGraphicsDevice::CreateRhiShaderModule(const RhiShaderModuleDesc& desc)
    {
        if (!m_hdc || !m_context || !desc.bytecode || desc.bytecodeSize == 0) {
            return {};
        }
        if (!wglMakeCurrent(m_hdc, m_context)) {
            return {};
        }

        auto createShader = LoadGlProc<GlCreateShaderFn>("glCreateShader");
        auto shaderSource = LoadGlProc<GlShaderSourceFn>("glShaderSource");
        auto compileShader = LoadGlProc<GlCompileShaderFn>("glCompileShader");
        auto getShaderiv = LoadGlProc<GlGetShaderivFn>("glGetShaderiv");
        auto deleteShader = LoadGlProc<GlDeleteShaderFn>("glDeleteShader");
        const GLenum stage = ToGlShaderStage(desc.stage);
        if (!createShader || !shaderSource || !compileShader || !getShaderiv || !deleteShader || stage == 0) {
            return {};
        }

        const char* source = static_cast<const char*>(desc.bytecode);
        const GLint length = static_cast<GLint>(desc.bytecodeSize);
        GLuint shader = createShader(stage);
        shaderSource(shader, 1, &source, &length);
        compileShader(shader);
        GLint ok = GL_FALSE;
        getShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (ok == GL_FALSE) {
            deleteShader(shader);
            return {};
        }

        const uint64_t id = m_nextRhiShaderHandle++;
        m_rhiShaders[id] = OpenGLRhiShader{ shader, desc.stage, desc.entryPoint ? desc.entryPoint : "main" };
        return RhiShaderHandle{ id };
    }

    RhiPipelineLayoutHandle OpenGLGraphicsDevice::CreateRhiPipelineLayout(const RhiPipelineLayoutDesc& desc)
    {
        if (!m_hdc || !m_context || !wglMakeCurrent(m_hdc, m_context) ||
            (desc.staticSamplerCount > 0 && !desc.staticSamplers)) return {};
        auto genSamplers = LoadGlProc<GlGenSamplersFn>("glGenSamplers");
        auto deleteSamplers = LoadGlProc<GlDeleteSamplersFn>("glDeleteSamplers");
        auto samplerParameteri = LoadGlProc<GlSamplerParameteriFn>("glSamplerParameteri");
        if (desc.staticSamplerCount > 0 && (!genSamplers || !deleteSamplers || !samplerParameteri)) return {};
        OpenGLRhiPipelineLayout layout{};
        layout.bindingCount = desc.bindingCount;
        layout.staticSamplers.reserve(desc.staticSamplerCount);
        for (uint32_t i = 0; i < desc.staticSamplerCount; ++i) {
            const auto& source = desc.staticSamplers[i];
            if (source.registerSpace != 0) return {};
            OpenGLRhiPipelineLayout::StaticSampler sampler{};
            sampler.visibility = source.visibility;
            sampler.shaderRegister = source.shaderRegister;
            genSamplers(1, &sampler.sampler);
            if (sampler.sampler == 0) {
                for (const auto& created : layout.staticSamplers) deleteSamplers(1, &created.sampler);
                return {};
            }
            samplerParameteri(sampler.sampler, GL_TEXTURE_MIN_FILTER, source.linearFilter ? GL_LINEAR : GL_NEAREST);
            samplerParameteri(sampler.sampler, GL_TEXTURE_MAG_FILTER, source.linearFilter ? GL_LINEAR : GL_NEAREST);
            samplerParameteri(sampler.sampler, GL_TEXTURE_WRAP_S, source.clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            samplerParameteri(sampler.sampler, GL_TEXTURE_WRAP_T, source.clamp ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            layout.staticSamplers.push_back(sampler);
        }
        const uint64_t id = m_nextRhiPipelineLayoutHandle++;
        m_rhiPipelineLayouts[id] = std::move(layout);
        return RhiPipelineLayoutHandle{ id };
    }

    RhiPipelineHandle OpenGLGraphicsDevice::CreateRhiGraphicsPipeline(const RhiGraphicsPipelineDesc& desc)
    {
        if (!m_hdc || !m_context || !wglMakeCurrent(m_hdc, m_context)) {
            return {};
        }
        auto createProgram = LoadGlProc<GlCreateProgramFn>("glCreateProgram");
        auto attachShader = LoadGlProc<GlAttachShaderFn>("glAttachShader");
        auto linkProgram = LoadGlProc<GlLinkProgramFn>("glLinkProgram");
        auto getProgramiv = LoadGlProc<GlGetProgramivFn>("glGetProgramiv");
        auto deleteProgram = LoadGlProc<GlDeleteProgramFn>("glDeleteProgram");
        if (!createProgram || !attachShader || !linkProgram || !getProgramiv || !deleteProgram) {
            return {};
        }

        GLuint program = createProgram();
        if (desc.shaderHandles) {
            for (uint32_t i = 0; i < desc.shaderHandleCount; ++i) {
                const auto it = m_rhiShaders.find(desc.shaderHandles[i].id);
                if (it != m_rhiShaders.end() && it->second.shader != 0) {
                    attachShader(program, it->second.shader);
                }
            }
        }
        linkProgram(program);
        GLint linked = GL_FALSE;
        getProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked == GL_FALSE) {
            deleteProgram(program);
            return {};
        }

        const uint64_t id = m_nextRhiPipelineHandle++;
        m_rhiPipelines[id] = OpenGLRhiPipeline{ program };
        return RhiPipelineHandle{ id };
    }

    RhiPipelineHandle OpenGLGraphicsDevice::CreateRhiComputePipeline(const RhiComputePipelineDesc& desc)
    {
        if (!m_hdc || !m_context || !desc.shader.bytecode || desc.shader.bytecodeSize == 0 ||
            !wglMakeCurrent(m_hdc, m_context)) {
            return {};
        }
        auto createShader = LoadGlProc<GlCreateShaderFn>("glCreateShader");
        auto shaderSource = LoadGlProc<GlShaderSourceFn>("glShaderSource");
        auto compileShader = LoadGlProc<GlCompileShaderFn>("glCompileShader");
        auto getShaderiv = LoadGlProc<GlGetShaderivFn>("glGetShaderiv");
        auto deleteShader = LoadGlProc<GlDeleteShaderFn>("glDeleteShader");
        auto createProgram = LoadGlProc<GlCreateProgramFn>("glCreateProgram");
        auto attachShader = LoadGlProc<GlAttachShaderFn>("glAttachShader");
        auto linkProgram = LoadGlProc<GlLinkProgramFn>("glLinkProgram");
        auto getProgramiv = LoadGlProc<GlGetProgramivFn>("glGetProgramiv");
        auto deleteProgram = LoadGlProc<GlDeleteProgramFn>("glDeleteProgram");
        if (!createShader || !shaderSource || !compileShader || !getShaderiv || !deleteShader ||
            !createProgram || !attachShader || !linkProgram || !getProgramiv || !deleteProgram) {
            return {};
        }

        const char* source = static_cast<const char*>(desc.shader.bytecode);
        const GLint length = static_cast<GLint>(desc.shader.bytecodeSize);
        const GLuint shader = createShader(GL_COMPUTE_SHADER);
        shaderSource(shader, 1, &source, &length);
        compileShader(shader);
        GLint compiled = GL_FALSE;
        getShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled == GL_FALSE) {
            deleteShader(shader);
            return {};
        }

        const GLuint program = createProgram();
        attachShader(program, shader);
        linkProgram(program);
        deleteShader(shader);
        GLint linked = GL_FALSE;
        getProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked == GL_FALSE) {
            deleteProgram(program);
            return {};
        }

        const uint64_t id = m_nextRhiPipelineHandle++;
        m_rhiPipelines[id] = OpenGLRhiPipeline{ program };
        return RhiPipelineHandle{ id };
    }

    RhiDescriptorAllocation OpenGLGraphicsDevice::AllocateRhiDescriptors(RhiDescriptorHeapType type,
                                                                        uint32_t count,
                                                                        bool shaderVisible)
    {
        (void)shaderVisible;
        if (count == 0) {
            return {};
        }
        const uint64_t base = m_nextRhiDescriptorHandle;
        m_nextRhiDescriptorHandle += count;

        RhiDescriptorAllocation allocation{};
        allocation.type = type;
        allocation.cpu.ptr = base;
        allocation.gpu.ptr = base;
        allocation.count = count;
        allocation.increment = 1;
        return allocation;
    }

    bool OpenGLGraphicsDevice::CreateRhiShaderResourceView(RhiResourceHandle resource,
                                                           const RhiTextureViewDesc& desc,
                                                           RhiCpuDescriptorHandle destination)
    {
        const auto it = m_rhiTextures.find(resource.id);
        if (it == m_rhiTextures.end() || !destination.IsValid()) {
            return false;
        }
        // desc.dimension used to be discarded here, which left every SRV -- including the ones
        // Skybox creates over a cube texture -- recorded as a bare GL name with no target. GL
        // requires the target at bind time (glBindTexture(target, name)), so a cube SRV bound as
        // GL_TEXTURE_2D would silently bind nothing useful; the target has to be captured now.
        const unsigned int target = desc.dimension == RhiTextureViewDimension::TextureCube
            ? GL_TEXTURE_CUBE_MAP
            : (desc.dimension == RhiTextureViewDimension::Texture3D ? GL_TEXTURE_3D : GL_TEXTURE_2D);
        m_rhiTextureViews[destination.ptr] = OpenGLRhiTextureView{ it->second, target };
        return true;
    }

    bool OpenGLGraphicsDevice::CreateRhiBufferShaderResourceView(RhiBufferHandle buffer,
                                                                 const RhiBufferViewDesc& desc,
                                                                 RhiCpuDescriptorHandle destination)
    {
        const auto it = m_rhiBuffers.find(buffer.id);
        if (it == m_rhiBuffers.end() || !destination.IsValid()) {
            return false;
        }

        OpenGLRhiBufferView view{};
        view.buffer = it->second;
        view.target = desc.type == RhiBufferViewType::Constant
            ? GL_UNIFORM_BUFFER
            : GL_SHADER_STORAGE_BUFFER;
        view.resourceId = buffer.id;
        m_rhiBufferViews[destination.ptr] = view;
        return true;
    }

    bool OpenGLGraphicsDevice::CreateRhiUnorderedAccessView(RhiTextureHandle texture,
                                                            const RhiTextureViewDesc& desc,
                                                            RhiCpuDescriptorHandle destination)
    {
        const auto it = m_rhiTextures.find(texture.id);
        if (it == m_rhiTextures.end() || !destination.IsValid() ||
            desc.dimension != RhiTextureViewDimension::Texture2D ||
            ToGlInternalFormat(desc.format) == 0) return false;
        m_rhiTextureViews[destination.ptr] = OpenGLRhiTextureView{ it->second, GL_TEXTURE_2D };
        m_rhiTextureUavs[destination.ptr] = desc.format;
        return true;
    }

    bool OpenGLGraphicsDevice::CreateRhiBufferUnorderedAccessView(RhiBufferHandle buffer,
                                                                  const RhiBufferViewDesc& desc,
                                                                  RhiCpuDescriptorHandle destination)
    {
        if (!CreateRhiBufferShaderResourceView(buffer, desc, destination)) return false;
        m_rhiBufferUavs[destination.ptr] = true;
        return true;
    }

    bool OpenGLGraphicsDevice::CreateRhiRenderTargetView(RhiTextureHandle texture,
                                                         const RhiRenderTargetViewDesc&,
                                                         RhiCpuDescriptorHandle destination)
    {
        const auto it = m_rhiTextures.find(texture.id);
        if (it == m_rhiTextures.end() || !destination.IsValid()) {
            return false;
        }
        m_rhiTextureViews[destination.ptr] = OpenGLRhiTextureView{ it->second, GL_TEXTURE_2D };
        return true;
    }

    bool OpenGLGraphicsDevice::CreateRhiDepthStencilView(RhiTextureHandle texture,
                                                         const RhiDepthStencilViewDesc&,
                                                         RhiCpuDescriptorHandle destination)
    {
        const auto it = m_rhiTextures.find(texture.id);
        if (it == m_rhiTextures.end() || !destination.IsValid()) {
            return false;
        }
        m_rhiTextureViews[destination.ptr] = OpenGLRhiTextureView{ it->second, GL_TEXTURE_2D };
        return true;
    }

}

#endif // RHI_OPENGL
