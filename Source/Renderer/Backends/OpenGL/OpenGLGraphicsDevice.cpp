#include "Renderer/Backends/OpenGL/OpenGLGraphicsDevice.h"

#if RHI_OPENGL

#include "Foundation/Tools/DebugOutput.h"
#include "Foundation/Math/MathUtil.h"

#include <array>
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
            case RhiFormat::R16Float: return GL_R16F;
            case RhiFormat::R16UNorm: return GL_R16;
            case RhiFormat::R32Float: return GL_R32F;
            case RhiFormat::R32UInt: return GL_R32UI;
            case RhiFormat::R16Typeless:
            case RhiFormat::D16UNorm: return GL_DEPTH_COMPONENT16;
            case RhiFormat::R32Typeless:
            case RhiFormat::D32Float: return GL_DEPTH_COMPONENT32F;
            case RhiFormat::D24UNormS8UInt: return GL_DEPTH24_STENCIL8;
            case RhiFormat::R8G8B8A8UNorm:
            case RhiFormat::B8G8R8A8UNorm:
            default: return GL_RGBA8;
            }
        }

        GLenum ToGlFormat(RhiFormat format)
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

        GLenum ToGlType(RhiFormat format)
        {
            switch (format) {
            case RhiFormat::R16G16B16A16Float:
            case RhiFormat::R16Float:
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

        // ---- Ray march shaders ----
        const char* kOpenGLRayMarchVS = R"GLSL(
#version 330 core
void main()
{
    float x = (gl_VertexID == 1) ?  3.0 : -1.0;
    float y = (gl_VertexID == 2) ?  3.0 : -1.0;
    gl_Position = vec4(x, y, 0.0, 1.0);
}
)GLSL";

        const char* kOpenGLRayMarchFS = R"GLSL(
#version 330 core
out vec4 fragColor;

// Uniforms matching RayMarchCB layout (row-major matrix uploaded un-transposed;
// GLSL reads it as column-major = transposed, so M*v == HLSL mul(v,M_rowmajor))
uniform mat4  u_invVP;
uniform vec3  u_camPos;     uniform float u_time;
uniform vec3  u_sunDir;     uniform float u_sunI;
uniform vec3  u_sunColor;   uniform float u_cloudCover;
uniform float u_renderW;    uniform float u_renderH;
uniform float u_fluidMode;  uniform float u_cloudDensity;
uniform vec4  u_extra0;
uniform vec4  u_extra1;
uniform vec4  u_extra2;

const float kEps      = 0.0005;
const float kMaxDist  = 300.0;
const int   kMaxSteps = 512;
const float PI        = 3.14159265;
const float kFloatOriginCell = 1024.0;

// ---------------------------------------------------------------------------
// Floating Origin helpers
// ---------------------------------------------------------------------------
vec2 CamOrigin()  { return floor(u_camPos.xz / kFloatOriginCell) * kFloatOriginCell; }
vec2 MakeStableXZ(vec2 pWS_xz) { return pWS_xz - CamOrigin(); }
vec2 CamLocalXZ()  { return u_camPos.xz - CamOrigin(); }

// ---------------------------------------------------------------------------
// Wave / fluid helpers
// ---------------------------------------------------------------------------
void waveLod(vec2 p_stable, out float midW, out float hiW)
{
    float dist = length(p_stable - CamLocalXZ());
    midW = clamp(1.0 - (dist - 160.0) / 160.0, 0.0, 1.0);
    hiW  = clamp(1.0 - (dist -  80.0) /  80.0, 0.0, 1.0);
}

float waveHeight(vec2 p, float t)
{
    float midW, hiW;
    waveLod(p, midW, hiW);
    float h = 0.0;
    h += sin(p.x * 1.40 + t * 1.80) * 0.180;
    h += sin(p.y * 1.90 + t * 1.40) * 0.140;
    h += sin(p.x * 0.70 - p.y * 1.10 + t * 0.90) * 0.090;
    h += sin(p.x * 2.80 + p.y * 2.10 - t * 2.30) * 0.048 * midW;
    h += sin(p.x * 4.10 - p.y * 3.30 + t * 3.10) * 0.024 * hiW;
    h += sin(p.x * 6.50 + p.y * 5.20 - t * 4.20) * 0.012 * hiW;
    return h;
}

float sdWater(vec3 p)
{
    return (p.y - waveHeight(MakeStableXZ(p.xz), u_time)) * 0.75;
}

vec3 waveNormal(vec2 p, float t)
{
    float midW, hiW;
    waveLod(p, midW, hiW);
    float dhdx = 0.0, dhdz = 0.0;
    dhdx += cos(p.x * 1.40 + t * 1.80) * 1.40 * 0.180;
    dhdz += cos(p.y * 1.90 + t * 1.40) * 1.90 * 0.140;
    dhdx += cos(p.x * 0.70 - p.y * 1.10 + t * 0.90) *  0.70 * 0.090;
    dhdz += cos(p.x * 0.70 - p.y * 1.10 + t * 0.90) * -1.10 * 0.090;
    dhdx += cos(p.x * 2.80 + p.y * 2.10 - t * 2.30) *  2.80 * 0.048 * midW;
    dhdz += cos(p.x * 2.80 + p.y * 2.10 - t * 2.30) *  2.10 * 0.048 * midW;
    dhdx += cos(p.x * 4.10 - p.y * 3.30 + t * 3.10) *  4.10 * 0.024 * hiW;
    dhdz += cos(p.x * 4.10 - p.y * 3.30 + t * 3.10) * -3.30 * 0.024 * hiW;
    dhdx += cos(p.x * 6.50 + p.y * 5.20 - t * 4.20) *  6.50 * 0.012 * hiW;
    dhdz += cos(p.x * 6.50 + p.y * 5.20 - t * 4.20) *  5.20 * 0.012 * hiW;
    return normalize(vec3(-dhdx, 1.0, -dhdz));
}

// ---------------------------------------------------------------------------
// SDF primitives
// ---------------------------------------------------------------------------
float sdSphere(vec3 p, float r) { return length(p) - r; }

float sdRoundBox(vec3 p, vec3 b, float r)
{
    vec3 q = abs(p) - b;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0) - r;
}

