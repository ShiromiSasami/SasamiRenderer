#include "Renderer/Backends/OpenGL/OpenGLProgramBuilder.h"
// Pulled in for RHI_OPENGL itself: OpenGLProcs.h only reaches RhiTypes.h, which does not define
// the backend switches, so without this the guard below would compile the file away to nothing.
#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Backends/OpenGL/OpenGLProcs.h"

#if RHI_OPENGL

#include "Foundation/Tools/DebugOutput.h"

namespace SasamiEngine::Render
{
    namespace
    {
        void LogStageFailure(const char* debugName, const char* stage, const char* log)
        {
            DebugLog("BuildGlProgram: ");
            DebugLog(debugName != nullptr ? debugName : "<unnamed>");
            DebugLog(" ");
            DebugLog(stage);
            DebugLog(" failed:\n");
            DebugLog(log);
            DebugLog("\n");
        }
    }

    unsigned int BuildGlProgram(const char*               vertexSource,
                                const char*               fragmentSource,
                                const GlAttributeBinding* attributes,
                                std::size_t               attributeCount,
                                const char*               debugName)
    {
        if (vertexSource == nullptr || fragmentSource == nullptr) {
            return 0;
        }

        auto createShader      = LoadGlProc<GlCreateShaderFn>("glCreateShader");
        auto shaderSource      = LoadGlProc<GlShaderSourceFn>("glShaderSource");
        auto compileShader     = LoadGlProc<GlCompileShaderFn>("glCompileShader");
        auto getShaderiv       = LoadGlProc<GlGetShaderivFn>("glGetShaderiv");
        auto getShaderInfoLog  = LoadGlProc<GlGetShaderInfoLogFn>("glGetShaderInfoLog");
        auto deleteShader      = LoadGlProc<GlDeleteShaderFn>("glDeleteShader");
        auto createProgram     = LoadGlProc<GlCreateProgramFn>("glCreateProgram");
        auto attachShader      = LoadGlProc<GlAttachShaderFn>("glAttachShader");
        auto bindAttribLocation = LoadGlProc<GlBindAttribLocationFn>("glBindAttribLocation");
        auto linkProgram       = LoadGlProc<GlLinkProgramFn>("glLinkProgram");
        auto getProgramiv      = LoadGlProc<GlGetProgramivFn>("glGetProgramiv");
        auto getProgramInfoLog = LoadGlProc<GlGetProgramInfoLogFn>("glGetProgramInfoLog");
        auto deleteProgram     = LoadGlProc<GlDeleteProgramFn>("glDeleteProgram");
        if (!createShader || !shaderSource || !compileShader || !getShaderiv || !deleteShader ||
            !createProgram || !attachShader || !linkProgram || !getProgramiv) {
            LogStageFailure(debugName, "entry-point resolve", "required GL shader functions are unavailable");
            return 0;
        }
        if (attributeCount > 0 && !bindAttribLocation) {
            LogStageFailure(debugName, "entry-point resolve", "glBindAttribLocation is unavailable");
            return 0;
        }

        const auto compile = [&](GLenum stage, const char* source) -> GLuint {
            const GLuint shader = createShader(stage);
            shaderSource(shader, 1, &source, nullptr);
            compileShader(shader);
            GLint ok = GL_FALSE;
            getShaderiv(shader, GL_COMPILE_STATUS, &ok);
            if (ok == GL_FALSE) {
                if (getShaderInfoLog) {
                    char    log[2048] = {};
                    GLsizei length    = 0;
                    getShaderInfoLog(shader, static_cast<GLsizei>(sizeof(log)), &length, log);
                    LogStageFailure(debugName,
                                    stage == GL_VERTEX_SHADER ? "vertex compile" : "fragment compile",
                                    log);
                }
                deleteShader(shader);
                return 0;
            }
            return shader;
        };

        const GLuint vs = compile(GL_VERTEX_SHADER, vertexSource);
        const GLuint fs = compile(GL_FRAGMENT_SHADER, fragmentSource);
        if (vs == 0 || fs == 0) {
            if (vs != 0) { deleteShader(vs); }
            if (fs != 0) { deleteShader(fs); }
            return 0;
        }

        const GLuint program = createProgram();
        attachShader(program, vs);
        attachShader(program, fs);
        for (std::size_t i = 0; i < attributeCount; ++i) {
            if (attributes[i].name != nullptr) {
                bindAttribLocation(program, attributes[i].index, attributes[i].name);
            }
        }
        linkProgram(program);
        // The shaders are no longer needed once linked; the program holds its own copy.
        deleteShader(fs);
        deleteShader(vs);

        GLint linked = GL_FALSE;
        getProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked == GL_FALSE) {
            if (getProgramInfoLog) {
                char    log[2048] = {};
                GLsizei length    = 0;
                getProgramInfoLog(program, static_cast<GLsizei>(sizeof(log)), &length, log);
                LogStageFailure(debugName, "link", log);
            }
            if (deleteProgram) {
                deleteProgram(program);
            }
            return 0;
        }
        return program;
    }
}

#endif // RHI_OPENGL
