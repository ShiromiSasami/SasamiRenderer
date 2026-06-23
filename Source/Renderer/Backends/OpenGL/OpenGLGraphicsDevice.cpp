#include "Renderer/Backends/OpenGL/OpenGLGraphicsDevice.h"

#if RHI_OPENGL

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <GL/gl.h>

namespace SasamiRenderer
{
    namespace
    {
#ifndef GL_ARRAY_BUFFER
        constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
#endif
#ifndef GL_ELEMENT_ARRAY_BUFFER
        constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
#endif
#ifndef GL_STATIC_DRAW
        constexpr GLenum GL_STATIC_DRAW = 0x88E4;
#endif
#ifndef GL_DYNAMIC_DRAW
        constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8;
#endif
#ifndef GL_R8
        constexpr GLint GL_R8 = 0x8229;
#endif
#ifndef GL_RGBA8
        constexpr GLint GL_RGBA8 = 0x8058;
#endif
#ifndef GL_RGBA16F
        constexpr GLint GL_RGBA16F = 0x881A;
#endif
#ifndef GL_RED
        constexpr GLenum GL_RED = 0x1903;
#endif
#ifndef GL_RG
        constexpr GLenum GL_RG = 0x8227;
#endif
#ifndef GL_RG32F
        constexpr GLint GL_RG32F = 0x8230;
#endif
#ifndef GL_RGB32F
        constexpr GLint GL_RGB32F = 0x8815;
#endif
#ifndef GL_RGB
        constexpr GLenum GL_RGB = 0x1907;
#endif
#ifndef GL_R32F
        constexpr GLint GL_R32F = 0x822E;
#endif
#ifndef GL_DEPTH_COMPONENT32F
        constexpr GLint GL_DEPTH_COMPONENT32F = 0x8CAC;
#endif
#ifndef GL_VERTEX_SHADER
        constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
#endif
#ifndef GL_FRAGMENT_SHADER
        constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
#endif
#ifndef GL_GEOMETRY_SHADER
        constexpr GLenum GL_GEOMETRY_SHADER = 0x8DD9;
#endif
#ifndef GL_TESS_CONTROL_SHADER
        constexpr GLenum GL_TESS_CONTROL_SHADER = 0x8E88;
#endif
#ifndef GL_TESS_EVALUATION_SHADER
        constexpr GLenum GL_TESS_EVALUATION_SHADER = 0x8E87;
#endif
#ifndef GL_COMPUTE_SHADER
        constexpr GLenum GL_COMPUTE_SHADER = 0x91B9;
#endif
#ifndef GL_COMPILE_STATUS
        constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
#endif
#ifndef GL_LINK_STATUS
        constexpr GLenum GL_LINK_STATUS = 0x8B82;
#endif
#ifndef GL_INFO_LOG_LENGTH
        constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;
#endif
#ifndef GL_FRAMEBUFFER
        constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
#endif
#ifndef GL_COLOR_ATTACHMENT0
        constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
#endif
#ifndef GL_DEPTH_ATTACHMENT
        constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
        constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
#endif
#ifndef GL_TEXTURE0
        constexpr GLenum GL_TEXTURE0 = 0x84C0;
#endif