float sdCapsule(vec3 p, vec3 a, vec3 b, float r)
{
    vec3 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h) - r;
}

// ---------------------------------------------------------------------------
// Scene map
// ---------------------------------------------------------------------------
vec2 map(vec3 p)
{
    float bob = sin(u_time * 0.6) * 0.18;
    float dWater  = sdWater(p);
    vec3  pSphere = p - vec3(-1.8, 2.0 + bob, 0.5);
    float dSphere = sdSphere(pSphere, 1.0);
    float angle   = u_time * 0.3;
    vec3  pBox    = p - vec3(2.5, 1.2, -0.3);
    pBox.xz = vec2(pBox.x * cos(angle) + pBox.z * sin(angle),
                  -pBox.x * sin(angle) + pBox.z * cos(angle));
    float dBox    = sdRoundBox(pBox, vec3(0.75, 0.65, 0.75), 0.12);
    vec3  capA    = vec3(-4.2, -0.3,  0.8);
    vec3  capB    = vec3(-4.2,  2.8,  0.8);
    float dCap    = sdCapsule(p, capA, capB, 0.38);
    vec2  res     = vec2(dWater, 0.0);
    if (dSphere < res.x) res = vec2(dSphere, 1.0);
    if (dBox    < res.x) res = vec2(dBox,    2.0);
    if (dCap    < res.x) res = vec2(dCap,    3.0);
    return res;
}

vec3 calcNormal(vec3 p)
{
    vec2 e = vec2(kEps, 0.0);
    return normalize(vec3(
        map(p + e.xyy).x - map(p - e.xyy).x,
        map(p + e.yxy).x - map(p - e.yxy).x,
        map(p + e.yyx).x - map(p - e.yyx).x));
}

