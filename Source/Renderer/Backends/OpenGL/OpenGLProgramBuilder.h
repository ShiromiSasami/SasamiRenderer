#pragma once

// Compiles and links a GL program from a pair of GLSL sources.
//
// Every OpenGL translation unit that owns a program used to carry its own copy of the same
// compile-report-link-report sequence; the two ambient-occlusion passes would have made six. The
// sequence has no per-pass variation beyond the sources and the attribute bindings, so it lives
// here once.

#include <cstddef>

namespace SasamiEngine::Render
{
    // One generic vertex attribute pinned to a location before linking. The native mesh path
    // addresses attributes by index (glVertexAttribPointer(0..5, ...)), so a program sharing that
    // vertex layout has to agree on the indices rather than let the linker choose them.
    struct GlAttributeBinding
    {
        unsigned int index = 0;
        const char*  name  = nullptr;
    };

    // Returns the linked program, or 0 after logging the compiler or linker output. `debugName`
    // only prefixes that log.
    unsigned int BuildGlProgram(const char*               vertexSource,
                                const char*               fragmentSource,
                                const GlAttributeBinding* attributes,
                                std::size_t               attributeCount,
                                const char*               debugName);
}

// Same reason as the Foundation headers: the directive sits on
// SasamiEngine so that anything nested under it finds these names by enclosing-namespace lookup.
namespace SasamiEngine { using namespace Render; }