        using GlGenBuffersFn = void (APIENTRY*)(GLsizei, GLuint*);
        using GlBindBufferFn = void (APIENTRY*)(GLenum, GLuint);
        using GlBufferDataFn = void (APIENTRY*)(GLenum, std::ptrdiff_t, const void*, GLenum);
        using GlDeleteBuffersFn = void (APIENTRY*)(GLsizei, const GLuint*);
        using GlGenFramebuffersFn = void (APIENTRY*)(GLsizei, GLuint*);
        using GlBindFramebufferFn = void (APIENTRY*)(GLenum, GLuint);
        using GlFramebufferTexture2DFn = void (APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
        using GlCheckFramebufferStatusFn = GLenum (APIENTRY*)(GLenum);
        using GlDeleteFramebuffersFn = void (APIENTRY*)(GLsizei, const GLuint*);
        using GlActiveTextureFn = void (APIENTRY*)(GLenum);
        using GlCreateShaderFn = GLuint (APIENTRY*)(GLenum);
        using GlShaderSourceFn = void (APIENTRY*)(GLuint, GLsizei, const char* const*, const GLint*);
        using GlCompileShaderFn = void (APIENTRY*)(GLuint);
        using GlGetShaderivFn = void (APIENTRY*)(GLuint, GLenum, GLint*);
        using GlGetShaderInfoLogFn = void (APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
        using GlDeleteShaderFn = void (APIENTRY*)(GLuint);
        using GlCreateProgramFn = GLuint (APIENTRY*)();
        using GlAttachShaderFn = void (APIENTRY*)(GLuint, GLuint);
        using GlLinkProgramFn = void (APIENTRY*)(GLuint);
        using GlGetProgramivFn = void (APIENTRY*)(GLuint, GLenum, GLint*);
        using GlGetProgramInfoLogFn = void (APIENTRY*)(GLuint, GLsizei, GLsizei*, char*);
        using GlDeleteProgramFn = void (APIENTRY*)(GLuint);
        using GlUseProgramFn = void (APIENTRY*)(GLuint);
        using GlBindAttribLocationFn = void (APIENTRY*)(GLuint, GLuint, const char*);
        using GlGetUniformLocationFn = GLint (APIENTRY*)(GLuint, const char*);
        using GlUniformMatrix4fvFn = void (APIENTRY*)(GLint, GLsizei, GLboolean, const GLfloat*);
        using GlUniform4fvFn = void (APIENTRY*)(GLint, GLsizei, const GLfloat*);
        using GlUniform1iFn = void (APIENTRY*)(GLint, GLint);
        using GlEnableVertexAttribArrayFn = void (APIENTRY*)(GLuint);
        using GlDisableVertexAttribArrayFn = void (APIENTRY*)(GLuint);
        using GlVertexAttribPointerFn = void (APIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
        using GlDispatchComputeFn = void (APIENTRY*)(GLuint, GLuint, GLuint);
        using GlDrawArraysInstancedFn = void (APIENTRY*)(GLenum, GLint, GLsizei, GLsizei);
        using GlDrawElementsInstancedBaseVertexFn = void (APIENTRY*)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLint);
        using GlDrawElementsInstancedFn = void (APIENTRY*)(GLenum, GLsizei, GLenum, const void*, GLsizei);

        GLint ToGlInternalFormat(RhiFormat format)
        {
            switch (format) {
            case RhiFormat::R8UNorm: return GL_R8;
            case RhiFormat::R16G16B16A16Float: return GL_RGBA16F;
            case RhiFormat::R32G32B32Float: return GL_RGB32F;
            case RhiFormat::R32G32Float: return GL_RG32F;
            case RhiFormat::R16UNorm: return GL_R16;
            case RhiFormat::R32Float: return GL_R32F;
            case RhiFormat::R16Typeless:
            case RhiFormat::D16UNorm: return GL_DEPTH_COMPONENT16;
            case RhiFormat::R32Typeless:
            case RhiFormat::D32Float: return GL_DEPTH_COMPONENT32F;
            case RhiFormat::R8G8B8A8UNorm:
            case RhiFormat::B8G8R8A8UNorm:
            default: return GL_RGBA8;
            }
        }

        GLenum ToGlFormat(RhiFormat format)
        {
            switch (format) {
            case RhiFormat::R8UNorm:
            case RhiFormat::R16UNorm:
            case RhiFormat::R32Float:
                return GL_RED;
            case RhiFormat::R32G32B32Float:
                return GL_RGB;
            case RhiFormat::R32G32Float:
                return GL_RG;
            case RhiFormat::R16Typeless:
            case RhiFormat::R32Typeless:
            case RhiFormat::D16UNorm:
            case RhiFormat::D32Float:
                return GL_DEPTH_COMPONENT;
            default:
                return GL_RGBA;
            }
        }

        GLenum ToGlType(RhiFormat format)
        {
            switch (format) {
            case RhiFormat::R16G16B16A16Float:
            case RhiFormat::R32G32B32Float:
            case RhiFormat::R32G32Float:
            case RhiFormat::R32Float:
            case RhiFormat::R32Typeless:
            case RhiFormat::D32Float:
                return GL_FLOAT;
            case RhiFormat::R16UNorm:
            case RhiFormat::R16Typeless:
            case RhiFormat::D16UNorm:
                return GL_UNSIGNED_SHORT;
            default:
                return GL_UNSIGNED_BYTE;
            }
        }

        GLenum ToGlBufferTarget(RhiBufferUsageFlags usage)
        {
            return HasFlag(usage, RhiBufferUsageFlags::Index) ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
        }

        GLenum ToGlShaderStage(RhiShaderStageFlags stage)
        {
            switch (stage) {
            case RhiShaderStageFlags::Vertex: return GL_VERTEX_SHADER;
            case RhiShaderStageFlags::Hull: return GL_TESS_CONTROL_SHADER;
            case RhiShaderStageFlags::Domain: return GL_TESS_EVALUATION_SHADER;
            case RhiShaderStageFlags::Geometry: return GL_GEOMETRY_SHADER;
            case RhiShaderStageFlags::Pixel: return GL_FRAGMENT_SHADER;
            case RhiShaderStageFlags::Compute: return GL_COMPUTE_SHADER;
            default: return 0;
            }
        }

        template <typename Fn>
        Fn LoadGlProc(const char* name)
        {
            return reinterpret_cast<Fn>(wglGetProcAddress(name));
        }

        GLenum ToGlPrimitiveMode(RhiPrimitiveTopology topology)
        {
            switch (topology) {
            case RhiPrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
            case RhiPrimitiveTopology::LineList: return GL_LINES;
            case RhiPrimitiveTopology::LineStrip: return GL_LINE_STRIP;
            case RhiPrimitiveTopology::PointList: return GL_POINTS;
            case RhiPrimitiveTopology::TriangleList:
            case RhiPrimitiveTopology::PatchList:
            default: return GL_TRIANGLES;
            }
        }

        const char* kOpenGLNativeMeshVS = R"GLSL(
#version 120
attribute vec3 a_position;
attribute vec3 a_normal;
attribute vec4 a_color;
attribute vec2 a_uv;
uniform mat4 u_modelViewProjection;
uniform vec4 u_baseColor;
varying vec3 v_normal;
varying vec4 v_color;
varying vec2 v_uv;
void main()
{
    gl_Position = u_modelViewProjection * vec4(a_position, 1.0);
    v_normal = normalize(a_normal);
    v_color = a_color * u_baseColor;
    v_uv = a_uv;
}
)GLSL";

        const char* kOpenGLNativeMeshPS = R"GLSL(
#version 120
uniform vec4 u_lightDirIntensity;
uniform vec4 u_lightColor;
uniform vec4 u_emissiveRoughness;
uniform sampler2D u_albedoTexture;
uniform int u_hasAlbedoTexture;
varying vec3 v_normal;
varying vec4 v_color;
varying vec2 v_uv;
void main()
{
    vec3 n = normalize(v_normal);
    vec3 l = normalize(-u_lightDirIntensity.xyz);
    float ndotl = max(dot(n, l), 0.0);
    float ambient = 0.16;
    vec4 surface = v_color;
    if (u_hasAlbedoTexture != 0) {
        surface *= texture2D(u_albedoTexture, v_uv);
    }
    vec3 lit = surface.rgb * u_lightColor.rgb * (ambient + ndotl * u_lightDirIntensity.w);
    lit += u_emissiveRoughness.rgb;
    lit = lit / (lit + vec3(1.0));
    lit = pow(clamp(lit, 0.0, 1.0), vec3(1.0 / 2.2));
    gl_FragColor = vec4(lit, surface.a);
}
)GLSL";
    }