float softShadow(vec3 ro, vec3 rd, float mint, float maxt, float k)
{
    float res = 1.0, t = mint;
    for (int i = 0; i < 32; ++i) {
        float h = map(ro + rd * t).x;
        if (h < 0.001) return 0.0;
        res = min(res, k * h / t);
        t  += clamp(h, 0.01, 0.4);
        if (t > maxt) break;
    }
    return clamp(res, 0.0, 1.0);
}

float calcAO(vec3 pos, vec3 nor)
{
    float occ = 0.0, sca = 1.0;
    for (int i = 0; i < 5; i++) {
        float h = 0.01 + 0.12 * float(i) / 4.0;
        float d = map(pos + h * nor).x;
        occ    += (h - d) * sca;
        sca    *= 0.95;
    }
    return clamp(1.0 - 3.0 * occ, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// Cloud / sky
// ---------------------------------------------------------------------------
vec3 GradHash3(vec3 p)
{
    p = vec3(dot(p, vec3(127.1, 311.7,  74.7)),
             dot(p, vec3(269.5, 183.3, 246.1)),
             dot(p, vec3(113.5, 271.9, 124.6)));
    vec3 g = fract(sin(p) * 43758.5453) * 2.0 - 1.0;
    return g / (length(g) + 1e-5);
}

float GradNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    return mix(
        mix(mix(dot(GradHash3(i),                 f),
                dot(GradHash3(i + vec3(1,0,0)), f - vec3(1,0,0)), u.x),
            mix(dot(GradHash3(i + vec3(0,1,0)), f - vec3(0,1,0)),
                dot(GradHash3(i + vec3(1,1,0)), f - vec3(1,1,0)), u.x), u.y),
        mix(mix(dot(GradHash3(i + vec3(0,0,1)), f - vec3(0,0,1)),
                dot(GradHash3(i + vec3(1,0,1)), f - vec3(1,0,1)), u.x),
            mix(dot(GradHash3(i + vec3(0,1,1)), f - vec3(0,1,1)),
                dot(GradHash3(i + vec3(1,1,1)), f - vec3(1,1,1)), u.x), u.y), u.z);
}

float FBM5(vec3 p)
{
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++) {
        v += a * (GradNoise(p) * 0.5 + 0.5);
        p  = p * 2.02 + vec3(73.1, 61.4, 53.7);
        a *= 0.5;
    }
    return v;
}

float SampleCloudDensity(vec3 worldPos, float time, float cloudCover, float cloudDensity)
{
    const float heightScale = 4.0;
    vec3 p = vec3(worldPos.x, worldPos.y * heightScale, worldPos.z) * 0.0001;
    p.x += time * 0.008;
    p.z += time * 0.003;
    float base   = FBM5(p * 0.8);
    float detail = FBM5(p * 2.5 + vec3(1.4, 2.1, 0.9) + time * 0.002) * 0.35;
    float raw    = base + detail - (1.0 - cloudCover);
    return max(raw * cloudDensity, 0.0);
}

