#include "Renderer/Backends/OpenGL/OpenGLGraphicsDevice.h"
#include "Renderer/Backends/OpenGL/OpenGLShaderSources.h"
#include "Renderer/Backends/OpenGL/OpenGLProcs.h"

#if RHI_OPENGL

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"
#include "Renderer/Capture/PngWriter.h"
#include "Renderer/Structures/Skeleton.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include <GL/gl.h>

namespace SasamiEngine::Render
{
    namespace
    {






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

        void BeginRenderPass(const RhiRenderPassDesc& desc) override
        {
            if (!MakeCurrent() || desc.colorAttachmentCount > 8 ||
                (desc.colorAttachmentCount > 0 && !desc.colorAttachments)) {
                return;
            }
            auto genFramebuffers = LoadGlProc<GlGenFramebuffersFn>("glGenFramebuffers");
            auto bindFramebuffer = LoadGlProc<GlBindFramebufferFn>("glBindFramebuffer");
            auto framebufferTexture2D = LoadGlProc<GlFramebufferTexture2DFn>("glFramebufferTexture2D");
            auto drawBuffers = LoadGlProc<GlDrawBuffersFn>("glDrawBuffers");
            auto clearBufferfv = LoadGlProc<GlClearBufferfvFn>("glClearBufferfv");
            auto checkFramebufferStatus = LoadGlProc<GlCheckFramebufferStatusFn>("glCheckFramebufferStatus");
            if (!genFramebuffers || !bindFramebuffer || !framebufferTexture2D || !drawBuffers) {
                return;
            }
            if (m_framebuffer == 0) {
                genFramebuffers(1, &m_framebuffer);
            }
            bindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);

