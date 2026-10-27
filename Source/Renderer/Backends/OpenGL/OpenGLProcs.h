#pragma once

// The OpenGL entry points and enums the backend needs, shared by every OpenGL translation unit.
//
// Windows ships only the GL 1.1 headers and no loader, so everything newer has to be declared
// here and fetched through wglGetProcAddress at the call site. These lived in the anonymous
// namespace of OpenGLGraphicsDevice.cpp, which is why that file could not be split by
// responsibility: any piece moved out lost the typedefs it needed.
//
// Each enum is guarded, so a platform whose real GL headers already define it wins.

#include "Renderer/RHI/RhiTypes.h"

#include <windows.h>
#include <GL/gl.h>

namespace SasamiEngine::Render
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
#ifndef GL_SRGB8_ALPHA8
    constexpr GLint GL_SRGB8_ALPHA8 = 0x8C43;
#endif
#ifndef GL_HALF_FLOAT
    constexpr GLenum GL_HALF_FLOAT = 0x140B;
#endif
#ifndef GL_RGBA16F
    constexpr GLint GL_RGBA16F = 0x881A;
#endif
#ifndef GL_R16F
    constexpr GLint GL_R16F = 0x822D;
#endif
#ifndef GL_R16
    constexpr GLint GL_R16 = 0x822A;
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
#ifndef GL_R32UI
    constexpr GLint GL_R32UI = 0x8236;
#endif
#ifndef GL_RED_INTEGER
    constexpr GLenum GL_RED_INTEGER = 0x8D94;
#endif
#ifndef GL_DEPTH_COMPONENT32F
    constexpr GLint GL_DEPTH_COMPONENT32F = 0x8CAC;
#endif
#ifndef GL_DEPTH_COMPONENT16
    constexpr GLint GL_DEPTH_COMPONENT16 = 0x81A5;
#endif
#ifndef GL_DEPTH24_STENCIL8
    constexpr GLint GL_DEPTH24_STENCIL8 = 0x88F0;
#endif
#ifndef GL_DEPTH_STENCIL
    constexpr GLenum GL_DEPTH_STENCIL = 0x84F9;
#endif
#ifndef GL_UNSIGNED_INT_24_8
    constexpr GLenum GL_UNSIGNED_INT_24_8 = 0x84FA;
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
#ifndef GL_UNIFORM_BUFFER
    constexpr GLenum GL_UNIFORM_BUFFER = 0x8A11;
#endif
#ifndef GL_SHADER_IMAGE_ACCESS_BARRIER_BIT
    constexpr GLbitfield GL_SHADER_IMAGE_ACCESS_BARRIER_BIT = 0x00000020;
#endif
#ifndef GL_SHADER_STORAGE_BARRIER_BIT
    constexpr GLbitfield GL_SHADER_STORAGE_BARRIER_BIT = 0x00002000;
#endif
#ifndef GL_SHADER_STORAGE_BUFFER
    constexpr GLenum GL_SHADER_STORAGE_BUFFER = 0x90D2;
#endif
#ifndef GL_COPY_READ_BUFFER
    constexpr GLenum GL_COPY_READ_BUFFER = 0x8F36;
#endif
#ifndef GL_COPY_WRITE_BUFFER
    constexpr GLenum GL_COPY_WRITE_BUFFER = 0x8F37;
#endif
#ifndef GL_READ_WRITE
    constexpr GLenum GL_READ_WRITE = 0x88BA;
#endif
#ifndef GL_CLAMP_TO_EDGE
    constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
#endif
    // GL_TEXTURE_WRAP_R (the 3rd cube-sampling axis) is GL 1.2; the other wrap modes above are
    // already declared the same way, so this follows suit rather than leaving a 3rd axis unnamed.
#ifndef GL_TEXTURE_WRAP_R
    constexpr GLenum GL_TEXTURE_WRAP_R = 0x8072;
#endif
    // Cube maps arrived in GL 1.3, past the GL 1.1 headers Windows ships, so these are declared
    // here the same way as the other post-1.1 constants above.
#ifndef GL_TEXTURE_CUBE_MAP
    constexpr GLenum GL_TEXTURE_CUBE_MAP = 0x8513;