vec4 MarchClouds(vec3 rd, vec3 sunDir, vec3 sunColor, float sunIntensity,
                 float time, float cloudCover, float cloudDensity, float jitter)
{
    const float kCloudBaseY = 1500.0;
    const float kCloudTopY  = 5000.0;
    if (rd.y < 0.005) return vec4(0.0);
    float tBase = (kCloudBaseY - u_camPos.y) / rd.y;
    float tTop  = (kCloudTopY  - u_camPos.y) / rd.y;
    if (tTop <= 0.0) return vec4(0.0);
    tBase = max(tBase, 0.0);
    if (tBase > 300000.0) return vec4(0.0);

    const int kSteps = 48;
    float dt = (tTop - tBase) / float(kSteps);
    float g = 0.6, g2 = g * g;
    float cosSun = dot(rd, sunDir);
    float denom  = max(1.0 + g2 - 2.0 * g * cosSun, 1e-6);
    float phaseHG = (1.0 - g2) / (4.0 * PI * pow(denom, 1.5));

    float transmittance = 1.0;
    vec3  scattered     = vec3(0.0);

    for (int i = 0; i < kSteps; i++) {
        if (transmittance < 0.02) break;
        float  t       = tBase + (float(i) + jitter) * dt;
        vec3   pos     = u_camPos + rd * t;
        float  density = SampleCloudDensity(pos, time, cloudCover, cloudDensity);
        if (density < 1e-4) continue;
        float sigmaE   = density * 0.0008;
        float sampleT  = exp(-sigmaE * dt);
        float sunAtten = exp(-density * 1.2);
        vec3  sunLit   = sunColor * sunIntensity * sunAtten * phaseHG;
        vec3  ambient  = vec3(0.35, 0.50, 0.72) * 0.28;
        vec3  inScatter = (sunLit + ambient) * density;
        scattered += inScatter * transmittance * (1.0 - sampleT) / max(sigmaE, 1e-6);
        transmittance *= sampleT;
    }
    return vec4(scattered, 1.0 - transmittance);
}

vec3 ComputeSkyColor(vec3 rd, vec3 sunDir, vec3 sunColor, float sunIntensity)
{
    float cosTheta  = dot(rd, sunDir);
    float elevation = rd.y;
    vec3 zenithColor  = vec3(0.05, 0.15, 0.65) * 2.0;
    vec3 horizonColor = vec3(0.50, 0.68, 0.88) * 1.5;
    vec3 groundColor  = vec3(0.08, 0.06, 0.04);
    vec3 skyBase;
    if (elevation >= 0.0)
        skyBase = mix(horizonColor, zenithColor, pow(clamp(elevation, 0.0, 1.0), 0.4));
    else
        skyBase = mix(horizonColor, groundColor, clamp(-elevation * 5.0, 0.0, 1.0));
    float sunInfluence = clamp(cosTheta * 0.5 + 0.5, 0.0, 1.0);
    vec3  warmTint = mix(vec3(1.0), sunColor * vec3(1.1, 0.9, 0.7), sunInfluence * 0.4);
    skyBase *= warmTint;
    float mie      = pow(max(cosTheta, 0.0), 6.0) * 0.3;
    vec3  mieColor = sunColor * mie * sunIntensity;
    float horizonFactor = pow(clamp(1.0 - abs(elevation), 0.0, 1.0), 3.0);
    vec3  horizGlow = sunColor
        * mix(vec3(1.0, 0.7, 0.4), vec3(1.0, 0.9, 0.7), sunIntensity)
        * horizonFactor * max(cosTheta, 0.0) * 0.4 * sunIntensity;
    float sunDisc = smoothstep(0.9996, 1.0, cosTheta);
    vec3  disc    = sunColor * sunDisc * 12.0 * sunIntensity;
    return max(skyBase + mieColor + horizGlow + disc, vec3(0.0));
}

vec3 skyColor(vec3 rd)
{
    return ComputeSkyColor(rd, normalize(u_sunDir), u_sunColor, u_sunI);
}

