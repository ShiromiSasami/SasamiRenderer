#pragma once

#include <cstdint>
#include <string>

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        // `voxel.wind.fx gpucheck`: steps the same particles with StepWindParticle on the CPU and with
        // GpuWindParticleStepper on the GPU over the wind overlay's projected field (no gusts), then
        // compares them. Needs `voxel.wind.overlay on mode=project`. DX12 only. Returns "OK ..." or "ERR ...".
        std::string RunWindParticleGpuCheck(ApplicationCore& app, bool petals, uint32_t count, uint32_t steps);
    }
}