#endif
#ifndef GL_TEXTURE_CUBE_MAP_POSITIVE_X
    constexpr GLenum GL_TEXTURE_CUBE_MAP_POSITIVE_X = 0x8515;
#endif
    // GL_TEXTURE_CUBE_MAP_SEAMLESS is GL 3.2; guarded the same way so enabling it is a no-op
    // constant definition on headers that already have it.
#ifndef GL_TEXTURE_CUBE_MAP_SEAMLESS
    constexpr GLenum GL_TEXTURE_CUBE_MAP_SEAMLESS = 0x884F;
#endif
    // 3D textures are GL 1.2, also past the Windows GL 1.1 headers.
#ifndef GL_TEXTURE_3D
    constexpr GLenum GL_TEXTURE_3D = 0x806F;
#endif
#ifndef GL_LOWER_LEFT
    constexpr GLenum GL_LOWER_LEFT = 0x8CA1;
#endif
    // Windows ships only the GL 1.1 headers, and BGRA arrived in 1.2, so it is defined here
    // the same way as the other constants above.
#ifndef GL_BGRA
    constexpr GLenum GL_BGRA = 0x80E1;
#endif
#ifndef GL_ZERO_TO_ONE
    constexpr GLenum GL_ZERO_TO_ONE = 0x935F;