// ---------------------------------------------------------------------------
// Shading
// ---------------------------------------------------------------------------
vec3 shadeWater(vec3 pos, vec3 rd, float tHit)
{
    vec2  stableXZ = CamLocalXZ() + rd.xz * tHit;
    vec3  N        = waveNormal(stableXZ, u_time);
    vec3  V        = -rd;
    vec3  sunDir   = normalize(u_sunDir);
    float NdotV    = clamp(dot(N, V), 0.0, 1.0);
    float fresnel  = 0.02 + 0.98 * pow(1.0 - NdotV, 5.0);
    vec3  reflDir  = reflect(rd, N);
    vec3  reflColor = skyColor(reflDir);
    vec3  deepColor    = vec3(0.00, 0.08, 0.18);
    vec3  shallowColor = vec3(0.04, 0.28, 0.42);
    vec3  waterColor   = mix(deepColor, shallowColor, pow(NdotV, 1.5));
    float wh   = waveHeight(stableXZ, u_time);
    float foam = smoothstep(0.22, 0.40, wh);
    vec3  skyAmb = skyColor(vec3(0.0, 1.0, 0.0)) * 0.20;
    waterColor  += skyAmb * (1.0 - fresnel);
    float caustic = pow(clamp(dot(N, vec3(0.0, 1.0, 0.0)), 0.0, 1.0), 32.0)
                  * smoothstep(0.0, 0.06, wh + 0.06) * 0.4;
    waterColor  += vec3(0.3, 0.6, 0.8) * caustic;
    vec3  H     = normalize(sunDir + V);
    float spec  = pow(max(0.0, dot(N, H)), 512.0) * u_sunI;
    float shadow = softShadow(pos + vec3(0.0, 0.01, 0.0), sunDir, 0.05, 15.0, 8.0);
    vec3  color  = mix(waterColor, reflColor, fresnel);
    color  = mix(color, vec3(0.92, 0.96, 1.0), foam * 0.7);
    color += u_sunColor * spec * 3.5;
    float sunDiff = max(0.0, dot(N, sunDir)) * shadow * u_sunI * (1.0 - foam);
    color += u_sunColor * sunDiff * waterColor * 0.12;
    return color;
}