    class OpenGLRhiCommandEncoder final : public IRhiCommandEncoder
    {
    public:
        OpenGLRhiCommandEncoder(OpenGLGraphicsDevice& device, RhiQueueType queueType)
            : m_device(device)
            , m_queueType(queueType)
        {
        }

        ~OpenGLRhiCommandEncoder() override
        {
            if (m_framebuffer != 0 && MakeCurrent()) {
                auto deleteFramebuffers = LoadGlProc<GlDeleteFramebuffersFn>("glDeleteFramebuffers");
                if (deleteFramebuffers) {
                    deleteFramebuffers(1, &m_framebuffer);
                }
            }
        }

        RhiQueueType QueueType() const { return m_queueType; }

        void SetGraphicsPipeline(RhiPipelineHandle pipelineHandle) override
        {
            BindProgram(pipelineHandle);
        }

        void SetComputePipeline(RhiPipelineHandle pipelineHandle) override
        {
            BindProgram(pipelineHandle);
        }

        void SetPrimitiveTopology(RhiPrimitiveTopology topology) override
        {
            m_primitiveMode = ToGlPrimitiveMode(topology);
        }

        void SetViewports(const RhiViewport* viewports, uint32_t count) override
        {
            if (!MakeCurrent() || !viewports || count == 0) {
                return;
            }
            const RhiViewport& viewport = viewports[0];
            glViewport(static_cast<GLint>(viewport.x),
                       static_cast<GLint>(viewport.y),
                       static_cast<GLsizei>(viewport.width),
                       static_cast<GLsizei>(viewport.height));
        }

