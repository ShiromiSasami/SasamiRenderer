#ifndef SWRT_COMMON_HLSLI
#define SWRT_COMMON_HLSLI

// ---------------------------------------------------------------------------
// GPU Software Ray Tracer shared interface.
// Keep this file as the public include point. Responsibility-specific code is
// split into the files below so entry shaders do not depend on one monolithic
// common header.
// ---------------------------------------------------------------------------

#include "SWRT_Types.hlsli"
#include "SWRT_Material.hlsli"
#include "SWRT_Bindings.hlsli"
#include "SWRT_Intersection.hlsli"
#include "SWRT_Traversal.hlsli"

#endif // SWRT_COMMON_HLSLI