vec3 solidAlbedo(float matId, vec3 pos)
{
    if (matId < 1.5) return vec3(0.90, 0.35, 0.10);
    if (matId < 2.5) return vec3(0.20, 0.40, 0.75);
    return vec3(0.22, 0.52, 0.18);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
void main()
{
    vec2 uv  = gl_FragCoord.xy / vec2(u_renderW, u_renderH);
    vec2 ndc = uv * 2.0 - 1.0;
    ndc.y    = -ndc.y;

    vec3 ro = u_camPos;
    vec3 rd;
    vec3 right   = u_extra1.xyz;
    vec3 up      = u_extra2.xyz;
    vec3 forward = normalize(cross(right, up));
    if (u_extra0.w > 0.5) {
        rd = normalize(forward + right * (ndc.x * u_extra0.y) + up * (ndc.y * u_extra0.z));
    } else {
        vec4 near4  = u_invVP * vec4(ndc, 0.0, 1.0);
        vec4 far4   = u_invVP * vec4(ndc, 1.0, 1.0);
        vec3 nearWS = near4.xyz / near4.w;
        vec3 farWS  = far4.xyz  / far4.w;
        rd = normalize(farWS - nearWS);
    }

    // Cone marching pixel footprint
    vec2 adjNdc = vec2(ndc.x, ndc.y + 2.0 / u_renderH);
    vec3 adjRd;
    if (u_extra0.w > 0.5) {
        adjRd = normalize(forward + right * (adjNdc.x * u_extra0.y) + up * (adjNdc.y * u_extra0.z));
    } else {
        vec4 near4   = u_invVP * vec4(ndc,    0.0, 1.0);
        vec4 adjFar4 = u_invVP * vec4(adjNdc, 1.0, 1.0);
        vec3 nearWS  = near4.xyz / near4.w;
        adjRd = normalize(adjFar4.xyz / adjFar4.w - nearWS);
    }
    float pixelConeAngle = max(length(rd - adjRd), 1e-5);

    // Ray march
    vec2  camLocal = CamLocalXZ();
    float t        = 0.02;
    float matId    = -1.0;
    for (int i = 0; i < kMaxSteps; ++i) {
        vec3  p        = ro + rd * t;
        vec2  h        = map(p);
        vec2  stableXZ = camLocal + rd.xz * t;
        float dWaterP  = (p.y - waveHeight(stableXZ, u_time)) * 0.75;
        if (h.y < 0.5 || dWaterP < h.x) h = vec2(dWaterP, 0.0);
        bool  isWater  = (h.y < 0.5);
        float epsilon  = isWater ? kEps : max(pixelConeAngle * t * 0.2, kEps);
        if (h.x < epsilon) { matId = h.y; break; }
        t += h.x;
        if (t > kMaxDist) break;
    }

    // Miss: sky + clouds
    if (matId < 0.0) {
        if (u_extra0.x > 0.5) { fragColor = vec4(1.0); return; }
        vec3 sunDir = normalize(u_sunDir);
        vec3 col    = ComputeSkyColor(rd, sunDir, u_sunColor, u_sunI);
        if (rd.y > 0.01 && u_cloudCover > 0.01) {
            float cloudDensity = max(u_cloudDensity, 0.5);
            float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(127.1, 311.7))) * 43758.5453);
            vec4  clouds = MarchClouds(rd, sunDir, u_sunColor, u_sunI,
                                       u_time, u_cloudCover, cloudDensity, jitter);
            col = mix(col, clouds.rgb / max(clouds.a, 1e-4), clamp(clouds.a, 0.0, 1.0));
        }
        fragColor = vec4(col, 1.0);
        return;
    }

    vec3 pos   = ro + rd * t;
    vec3 color;
    if (matId < 0.5) {
        color = shadeWater(pos, rd, t);
    } else {
        vec3  N      = calcNormal(pos);
        vec3  V      = -rd;
        vec3  sunDir = normalize(u_sunDir);
        float ao     = calcAO(pos, N);
        float shadow = softShadow(pos + N * 0.002, sunDir, 0.01, 20.0, 8.0);
        vec3  albedo = solidAlbedo(matId, pos);
        float NdotL  = max(0.0, dot(N, sunDir));
        vec3  diffuse = albedo * u_sunColor * u_sunI * NdotL * shadow;
        vec3  ambient = albedo * skyColor(N) * 0.15 * ao;
        vec3  H      = normalize(sunDir + V);
        vec3  spec   = u_sunColor * u_sunI * pow(max(0.0, dot(N, H)), 48.0) * shadow * 0.3;
        color = diffuse + ambient + spec;
    }

    float fogFactor = 1.0 - exp(-t * 0.007);
    color = mix(color, skyColor(rd) * 0.55, fogFactor);

    if (u_extra0.x > 0.5) {
        float tn = clamp(t / kMaxDist, 0.0, 1.0);
        vec3 dbgCol;
        if      (tn < 0.25) { float s = tn / 0.25;           dbgCol = mix(vec3(0,0,1), vec3(0,1,1), s); }
        else if (tn < 0.50) { float s = (tn - 0.25) / 0.25;  dbgCol = mix(vec3(0,1,1), vec3(0,1,0), s); }
        else if (tn < 0.75) { float s = (tn - 0.50) / 0.25;  dbgCol = mix(vec3(0,1,0), vec3(1,1,0), s); }
        else                { float s = (tn - 0.75) / 0.25;  dbgCol = mix(vec3(1,1,0), vec3(1,0,0), s); }
        fragColor = vec4(dbgCol, 1.0);
        return;
    }

    fragColor = vec4(color, 1.0);
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
                glBindTexture(GL_TEXTURE_2D, it->second);
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
                    bindImageTexture(slot, textureIt->second, 0, GL_FALSE, 0, GL_READ_WRITE,
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
        const GLuint fs = compile(GL_FRAGMENT_SHADER, kOpenGLRayMarchFS);
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
        if (frameDesc.rayMarch.enabled) {
            (void)RenderRayMarchFrame(frameDesc.rayMarch);
        } else if (frameDesc.mesh.enabled) {
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

    bool OpenGLGraphicsDevice::ResizeBackendSwapChain(UINT width, UINT height)
    {
        if (!m_hdc || !m_context || width == 0 || height == 0 ||
            !wglMakeCurrent(m_hdc, m_context)) {
            return false;
        }
        glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        return true;
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
                if (it->second == texture) {
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
        m_rhiTextureViews[destination.ptr] = it->second;
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