        void SetScissors(const RhiRect* scissors, uint32_t count) override
        {
            if (!MakeCurrent() || !scissors || count == 0) {
                return;
            }
            const RhiRect& rect = scissors[0];
            glEnable(GL_SCISSOR_TEST);
            glScissor(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
        }

        void Draw(const RhiDrawDesc& draw) override
        {
            if (!MakeCurrent()) {
                return;
            }
            auto drawArraysInstanced = LoadGlProc<GlDrawArraysInstancedFn>("glDrawArraysInstanced");
            if (drawArraysInstanced) {
                drawArraysInstanced(m_primitiveMode,
                                    static_cast<GLint>(draw.startVertex),
                                    static_cast<GLsizei>(draw.vertexCount),
                                    static_cast<GLsizei>(draw.instanceCount));
                return;
            }
            glDrawArrays(m_primitiveMode,
                         static_cast<GLint>(draw.startVertex),
                         static_cast<GLsizei>(draw.vertexCount));
        }

        void DrawIndexed(const RhiDrawIndexedDesc& draw) override
        {
            if (!MakeCurrent()) {
                return;
            }

            const auto indexOffset = reinterpret_cast<const void*>(static_cast<uintptr_t>(draw.startIndex) * sizeof(uint32_t));
            auto drawBaseVertex = LoadGlProc<GlDrawElementsInstancedBaseVertexFn>("glDrawElementsInstancedBaseVertex");
            if (drawBaseVertex) {
                drawBaseVertex(m_primitiveMode,
                               static_cast<GLsizei>(draw.indexCount),
                               GL_UNSIGNED_INT,
                               indexOffset,
                               static_cast<GLsizei>(draw.instanceCount),
                               draw.baseVertex);
                return;
            }
            auto drawInstanced = LoadGlProc<GlDrawElementsInstancedFn>("glDrawElementsInstanced");
            if (drawInstanced) {
                drawInstanced(m_primitiveMode,
                              static_cast<GLsizei>(draw.indexCount),
                              GL_UNSIGNED_INT,
                              indexOffset,
                              static_cast<GLsizei>(draw.instanceCount));
                return;
            }
            glDrawElements(m_primitiveMode, static_cast<GLsizei>(draw.indexCount), GL_UNSIGNED_INT, indexOffset);
        }

        void Dispatch(const RhiDispatchDesc& dispatch) override
        {
            if (!MakeCurrent()) {
                return;
            }
            auto dispatchCompute = LoadGlProc<GlDispatchComputeFn>("glDispatchCompute");
            if (dispatchCompute) {
                dispatchCompute(dispatch.groupCountX, dispatch.groupCountY, dispatch.groupCountZ);
            }
        }

        void SetGraphicsDescriptorTable(uint32_t slot, RhiGpuDescriptorHandle table) override
        {
            if (!MakeCurrent() || !table.IsValid()) {
                return;
            }
            const auto it = m_device.m_rhiTextureViews.find(table.ptr);
            if (it == m_device.m_rhiTextureViews.end()) {
                return;
            }
            auto activeTexture = LoadGlProc<GlActiveTextureFn>("glActiveTexture");
            if (activeTexture) {
                activeTexture(GL_TEXTURE0 + slot);
            }
            glBindTexture(GL_TEXTURE_2D, it->second);
        }

        void SetComputeDescriptorTable(uint32_t slot, RhiGpuDescriptorHandle table) override
        {
            SetGraphicsDescriptorTable(slot, table);
        }

        void SetRenderTargets(uint32_t numRtvs,
                              const RhiCpuDescriptorHandle* rtvs,
                              const RhiCpuDescriptorHandle* dsv = nullptr) override
        {
            if (!MakeCurrent()) {
                return;
            }

            auto genFramebuffers = LoadGlProc<GlGenFramebuffersFn>("glGenFramebuffers");
            auto bindFramebuffer = LoadGlProc<GlBindFramebufferFn>("glBindFramebuffer");
            auto framebufferTexture2D = LoadGlProc<GlFramebufferTexture2DFn>("glFramebufferTexture2D");
            auto checkFramebufferStatus = LoadGlProc<GlCheckFramebufferStatusFn>("glCheckFramebufferStatus");
            if (!genFramebuffers || !bindFramebuffer || !framebufferTexture2D) {
                return;
            }

            if (m_framebuffer == 0) {
                genFramebuffers(1, &m_framebuffer);
            }
            bindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);

            for (uint32_t i = 0; i < numRtvs; ++i) {
                GLuint texture = 0;
                if (rtvs) {
                    const auto it = m_device.m_rhiTextureViews.find(rtvs[i].ptr);
                    if (it != m_device.m_rhiTextureViews.end()) {
                        texture = it->second;
                    }
                }
                framebufferTexture2D(GL_FRAMEBUFFER,
                                      GL_COLOR_ATTACHMENT0 + i,
                                      GL_TEXTURE_2D,
                                      texture,
                                      0);
            }

            GLuint depthTexture = 0;
            if (dsv && dsv->IsValid()) {
                const auto it = m_device.m_rhiTextureViews.find(dsv->ptr);
                if (it != m_device.m_rhiTextureViews.end()) {
                    depthTexture = it->second;
                }
            }
            framebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthTexture, 0);

