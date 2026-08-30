// MeshletConstants.hlsli
// Single source of truth for the meshlet size on the shader side.
//
// MUST stay in sync with MeshletBuffer::kMaxTrianglesPerMeshlet (MeshletBuffer.h),
// which is what the CPU-side builder uses to chunk the index buffer.  Any shader
// that needs the meshlet size includes this header instead of hardcoding a literal,
// so raising the meshlet size can never silently desync one shader from another.
#ifndef SASAMI_MESHLET_CONSTANTS_HLSLI
#define SASAMI_MESHLET_CONSTANTS_HLSLI

static const uint kMaxTrianglesPerMeshlet = 64u; // mirrors MeshletBuffer.h

#endif // SASAMI_MESHLET_CONSTANTS_HLSLI