            std::array<GLenum, 8> drawAttachments{};
            for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i) {
                const auto textureIt = m_device.m_rhiTextures.find(desc.colorAttachments[i].texture.id);
                const GLuint texture = textureIt != m_device.m_rhiTextures.end() ? textureIt->second : 0;
                framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, texture, 0);
                drawAttachments[i] = GL_COLOR_ATTACHMENT0 + i;
            }
            for (uint32_t i = desc.colorAttachmentCount; i < m_colorAttachmentCount; ++i) {
                framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, 0, 0);
            }
            m_colorAttachmentCount = desc.colorAttachmentCount;
            if (desc.colorAttachmentCount > 0) {
                drawBuffers(static_cast<GLsizei>(desc.colorAttachmentCount), drawAttachments.data());
            } else {
                glDrawBuffer(GL_NONE);
                glReadBuffer(GL_NONE);
            }

            GLuint depthTexture = 0;
            if (desc.depthStencilAttachment) {
                const auto depthIt = m_device.m_rhiTextures.find(desc.depthStencilAttachment->texture.id);
                if (depthIt != m_device.m_rhiTextures.end()) {
                    depthTexture = depthIt->second;
                }
            }
            framebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthTexture, 0);
            if (checkFramebufferStatus && checkFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                DebugLog("OpenGLRhiCommandEncoder::BeginRenderPass: framebuffer is incomplete.\n");
                return;
            }

            for (uint32_t i = 0; i < desc.colorAttachmentCount; ++i) {
                const RhiAttachmentDesc& attachment = desc.colorAttachments[i];
                if (attachment.loadOp != RhiLoadOp::Clear) {
                    continue;
                }
                const GLfloat color[4] = {
                    attachment.clearColor.r,
                    attachment.clearColor.g,
                    attachment.clearColor.b,
                    attachment.clearColor.a,
                };
                if (clearBufferfv) {
                    clearBufferfv(GL_COLOR, static_cast<GLint>(i), color);
                } else if (i == 0) {
                    glClearColor(color[0], color[1], color[2], color[3]);
                    glClear(GL_COLOR_BUFFER_BIT);
                }
            }
            if (desc.depthStencilAttachment && desc.depthStencilAttachment->loadOp == RhiLoadOp::Clear) {
                const GLfloat depth = desc.depthStencilAttachment->clearDepthStencil.depth;
                if (clearBufferfv) {
                    clearBufferfv(GL_DEPTH, 0, &depth);
                } else {
                    glClearDepth(depth);
                    glClear(GL_DEPTH_BUFFER_BIT);
                }
            }
            m_renderPassActive = true;
        }

        void EndRenderPass() override
        {
            if (!MakeCurrent() || !m_renderPassActive) {
                return;
            }
            auto bindFramebuffer = LoadGlProc<GlBindFramebufferFn>("glBindFramebuffer");
            if (bindFramebuffer) {
                bindFramebuffer(GL_FRAMEBUFFER, 0);
            }
            m_renderPassActive = false;
        }

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

        void UavBarrier(const RhiResourceHandle*, uint32_t) override
        {
            // GL has no per-resource UAV barrier; a global memory barrier covers SSBO and image writes.
            if (!MakeCurrent()) {
                return;
            }
            auto memoryBarrier = LoadGlProc<GlMemoryBarrierFn>("glMemoryBarrier");
            if (memoryBarrier) {
                memoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            }
        }

        void SetGraphicsDescriptorTable(uint32_t slot, RhiGpuDescriptorHandle table) override
        {
            if (!MakeCurrent() || !table.IsValid()) {
                return;
            }
            const auto it = m_device.m_rhiTextureViews.find(table.ptr);
            if (it != m_device.m_rhiTextureViews.end()) {
                auto activeTexture = LoadGlProc<GlActiveTextureFn>("glActiveTexture");
                if (activeTexture) {
                    activeTexture(GL_TEXTURE0 + slot);
                }
                glBindTexture(it->second.target, it->second.texture);
                return;
            }

            const auto bufferIt = m_device.m_rhiBufferViews.find(table.ptr);
            if (bufferIt != m_device.m_rhiBufferViews.end()) {
                auto bindBufferBase = LoadGlProc<GlBindBufferBaseFn>("glBindBufferBase");
                if (bindBufferBase) {
                    bindBufferBase(bufferIt->second.target, slot, bufferIt->second.buffer);
                }
            }
        }

        void SetComputeDescriptorTable(uint32_t slot, RhiGpuDescriptorHandle table) override
        {
            const auto uavIt = m_device.m_rhiTextureUavs.find(table.ptr);
            const auto textureIt = m_device.m_rhiTextureViews.find(table.ptr);
            if (MakeCurrent() && uavIt != m_device.m_rhiTextureUavs.end() && textureIt != m_device.m_rhiTextureViews.end()) {
                auto bindImageTexture = LoadGlProc<GlBindImageTextureFn>("glBindImageTexture");
                if (bindImageTexture) {
                    bindImageTexture(slot, textureIt->second.texture, 0, GL_FALSE, 0, GL_READ_WRITE,
                                     static_cast<GLenum>(ToGlInternalFormat(uavIt->second)));
                    return;
                }
            }
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
                        texture = it->second.texture;
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
                    depthTexture = it->second.texture;
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

        void CopyBuffer(const RhiBufferCopyDesc& copy) override
        {
            if (!MakeCurrent() || copy.sizeInBytes == 0) {
                return;
            }
            const auto sourceIt = m_device.m_rhiBuffers.find(copy.source.id);
            const auto destinationIt = m_device.m_rhiBuffers.find(copy.destination.id);
            const auto sourceSizeIt = m_device.m_rhiBufferSizes.find(copy.source.id);
            const auto destinationSizeIt = m_device.m_rhiBufferSizes.find(copy.destination.id);
            if (sourceIt == m_device.m_rhiBuffers.end() || destinationIt == m_device.m_rhiBuffers.end() ||
                sourceSizeIt == m_device.m_rhiBufferSizes.end() || destinationSizeIt == m_device.m_rhiBufferSizes.end() ||
                copy.sourceOffsetInBytes >= sourceSizeIt->second ||
                copy.sizeInBytes > sourceSizeIt->second - copy.sourceOffsetInBytes ||
                copy.destinationOffsetInBytes >= destinationSizeIt->second ||
                copy.sizeInBytes > destinationSizeIt->second - copy.destinationOffsetInBytes) {
                return;
            }
            auto bindBuffer = LoadGlProc<GlBindBufferFn>("glBindBuffer");
            auto copyBufferSubData = LoadGlProc<GlCopyBufferSubDataFn>("glCopyBufferSubData");
            if (!bindBuffer || !copyBufferSubData) {
                return;
            }
            bindBuffer(GL_COPY_READ_BUFFER, sourceIt->second);
            bindBuffer(GL_COPY_WRITE_BUFFER, destinationIt->second);
            copyBufferSubData(GL_COPY_READ_BUFFER,
                              GL_COPY_WRITE_BUFFER,
                              static_cast<std::ptrdiff_t>(copy.sourceOffsetInBytes),
                              static_cast<std::ptrdiff_t>(copy.destinationOffsetInBytes),
                              static_cast<std::ptrdiff_t>(copy.sizeInBytes));
            bindBuffer(GL_COPY_READ_BUFFER, 0);
            bindBuffer(GL_COPY_WRITE_BUFFER, 0);
        }

    private:
        bool MakeCurrent() const
        {
            return m_device.m_hdc && m_device.m_context && wglMakeCurrent(m_device.m_hdc, m_device.m_context);
        }

        void SetGraphicsPipelineLayout(RhiPipelineLayoutHandle handle) override
        {
            if (!MakeCurrent()) return;
            const auto it = m_device.m_rhiPipelineLayouts.find(handle.id);
            auto bindSampler = LoadGlProc<GlBindSamplerFn>("glBindSampler");
            if (it == m_device.m_rhiPipelineLayouts.end() || !bindSampler) return;
            for (const auto& sampler : it->second.staticSamplers) {
                const uint32_t flags = static_cast<uint32_t>(sampler.visibility);
                if (flags & static_cast<uint32_t>(RhiShaderStageFlags::AllGraphics)) {
                    bindSampler(sampler.shaderRegister, sampler.sampler);
                }
            }
        }

        void SetComputePipelineLayout(RhiPipelineLayoutHandle handle) override
        {
            if (!MakeCurrent()) return;
            const auto it = m_device.m_rhiPipelineLayouts.find(handle.id);
            auto bindSampler = LoadGlProc<GlBindSamplerFn>("glBindSampler");
            if (it == m_device.m_rhiPipelineLayouts.end() || !bindSampler) return;
            for (const auto& sampler : it->second.staticSamplers) {
                if (static_cast<uint32_t>(sampler.visibility) & static_cast<uint32_t>(RhiShaderStageFlags::Compute)) {
                    bindSampler(sampler.shaderRegister, sampler.sampler);
                }
            }
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
        uint32_t m_colorAttachmentCount = 0;
        bool m_renderPassActive = false;
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
        m_capabilities.supportsComputeQueue = true;
        m_capabilities.supportsSwapChain = true;
        m_capabilities.supportsNativeFrame = true;
        m_capabilities.supportsFeatureRenderPasses = false;
        m_capabilities.supportsD3D12CompatibilitySurface = false;
        m_capabilities.supportsRhiResourceCreation = true;
        m_capabilities.supportsRhiDescriptorCreation = true;
        m_capabilities.supportsRhiPipelineCreation = true;
        m_capabilities.supportsRhiCommandEncoding = true;
        // Single GL context bound to one thread; no threaded resource/pipeline creation.
        m_capabilities.supportsThreadedResourceCreation = false;
        m_capabilities.supportsThreadedPipelineCreation = false;
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

    bool OpenGLGraphicsDevice::GetOverlayDeviceContext(RhiOverlayDeviceContext& context)
    {
        if (!m_hdc || !m_context) {
            return false;
        }

        // ImGui's OpenGL backend resolves its own function pointers and draws into whatever
        // context is current, so no device/queue handles need to travel through here.
        context.api = RhiBackendApi::OpenGL;
        return true;
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


    bool OpenGLGraphicsDevice::ExecuteBackendFrame(const RhiBackendFrameDesc& frameDesc)
    {
        if (!m_hdc || !m_context || !frameDesc.present) {
            return false;
        }
        if (!wglMakeCurrent(m_hdc, m_context)) {
            return false;
        }

        static bool clipControlResolved = false;
        static GlClipControlFn clipControl = nullptr;
        if (!clipControlResolved) {
            clipControl = LoadGlProc<GlClipControlFn>("glClipControl");
            clipControlResolved = true;
        }
        if (clipControl) {
            // Map clip z [-1,1] to depth [0,1] so reverse-Z's z in [0,w] uses the full
            // depth range (near=1.0 / far=0.0), matching the DX12 path's precision.
            clipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        }
        // Recorded because the native SSAO pass reconstructs a world position from a sampled depth,
        // which is only valid under the [0,1] mapping above; it skips generation otherwise.
        m_depthZeroToOne = clipControl != nullptr;
        // else: no glClipControl available. Clip z stays mapped to depth [0,1] via the
        // legacy [-1,1] transform, so reverse-Z's z in [0,w] only reaches depth [0.5,1.0]
        // (near=1.0 / far=0.5). Still monotonic, so GEQUAL + clear-to-0 renders correctly
        // with just half the depth precision.

        glClearColor(frameDesc.clearColor.r,
                     frameDesc.clearColor.g,
                     frameDesc.clearColor.b,
                     frameDesc.clearColor.a);
        glClearDepth(0.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (frameDesc.rayMarch.enabled) {
            (void)RenderRayMarchFrame(frameDesc.rayMarch);
        } else if (frameDesc.mesh.enabled) {
            // Generates the screen-space occlusion both mesh passes below sample as runtimeAo, so
            // it has to run before them. It renders into its own framebuffers and rebinds the
            // default one, leaving the back buffer cleared above untouched.
            RenderNativeAmbientOcclusion(frameDesc.mesh);
            (void)RenderMeshFrame(frameDesc.mesh);
            (void)RenderSkinnedMeshFrame(frameDesc.mesh);
            // After the mesh passes so it replaces the lit frame rather than being painted over,
            // and before the overlay so the UI stays readable on top. A no-op unless
            // SASAMI_GL_AO_DEBUG=1.
            RenderAoDebugView(frameDesc.mesh);
        }
        if (frameDesc.overlay) {
            // The context is already current from wglMakeCurrent above; the ImGui GL backend
            // draws into whatever is current, so it must not be made current again here.
            RhiOverlayFrameContext overlayContext{};
            overlayContext.api = RhiBackendApi::OpenGL;
            frameDesc.overlay->RecordOverlay(overlayContext);
        }
        if (frameDesc.capture && frameDesc.capture->path) {
            // Before the swap, for the same reason the DirectX 11 path gives: the back buffer is
            // undefined once it has been presented.
            frameDesc.capture->succeeded = CaptureBackBufferToPng(frameDesc.capture->path);
            frameDesc.capture->completed = true;
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

    bool OpenGLGraphicsDevice::ResizeBackendSwapChain(UINT width, UINT height)
    {
        if (!m_hdc || !m_context || width == 0 || height == 0 ||
            !wglMakeCurrent(m_hdc, m_context)) {
            return false;
        }
        glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
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
                if (m_skinnedMeshProgram != 0) {
                    glDeleteProgramPtr(m_skinnedMeshProgram);
                    m_skinnedMeshProgram = 0;
                    m_skinnedMeshMvpLocation = -1;
                    m_skinnedMeshBaseColorLocation = -1;
                    m_skinnedMeshLightDirIntensityLocation = -1;
                    m_skinnedMeshLightColorLocation = -1;
                    m_skinnedMeshEmissiveRoughnessLocation = -1;
                    m_skinnedMeshAlbedoTextureLocation = -1;
                    m_skinnedMeshHasAlbedoTextureLocation = -1;
                    m_skinnedMeshBoneMatricesLocation = -1;
                }
                if (m_rayMarchProgram != 0) {
                    glDeleteProgramPtr(m_rayMarchProgram);
                    m_rayMarchProgram = 0;
                }
                for (const auto& entry : m_rhiPipelines) {
                    if (entry.second.program != 0) {
                        glDeleteProgramPtr(entry.second.program);
                    }
                }
                }
                // Outside the glDeleteProgram guard above: these passes own framebuffers and
                // textures as well as programs, and each resolves the entry points it needs
                // itself, so a missing glDeleteProgram must not keep the rest from being freed.
                ReleaseAoDebugResources();
                ReleaseSsaoResources();
                ReleaseAoPrepassResources();
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
            m_rhiBufferSizes.clear();
            m_rhiBufferTargets.clear();
            m_rhiShaders.clear();
            auto deleteSamplers = LoadGlProc<GlDeleteSamplersFn>("glDeleteSamplers");
            if (deleteSamplers) {
                for (const auto& layout : m_rhiPipelineLayouts) {
                    for (const auto& sampler : layout.second.staticSamplers) {
                        if (sampler.sampler != 0) deleteSamplers(1, &sampler.sampler);
                    }
                }
            }
            m_rhiPipelineLayouts.clear();
            m_rhiPipelines.clear();
            m_rhiTextureViews.clear();
            m_rhiBufferViews.clear();
            m_rhiTextureUavs.clear();
            m_rhiBufferUavs.clear();
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