            if (checkFramebufferStatus && checkFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                DebugLog("OpenGLRhiCommandEncoder::SetRenderTargets: framebuffer is incomplete.\n");
            }
        }

        void ClearRenderTarget(RhiCpuDescriptorHandle, const RhiClearColor& color) override
        {
            if (MakeCurrent()) {
                glClearColor(color.r, color.g, color.b, color.a);
                glClear(GL_COLOR_BUFFER_BIT);
            }
        }

        void ClearDepthStencil(RhiCpuDescriptorHandle, float depth, uint8_t) override
        {
            if (MakeCurrent()) {
                glClearDepth(static_cast<GLdouble>(depth));
                glClear(GL_DEPTH_BUFFER_BIT);
            }
        }

        void SetVertexBuffers(uint32_t, uint32_t count, const RhiVertexBufferView* views) override
        {
            if (!MakeCurrent() || !views || count == 0) {
                return;
            }
            auto bindBuffer = LoadGlProc<GlBindBufferFn>("glBindBuffer");
            if (!bindBuffer) {
                return;
            }
            const auto it = m_device.m_rhiBuffers.find(views[0].gpuAddress);
            if (it != m_device.m_rhiBuffers.end()) {
                bindBuffer(GL_ARRAY_BUFFER, it->second);
            }
        }

        void SetIndexBuffer(const RhiIndexBufferView& view) override
        {
            if (!MakeCurrent() || view.gpuAddress == 0) {
                return;
            }
            auto bindBuffer = LoadGlProc<GlBindBufferFn>("glBindBuffer");
            if (!bindBuffer) {
                return;
            }
            const auto it = m_device.m_rhiBuffers.find(view.gpuAddress);
            if (it != m_device.m_rhiBuffers.end()) {
                bindBuffer(GL_ELEMENT_ARRAY_BUFFER, it->second);
            }
        }

        void SetVertexBufferBindings(uint32_t, uint32_t count, const RhiVertexBufferBinding* bindings) override
        {
            if (!MakeCurrent() || !bindings || count == 0) {
                return;
            }
            auto bindBuffer = LoadGlProc<GlBindBufferFn>("glBindBuffer");
            if (!bindBuffer) {
                return;
            }
            const auto it = m_device.m_rhiBuffers.find(bindings[0].buffer.id);
            if (it != m_device.m_rhiBuffers.end()) {
                bindBuffer(GL_ARRAY_BUFFER, it->second);
            }
        }

        void SetIndexBufferBinding(const RhiIndexBufferBinding& binding) override
        {
            if (!MakeCurrent() || !binding.buffer.IsValid()) {
                return;
            }
            auto bindBuffer = LoadGlProc<GlBindBufferFn>("glBindBuffer");
            if (!bindBuffer) {
                return;
            }
            const auto it = m_device.m_rhiBuffers.find(binding.buffer.id);
            if (it != m_device.m_rhiBuffers.end()) {
                bindBuffer(GL_ELEMENT_ARRAY_BUFFER, it->second);
            }
        }

    private:
        bool MakeCurrent() const
        {
            return m_device.m_hdc && m_device.m_context && wglMakeCurrent(m_device.m_hdc, m_device.m_context);
        }

