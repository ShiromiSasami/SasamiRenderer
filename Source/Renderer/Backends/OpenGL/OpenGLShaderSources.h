#pragma once

// The GLSL sources for the OpenGL backend, kept apart from the device that compiles them.
// They are plain data with no dependency on the GL entry points, and they were more than a
// fifth of OpenGLGraphicsDevice.cpp on their own.
//
// The shaders are GLSL 1.20 where they feed the fixed native mesh path (that profile has no
// gl_VertexID), and #version 330 core where they draw a fullscreen triangle from the vertex id
// alone. The two groups cannot be merged for that reason.

namespace SasamiEngine::Render
{
    // Native static mesh, GLSL 1.20.
    extern const char* const kOpenGLNativeMeshVS;

    // Native static mesh fragment stage, with the alpha-test discard.
    extern const char* const kOpenGLNativeMeshPS;

    // Native skinned mesh, GLSL 1.20, bone matrices as uniforms.
    extern const char* const kOpenGLNativeSkinnedMeshVS;

    // Fullscreen triangle from gl_VertexID; needs #version 330.
    extern const char* const kOpenGLRayMarchVS;

    // Ray-march fragment stage, first half (spliced around the sky).
    extern const char* const kOpenGLRayMarchFS_Part1;

    // ComputeSkyColor, shared by the ray-march background and the native sky.
    // The canonical version is HLSL (Shaders/Effects/Sky/ProceduralSky/ProceduralSky.hlsli);
    // this is its GLSL translation, deliberately kept as ONE copy that both users splice in.
    extern const char* const kOpenGLSkyFunctionGLSL;

    // Ray-march fragment stage, second half.
    extern const char* const kOpenGLRayMarchFS_Part2;

    // Native sky: the same fullscreen-triangle trick, #version 330.
    extern const char* const kOpenGLNativeSkyVS;

    // Native sky fragment stage, uniforms (spliced around the sky function).
    extern const char* const kOpenGLNativeSkyFS_Part1;

    // Native sky fragment stage, the body.
    extern const char* const kOpenGLNativeSkyFS_Part2;

    // Depth + world-normal prepass for the SSAO inputs the native path has no GBuffer to supply.
    // GLSL 1.20, sharing the mesh programs' vertex layout.
    extern const char* const kOpenGLAoPrepassVS;

    // Skinned variant of the prepass vertex stage, bone matrices as uniforms.
    extern const char* const kOpenGLAoPrepassSkinnedVS;

    // Prepass fragment stage, shared by both vertex stages: encoded world normal + alpha clip.
    extern const char* const kOpenGLAoPrepassFS;

    // Full-screen triangle for the SSAO pass; needs gl_VertexID, hence #version 330.
    extern const char* const kOpenGLSsaoVS;

    // SSAO fragment stage, a translation of Shaders/Effects/AmbientOcclusion/SSAO/SSAO_PS.hlsl.
    extern const char* const kOpenGLSsaoFS;

    // Depth- and normal-aware blur over the raw SSAO, a translation of SSAO_Blur_PS.hlsl.
    extern const char* const kOpenGLSsaoBlurFS;

    // Grey-scale ambient-occlusion view for SASAMI_GL_AO_DEBUG=1.
    extern const char* const kOpenGLAoDebugFS;

}
