#pragma once
#include <cstdint>

namespace SasamiRenderer
{
    struct RayTracingStats
    {
        bool     hardwareSupported           = false;
        bool     usingHardwarePath           = false;
        bool     usedFallback                = false;
        bool     shadowUpdatedThisFrame      = false;
        bool     reflectionUpdatedThisFrame  = false;
        bool     shadowReusedThisFrame       = false;
        bool     reflectionReusedThisFrame   = false;
        uint32_t instanceCount               = 0;
        uint32_t triangleCount               = 0;
        uint32_t bvhNodeCount                = 0;
        uint32_t renderWidth                 = 0;
        uint32_t renderHeight                = 0;
        uint32_t qualityTier                 = 0;
        uint32_t shadowMapSize               = 0;
        uint32_t reflectionWidth             = 0;
        uint32_t reflectionHeight            = 0;
        uint32_t shadowUpdateInterval        = 1;
        uint32_t reflectionUpdateInterval    = 1;
        uint32_t reflectionPhaseCount        = 1;
        uint32_t reflectionPhaseIndex        = 0;
        float    dynamicResolutionScale      = 1.0f;
        float    reflectionResolutionScale   = 1.0f;
        float    reflectionMaxRoughness      = 0.0f;
        float    reflectionMinEnergy         = 0.0f;
        float    reflectionMaxDistance       = 0.0f;
        float    sceneBuildMs                = 0.0f;
        float    primaryTraceMs             = 0.0f;
        float    shadowTraceMs               = 0.0f;
        float    shadeMs                     = 0.0f;
        float    resolveMs                   = 0.0f;
        float    traceMs                     = 0.0f;
        float    copyMs                      = 0.0f;
        float    lastFrameMs                 = 0.0f;
    };
}