        void BindProgram(RhiPipelineHandle pipelineHandle)
        {
            if (!MakeCurrent()) {
                return;
            }
            const auto it = m_device.m_rhiPipelines.find(pipelineHandle.id);
            if (it == m_device.m_rhiPipelines.end()) {
                return;
            }
            auto useProgram = LoadGlProc<GlUseProgramFn>("glUseProgram");
            if (useProgram) {
                useProgram(it->second.program);
            }
        }

        OpenGLGraphicsDevice& m_device;
        RhiQueueType m_queueType = RhiQueueType::Graphics;
        GLenum m_primitiveMode = GL_TRIANGLES;
        GLuint m_framebuffer = 0;
    };

    OpenGLGraphicsDevice::~OpenGLGraphicsDevice()
    {
        Cleanup();
    }

    bool OpenGLGraphicsDevice::Initialize(HWND hWnd, UINT, UINT, UINT)
    {
        Cleanup();
        m_hwnd = hWnd;
        m_hdc = GetDC(hWnd);
        if (!m_hdc) {
            DebugLog("OpenGLGraphicsDevice::Initialize: GetDC failed.\n");
            return false;
        }

        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        pfd.cStencilBits = 8;
        pfd.iLayerType = PFD_MAIN_PLANE;

        const int pixelFormat = ChoosePixelFormat(m_hdc, &pfd);
        if (pixelFormat == 0) {
            DebugLog("OpenGLGraphicsDevice::Initialize: ChoosePixelFormat failed.\n");
            Cleanup();
            return false;
        }
        if (!SetPixelFormat(m_hdc, pixelFormat, &pfd)) {
            DebugLog("OpenGLGraphicsDevice::Initialize: SetPixelFormat failed.\n");
            Cleanup();
            return false;
        }

        m_context = wglCreateContext(m_hdc);
        if (!m_context) {
            DebugLog("OpenGLGraphicsDevice::Initialize: wglCreateContext failed.\n");
            Cleanup();
            return false;
        }
        if (!wglMakeCurrent(m_hdc, m_context)) {
            DebugLog("OpenGLGraphicsDevice::Initialize: wglMakeCurrent failed.\n");
            Cleanup();
            return false;
        }

        m_capabilities = {};
        m_capabilities.api = RhiBackendApi::OpenGL;
        m_capabilities.supportsGraphicsQueue = true;
        m_capabilities.supportsSwapChain = true;
        m_capabilities.supportsNativeFrame = true;
        m_capabilities.supportsFeatureRenderPasses = false;
        m_capabilities.supportsD3D12CompatibilitySurface = false;
        m_capabilities.supportsRhiResourceCreation = true;
        m_capabilities.supportsRhiDescriptorCreation = true;
        m_capabilities.supportsRhiPipelineCreation = true;
        m_capabilities.supportsRhiCommandEncoding = true;
        return true;
    }

    GraphicsRuntime OpenGLGraphicsDevice::GetBackend() const
    {
        return GraphicsRuntime::OpenGL;
    }

    void* OpenGLGraphicsDevice::GetNativeDeviceHandle() const
    {
        return m_context;
    }

    void* OpenGLGraphicsDevice::GetNativeGraphicsQueueHandle() const
    {
        return m_hdc;
    }

    ID3D12Device* OpenGLGraphicsDevice::GetDevice() const
    {
        return nullptr;
    }

    ID3D12Device5* OpenGLGraphicsDevice::GetRayTracingDevice() const
    {
        return nullptr;
    }

    const RhiBackendCapabilities& OpenGLGraphicsDevice::GetCapabilities() const
    {
        return m_capabilities;
    }

    bool OpenGLGraphicsDevice::SupportsHardwareRayTracing() const
    {
        return false;
    }

    CommandQueue& OpenGLGraphicsDevice::GetCommandQueue()
    {
        return m_emptyGraphicsQueue;
    }

    CommandQueue& OpenGLGraphicsDevice::GetComputeQueue()
    {
        return m_emptyComputeQueue;
    }

    SwapChain& OpenGLGraphicsDevice::GetSwapChain()
    {
        return m_emptySwapChain;
    }

    UINT OpenGLGraphicsDevice::GetDescriptorHandleIncrementSize(DescriptorHeapType) const
    {
        return 0;
    }

    void OpenGLGraphicsDevice::WaitForGPU()
    {
        if (m_context) {
            glFinish();
        }
    }

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
        return true;
    }

    bool OpenGLGraphicsDevice::RenderMeshFrame(const RhiBackendMeshFrameDesc& desc)
    {
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
        auto activeTexture = LoadGlProc<GlActiveTextureFn>("glActiveTexture");
        if (!useProgram || !bindBuffer || !enableVertexAttribArray || !disableVertexAttribArray ||
            !vertexAttribPointer || !uniformMatrix4fv || !uniform4fv || !uniform1i) {
            return false;
        }

        glViewport(0, 0, static_cast<GLsizei>(desc.renderWidth), static_cast<GLsizei>(desc.renderHeight));
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        useProgram(m_meshProgram);
        if (activeTexture) {
            activeTexture(GL_TEXTURE0);
        }
        uniform1i(m_meshAlbedoTextureLocation, 0);

        for (uint32_t i = 0; i < desc.drawCount; ++i) {
            const RhiBackendMeshDrawDesc& draw = desc.draws[i];
            const auto vbIt = m_rhiBuffers.find(draw.vertexBufferHandle);
            if (vbIt == m_rhiBuffers.end()) {
                continue;
            }

            float mvp[16] = {};
            Math::Mul4x4(draw.model, desc.viewProjection, mvp);
            uniformMatrix4fv(m_meshMvpLocation, 1, GL_TRUE, mvp);
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
            const auto textureIt = m_rhiTextureViews.find(draw.albedoSrv);
            if (textureIt != m_rhiTextureViews.end()) {
                albedoTexture = textureIt->second;
            }
            glBindTexture(GL_TEXTURE_2D, albedoTexture);
            uniform1i(m_meshHasAlbedoTextureLocation, albedoTexture != 0 ? 1 : 0);

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
        }

        disableVertexAttribArray(3);
        disableVertexAttribArray(2);
        disableVertexAttribArray(1);
        disableVertexAttribArray(0);
        bindBuffer(GL_ARRAY_BUFFER, 0);
        bindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        useProgram(0);
        return true;
    }

    bool OpenGLGraphicsDevice::ExecuteBackendFrame(const RhiBackendFrameDesc& frameDesc)
    {
        if (!m_hdc || !m_context || !frameDesc.present) {
            return false;
        }
        if (!wglMakeCurrent(m_hdc, m_context)) {
            return false;
        }
        glClearColor(frameDesc.clearColor.r,
                     frameDesc.clearColor.g,
                     frameDesc.clearColor.b,
                     frameDesc.clearColor.a);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (frameDesc.mesh.enabled) {
            (void)RenderMeshFrame(frameDesc.mesh);
        }
        glFlush();
        return SwapBuffers(m_hdc) != FALSE;
    }

    bool OpenGLGraphicsDevice::RenderBackendClearFrame(const float clearColor[4])
    {
        RhiBackendFrameDesc frameDesc{};
        if (clearColor) {
            frameDesc.clearColor = { clearColor[0], clearColor[1], clearColor[2], clearColor[3] };
        }
        frameDesc.present = true;
        return ExecuteBackendFrame(frameDesc);
    }

    RhiTextureHandle OpenGLGraphicsDevice::CreateRhiTexture2DFromRgba8(uint32_t width,
                                                                       uint32_t height,
                                                                       const void* pixels,
                                                                       uint32_t rowPitchBytes)
    {
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
                     GL_RGBA8,
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
        return RhiBufferHandle{ id };
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
        const uint64_t id = m_nextRhiPipelineLayoutHandle++;
        m_rhiPipelineLayouts[id] = desc.bindingCount;
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

    RhiPipelineHandle OpenGLGraphicsDevice::CreateRhiComputePipeline(const RhiComputePipelineDesc&)
    {
        return {};
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


    HRESULT OpenGLGraphicsDevice::CreateDescriptorHeap(const DescriptorHeapDesc&, DescriptorHeap&)
    {
        return E_NOTIMPL;
    }

    HRESULT OpenGLGraphicsDevice::CreateCommittedResource(const HeapProperties*,
                                                          HeapFlags,
                                                          const ResourceDesc*,
                                                          ResourceState,
                                                          const ClearValue*,
                                                          Resource&)
    {
        return E_NOTIMPL;
    }

    HRESULT OpenGLGraphicsDevice::CreateCommandAllocator(CommandListType, CommandAllocator&)
    {
        return E_NOTIMPL;
    }

    HRESULT OpenGLGraphicsDevice::CreateCommandList(UINT, CommandListType, CommandAllocator&, PipelineState*, CommandList&)
    {
        return E_NOTIMPL;
    }

    HRESULT OpenGLGraphicsDevice::CreateGraphicsPipelineState(const GraphicsPipelineDesc&, PipelineState&)
    {
        return E_NOTIMPL;
    }

    HRESULT OpenGLGraphicsDevice::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC&, PipelineState&)
    {
        return E_NOTIMPL;
    }

    HRESULT OpenGLGraphicsDevice::CreatePipelineStateFromStream(const void*, size_t, PipelineState&)
    {
        return E_NOTIMPL;
    }

    HRESULT OpenGLGraphicsDevice::CreateRootSignature(UINT, const void*, size_t, RootSignature&)
    {
        return E_NOTIMPL;
    }

    void OpenGLGraphicsDevice::CreateShaderResourceView(Resource&, const ShaderResourceViewDesc*, CpuDescriptorHandle)
    {
    }

    void OpenGLGraphicsDevice::CreateDepthStencilView(Resource&, const DepthStencilViewDesc*, CpuDescriptorHandle)
    {
    }

    void OpenGLGraphicsDevice::CreateRenderTargetView(Resource&, const D3D12_RENDER_TARGET_VIEW_DESC*, CpuDescriptorHandle)
    {
    }

    void OpenGLGraphicsDevice::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC*, CpuDescriptorHandle)
    {
    }

    void OpenGLGraphicsDevice::CreateSampler(const D3D12_SAMPLER_DESC*, CpuDescriptorHandle)
    {
    }

    HRESULT OpenGLGraphicsDevice::CreateFence(UINT64, D3D12_FENCE_FLAGS, ID3D12Fence**)
    {
        return E_NOTIMPL;
    }

    void OpenGLGraphicsDevice::Cleanup()
    {
        if (m_context) {
            if (wglMakeCurrent(m_hdc, m_context)) {
                for (const auto& entry : m_rhiTextures) {
                    const GLuint texture = entry.second;
                    glDeleteTextures(1, &texture);
                }
                auto glDeleteBuffersPtr = reinterpret_cast<GlDeleteBuffersFn>(wglGetProcAddress("glDeleteBuffers"));
                if (glDeleteBuffersPtr) {
                    for (const auto& entry : m_rhiBuffers) {
                        const GLuint buffer = entry.second;
                        glDeleteBuffersPtr(1, &buffer);
                    }
                }
            auto glDeleteProgramPtr = LoadGlProc<GlDeleteProgramFn>("glDeleteProgram");
            if (glDeleteProgramPtr) {
                if (m_meshProgram != 0) {
                    glDeleteProgramPtr(m_meshProgram);
                    m_meshProgram = 0;
                    m_meshMvpLocation = -1;
                    m_meshBaseColorLocation = -1;
                    m_meshLightDirIntensityLocation = -1;
                    m_meshLightColorLocation = -1;
                    m_meshEmissiveRoughnessLocation = -1;
                    m_meshAlbedoTextureLocation = -1;
                    m_meshHasAlbedoTextureLocation = -1;
                }
                for (const auto& entry : m_rhiPipelines) {
                    if (entry.second.program != 0) {
                        glDeleteProgramPtr(entry.second.program);
                    }
                }
                }
                auto glDeleteShaderPtr = LoadGlProc<GlDeleteShaderFn>("glDeleteShader");
                if (glDeleteShaderPtr) {
                    for (const auto& entry : m_rhiShaders) {
                        if (entry.second.shader != 0) {
                            glDeleteShaderPtr(entry.second.shader);
                        }
                    }
                }
            }
            m_rhiTextures.clear();
            m_rhiBuffers.clear();
            m_rhiShaders.clear();
            m_rhiPipelineLayouts.clear();
            m_rhiPipelines.clear();
            m_rhiTextureViews.clear();
            m_nextRhiResourceHandle = 1;
            m_nextRhiDescriptorHandle = 1;
            m_nextRhiShaderHandle = 1;
            m_nextRhiPipelineLayoutHandle = 1;
            m_nextRhiPipelineHandle = 1;
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(m_context);
            m_context = nullptr;
        }
        if (m_hdc && m_hwnd) {
            ReleaseDC(m_hwnd, m_hdc);
            m_hdc = nullptr;
        }
        m_hwnd = nullptr;
        m_capabilities = {};
    }
}

#endif