#endif

    using GlGenBuffersFn = void (APIENTRY*)(GLsizei, GLuint*);
    using GlBindBufferFn = void (APIENTRY*)(GLenum, GLuint);
    using GlBindBufferBaseFn = void (APIENTRY*)(GLenum, GLuint, GLuint);
    using GlBufferDataFn = void (APIENTRY*)(GLenum, std::ptrdiff_t, const void*, GLenum);
    using GlBufferSubDataFn = void (APIENTRY*)(GLenum, std::ptrdiff_t, std::ptrdiff_t, const void*);
    using GlGetBufferSubDataFn = void (APIENTRY*)(GLenum, std::ptrdiff_t, std::ptrdiff_t, void*);
    using GlCopyBufferSubDataFn = void (APIENTRY*)(GLenum, GLenum, std::ptrdiff_t, std::ptrdiff_t, std::ptrdiff_t);
    using GlBindImageTextureFn = void (APIENTRY*)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
    using GlGenSamplersFn = void (APIENTRY*)(GLsizei, GLuint*);
    using GlDeleteSamplersFn = void (APIENTRY*)(GLsizei, const GLuint*);
    using GlSamplerParameteriFn = void (APIENTRY*)(GLuint, GLenum, GLint);
    using GlBindSamplerFn = void (APIENTRY*)(GLuint, GLuint);
    using GlDeleteBuffersFn = void (APIENTRY*)(GLsizei, const GLuint*);
    using GlGenFramebuffersFn = void (APIENTRY*)(GLsizei, GLuint*);
    using GlBindFramebufferFn = void (APIENTRY*)(GLenum, GLuint);
    using GlFramebufferTexture2DFn = void (APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
    using GlCheckFramebufferStatusFn = GLenum (APIENTRY*)(GLenum);
    using GlDeleteFramebuffersFn = void (APIENTRY*)(GLsizei, const GLuint*);
    using GlDrawBuffersFn = void (APIENTRY*)(GLsizei, const GLenum*);
    using GlClearBufferfvFn = void (APIENTRY*)(GLenum, GLint, const GLfloat*);
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
    using GlUniform3fvFn = void (APIENTRY*)(GLint, GLsizei, const GLfloat*);
    using GlUniform1fFn  = void (APIENTRY*)(GLint, GLfloat);
    using GlUniform2fFn  = void (APIENTRY*)(GLint, GLfloat, GLfloat);
    using GlUniform1iFn = void (APIENTRY*)(GLint, GLint);
    using GlEnableVertexAttribArrayFn = void (APIENTRY*)(GLuint);
    using GlDisableVertexAttribArrayFn = void (APIENTRY*)(GLuint);
    using GlVertexAttribPointerFn = void (APIENTRY*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
    using GlDispatchComputeFn = void (APIENTRY*)(GLuint, GLuint, GLuint);
    using GlMemoryBarrierFn = void (APIENTRY*)(GLbitfield);
    using GlDrawArraysInstancedFn = void (APIENTRY*)(GLenum, GLint, GLsizei, GLsizei);
    using GlDrawElementsInstancedBaseVertexFn = void (APIENTRY*)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLint);
    using GlDrawElementsInstancedFn = void (APIENTRY*)(GLenum, GLsizei, GLenum, const void*, GLsizei);
    using GlClipControlFn = void (APIENTRY*)(GLenum, GLenum);
    using GlTexImage3DFn = void (APIENTRY*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum,
                                            const void*);

    // Resolved per call rather than cached: wglGetProcAddress is tied to the current context, and
    // the backend makes contexts current on demand.

    template <typename Fn>
    Fn LoadGlProc(const char* name)
    {
        return reinterpret_cast<Fn>(wglGetProcAddress(name));
    }
    // RhiFormat and friends translated to GL enums. Shared for the same reason as the entry
    // points above: the resource file cannot create a texture without them.
    inline GLint ToGlInternalFormat(RhiFormat format)
    {
        switch (format) {
        case RhiFormat::R8UNorm: return GL_R8;
        case RhiFormat::R16G16B16A16Float: return GL_RGBA16F;
        case RhiFormat::R32G32B32Float: return GL_RGB32F;
        case RhiFormat::R32G32Float: return GL_RG32F;
        case RhiFormat::R16Float: return GL_R16F;
        case RhiFormat::R16UNorm: return GL_R16;
        case RhiFormat::R32Float: return GL_R32F;
        case RhiFormat::R32UInt: return GL_R32UI;
        case RhiFormat::R16Typeless:
        case RhiFormat::D16UNorm: return GL_DEPTH_COMPONENT16;
        case RhiFormat::R32Typeless:
        case RhiFormat::D32Float: return GL_DEPTH_COMPONENT32F;
        case RhiFormat::D24UNormS8UInt: return GL_DEPTH24_STENCIL8;
        case RhiFormat::R8G8B8A8UNormSrgb: return GL_SRGB8_ALPHA8;
        case RhiFormat::R8G8B8A8UNorm:
        case RhiFormat::B8G8R8A8UNorm:
        default: return GL_RGBA8;
        }
    }
    inline GLenum ToGlFormat(RhiFormat format)
    {
        switch (format) {
        case RhiFormat::R8UNorm:
        case RhiFormat::R16Float:
        case RhiFormat::R16UNorm:
        case RhiFormat::R32Float:
            return GL_RED;
        case RhiFormat::R32UInt:
            return GL_RED_INTEGER;
        case RhiFormat::R32G32B32Float:
            return GL_RGB;
        case RhiFormat::R32G32Float:
            return GL_RG;
        case RhiFormat::R16Typeless:
        case RhiFormat::R32Typeless:
        case RhiFormat::D16UNorm:
        case RhiFormat::D32Float:
            return GL_DEPTH_COMPONENT;
        case RhiFormat::D24UNormS8UInt:
            return GL_DEPTH_STENCIL;
        default:
            return GL_RGBA;
        }
    }
    inline GLenum ToGlType(RhiFormat format)
    {
        switch (format) {
        // 16-bit float formats carry half floats, not 32-bit floats. Returning GL_FLOAT here made
        // glTexImage2D read 4 bytes per channel from a buffer holding 2, so an RGBA16F upload read
        // twice its own size (8 MB past the end for a 1024 face) and produced garbage.
        case RhiFormat::R16G16B16A16Float:
        case RhiFormat::R16Float:
            return GL_HALF_FLOAT;
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
        case RhiFormat::R32UInt:
            return GL_UNSIGNED_INT;
        case RhiFormat::D24UNormS8UInt:
            return GL_UNSIGNED_INT_24_8;
        default:
            return GL_UNSIGNED_BYTE;
        }
    }
    inline GLenum ToGlBufferTarget(RhiBufferUsageFlags usage)
    {
        return HasFlag(usage, RhiBufferUsageFlags::Index) ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
    }
    inline GLenum ToGlShaderStage(RhiShaderStageFlags stage)
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

}
