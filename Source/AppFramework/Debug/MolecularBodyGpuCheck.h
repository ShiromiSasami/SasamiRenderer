#pragma once

#include <cstdint>
#include <string>

namespace SasamiEngine::App
{
    class ApplicationCore;

    namespace Debug
    {
        // `pchar.gpucheck`: runs the pchar.sim hold / release / re-form schedule on MolecularBody (CPU) and on
        // GpuMolecularBodyStepper (GPU, every step in one submit) over the same per-step targets, then compares
        // the final state. mode: scatter|swirl|melt. DX12 only. Returns "OK ..." or "ERR ...".
        std::string RunMolecularBodyGpuCheck(ApplicationCore& app, const std::string& model, const std::string& mode,
                                             uint32_t count, uint32_t steps);
    }
}
