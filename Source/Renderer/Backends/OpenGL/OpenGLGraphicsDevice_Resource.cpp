// OpenGLGraphicsDevice_Resource.cpp
// RHI resource creation, pipeline state, command encoding.
#include "Renderer/Backends/OpenGL/OpenGLGraphicsDevice.h"

#if RHI_OPENGL

#include "Foundation/Tools/DebugOutput.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <GL/gl.h>


namespace SasamiRenderer
{
    RhiTextureHandle OpenGLGraphicsDevice::CreateRhiTexture(const RhiTextureDesc& desc)
    {
        if (!m_context || desc.dimension != RhiResourceDimension::Texture2D ||
            desc.extent.width == 0 || desc.extent.height == 0) {
            return {};
        }
        if (!wglMakeCurrent(m_hdc, m_context)) {
            return {};
        }

        GLuint texture = 0;
        glGenTextures(1, &texture);
        if (texture == 0) {
            return {};
        }

        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, desc.mipLevels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
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
        m_rhiTextures.emplace(id, texture);
        return RhiTextureHandle{ id };
    }

    RhiBufferHandle OpenGLGraphicsDevice::CreateRhiBuffer(const RhiBufferDesc& desc, const void* initialData)
    {
        if (!m_context || desc.sizeInBytes == 0 || !wglMakeCurrent(m_hdc, m_context)) {
            return {};
        }

        auto glGenBuffersPtr = reinterpret_cast<GlGenBuffersFn>(wglGetProcAddress("glGenBuffers"));
        auto glBindBufferPtr = reinterpret_cast<GlBindBufferFn>(wglGetProcAddress("glBindBuffer"));
        auto glBufferDataPtr = reinterpret_cast<GlBufferDataFn>(wglGetProcAddress("glBufferData"));
        if (!glGenBuffersPtr || !glBindBufferPtr || !glBufferDataPtr) {
            return {};
        }

        GLuint buffer = 0;
        const GLenum target = ToGlBufferTarget(desc.usage);
        glGenBuffersPtr(1, &buffer);
        if (buffer == 0) {
            return {};
        }
        glBindBufferPtr(target, buffer);
        glBufferDataPtr(target,
                        static_cast<std::ptrdiff_t>(desc.sizeInBytes),
                        initialData,
                        desc.memoryUsage == RhiMemoryUsage::CpuToGpu ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        glBindBufferPtr(target, 0);

        const uint64_t id = m_nextRhiResourceHandle++;
        m_rhiBuffers.emplace(id, buffer);
        return RhiBufferHandle{ id };
    }

    RhiShaderHandle OpenGLGraphicsDevice::CreateRhiShaderModule(const RhiShaderModuleDesc& desc)
    {
        if (!m_context || !desc.bytecode || desc.bytecodeSize == 0 || !wglMakeCurrent(m_hdc, m_context)) {
            return {};
        }

        auto glCreateShaderPtr = LoadGlProc<GlCreateShaderFn>("glCreateShader");
        auto glShaderSourcePtr = LoadGlProc<GlShaderSourceFn>("glShaderSource");
        auto glCompileShaderPtr = LoadGlProc<GlCompileShaderFn>("glCompileShader");
        auto glGetShaderivPtr = LoadGlProc<GlGetShaderivFn>("glGetShaderiv");
        auto glGetShaderInfoLogPtr = LoadGlProc<GlGetShaderInfoLogFn>("glGetShaderInfoLog");
        auto glDeleteShaderPtr = LoadGlProc<GlDeleteShaderFn>("glDeleteShader");
        if (!glCreateShaderPtr || !glShaderSourcePtr || !glCompileShaderPtr || !glGetShaderivPtr || !glDeleteShaderPtr) {
            return {};
        }

        const GLenum stage = ToGlShaderStage(desc.stage);
        if (stage == 0) {
            return {};
        }

        const GLuint shader = glCreateShaderPtr(stage);
        const char* source = static_cast<const char*>(desc.bytecode);
        const GLint length = static_cast<GLint>(desc.bytecodeSize);
        glShaderSourcePtr(shader, 1, &source, &length);
        glCompileShaderPtr(shader);

        GLint status = 0;
        glGetShaderivPtr(shader, GL_COMPILE_STATUS, &status);
        if (status == 0) {
            if (glGetShaderInfoLogPtr) {
                GLint logLength = 0;
                glGetShaderivPtr(shader, GL_INFO_LOG_LENGTH, &logLength);
                if (logLength > 1) {
                    std::vector<char> log(static_cast<size_t>(logLength));
                    glGetShaderInfoLogPtr(shader, logLength, nullptr, log.data());
                    DebugLog(log.data());
                    DebugLog("\n");
                }
            }
            glDeleteShaderPtr(shader);
            return {};
        }

        OpenGLRhiShader stored{};
        stored.shader = shader;
        stored.stage = desc.stage;
        stored.entryPoint = desc.entryPoint ? desc.entryPoint : "main";

        const uint64_t id = m_nextRhiShaderHandle++;
        m_rhiShaders.emplace(id, std::move(stored));
        return RhiShaderHandle{ id };
    }

    RhiPipelineLayoutHandle OpenGLGraphicsDevice::CreateRhiPipelineLayout(const RhiPipelineLayoutDesc& desc)
    {
        const uint64_t id = m_nextRhiPipelineLayoutHandle++;
        m_rhiPipelineLayouts.emplace(id, desc.bindingCount);
        return RhiPipelineLayoutHandle{ id };
    }

    RhiPipelineHandle OpenGLGraphicsDevice::CreateRhiGraphicsPipeline(const RhiGraphicsPipelineDesc& desc)
    {
        if (!m_context || !desc.layout.IsValid() || m_rhiPipelineLayouts.find(desc.layout.id) == m_rhiPipelineLayouts.end() ||
            !wglMakeCurrent(m_hdc, m_context)) {
            return {};
        }

        auto glCreateProgramPtr = LoadGlProc<GlCreateProgramFn>("glCreateProgram");
        auto glAttachShaderPtr = LoadGlProc<GlAttachShaderFn>("glAttachShader");
        auto glLinkProgramPtr = LoadGlProc<GlLinkProgramFn>("glLinkProgram");
        auto glGetProgramivPtr = LoadGlProc<GlGetProgramivFn>("glGetProgramiv");
        auto glGetProgramInfoLogPtr = LoadGlProc<GlGetProgramInfoLogFn>("glGetProgramInfoLog");
        auto glDeleteProgramPtr = LoadGlProc<GlDeleteProgramFn>("glDeleteProgram");
        if (!glCreateProgramPtr || !glAttachShaderPtr || !glLinkProgramPtr || !glGetProgramivPtr || !glDeleteProgramPtr) {
            return {};
        }

        const GLuint program = glCreateProgramPtr();
        if (program == 0) {
            return {};
        }

        if (desc.shaderHandles) {
            for (uint32_t i = 0; i < desc.shaderHandleCount; ++i) {
                const auto shaderIt = m_rhiShaders.find(desc.shaderHandles[i].id);
                if (shaderIt != m_rhiShaders.end() && shaderIt->second.shader != 0) {
                    glAttachShaderPtr(program, shaderIt->second.shader);
                }
            }
        }
        if (desc.shaders) {
            for (uint32_t i = 0; i < desc.shaderCount; ++i) {
                const RhiShaderHandle shader = CreateRhiShaderModule(desc.shaders[i]);
                const auto shaderIt = m_rhiShaders.find(shader.id);
                if (shaderIt != m_rhiShaders.end() && shaderIt->second.shader != 0) {
                    glAttachShaderPtr(program, shaderIt->second.shader);
                }
            }
        }

        glLinkProgramPtr(program);
        GLint status = 0;
        glGetProgramivPtr(program, GL_LINK_STATUS, &status);
        if (status == 0) {
            if (glGetProgramInfoLogPtr) {
                GLint logLength = 0;
                glGetProgramivPtr(program, GL_INFO_LOG_LENGTH, &logLength);
                if (logLength > 1) {
                    std::vector<char> log(static_cast<size_t>(logLength));
                    glGetProgramInfoLogPtr(program, logLength, nullptr, log.data());
                    DebugLog(log.data());
                    DebugLog("\n");
                }
            }
            glDeleteProgramPtr(program);
            return {};
        }

        OpenGLRhiPipeline pipeline{};
        pipeline.program = program;
        const uint64_t id = m_nextRhiPipelineHandle++;
        m_rhiPipelines.emplace(id, pipeline);
        return RhiPipelineHandle{ id };
    }

    RhiPipelineHandle OpenGLGraphicsDevice::CreateRhiComputePipeline(const RhiComputePipelineDesc& desc)
    {
        RhiPipelineLayoutDesc layoutDesc{};
        const RhiPipelineLayoutHandle layout = CreateRhiPipelineLayout(layoutDesc);
        RhiGraphicsPipelineDesc pipelineDesc{};
        pipelineDesc.layout = layout;
        pipelineDesc.shaders = &desc.shader;
        pipelineDesc.shaderCount = 1;
        return CreateRhiGraphicsPipeline(pipelineDesc);
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
                                                           const RhiTextureViewDesc&,
                                                           RhiCpuDescriptorHandle destination)
    {
        const auto it = m_rhiTextures.find(resource.id);
        if (it == m_rhiTextures.end() || !destination.IsValid()) {
            return false;
        }
        m_rhiTextureViews[destination.ptr] = it->second;
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
        m_rhiTextureViews[destination.ptr] = it->second;
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
        m_rhiTextureViews[destination.ptr] = it->second;
        return true;
    }

    std::unique_ptr<IRhiCommandEncoder> OpenGLGraphicsDevice::CreateCommandEncoder(RhiQueueType queueType)
    {
        if (!m_context) {
            return std::make_unique<NullRhiCommandEncoder>();
        }
        return std::make_unique<OpenGLRhiCommandEncoder>(*this, queueType);
    }

    bool OpenGLGraphicsDevice::SubmitCommandEncoder(IRhiCommandEncoder& encoder, RhiQueueType queueType)
    {
        auto* glEncoder = dynamic_cast<OpenGLRhiCommandEncoder*>(&encoder);
        if (!glEncoder || glEncoder->QueueType() != queueType || !m_context || !wglMakeCurrent(m_hdc, m_context)) {
            return false;
        }
        glFlush();
        return true;
    }
} // namespace SasamiRenderer

#endif
