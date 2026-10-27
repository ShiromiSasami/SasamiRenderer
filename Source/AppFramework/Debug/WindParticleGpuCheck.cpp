#include "AppFramework/Debug/WindParticleGpuCheck.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Debug/WindFlowOverlay.h"
#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Runtime/Renderer.h"
#include "Renderer/Voxel/GpuWindParticleStepper.h"
#include "Renderer/Voxel/ProjectedWindField.h"
#include "Renderer/Voxel/SignedDistanceGrid.h"
#include "Renderer/Voxel/WindDrivenParticles.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        // Rebuilt when the device changes; the field is re-uploaded when the overlay generation changes.
        std::unique_ptr<Render::Voxel::GpuWindParticleStepper> g_stepper;
        Render::IRHIDevice* g_stepperDevice = nullptr;
        uint64_t g_uploadedGeneration = 0;
        bool g_hasUploadedGeneration = false;

        class XorShift32
        {
        public:
            float Next01()
            {
                m_state ^= m_state << 13;
                m_state ^= m_state >> 17;
                m_state ^= m_state << 5;
                return static_cast<float>(m_state >> 8) / 16777216.0f;
            }

        private:
            uint32_t m_state = 0x9E3779B9u;
        };
    }

    std::string RunWindParticleGpuCheck(ApplicationCore& app, bool petals, uint32_t count, uint32_t steps)
    {
        using namespace Render::Voxel;

        WindFlowOverlay& overlay = app.GetWindFlowOverlay();
        const ProjectedWindField* field = overlay.ProjectedField();
        const SignedDistanceGrid* sdf = overlay.Sdf();
        if (!field || !sdf) {
            return "ERR needs voxel.wind.overlay on mode=project";
        }
        if (count == 0 || count > 200000 || steps == 0 || steps > 2000) {
            return "ERR count must be 1..200000 and steps 1..2000";
        }
        Render::IRHIDevice* device = app.GetRenderer().GetRhiDevice();
        if (!device) {
            return "ERR no RHI device";
        }

        const VoxelGridDesc& desc = overlay.Desc();
        constexpr float DT = 1.0f / 30.0f;
        constexpr float TWO_PI = 6.28318530718f;

        WindDrivenParticleSettings settings;
        if (petals) {
            settings.kind = WindParticleKind::Petal;
            settings.dragTime = 0.3f;
            settings.acceleration[0] = 0.0f;
            settings.acceleration[1] = -1.5f;
            settings.acceleration[2] = 0.0f;
            settings.flutterAcceleration = 4.0f;
            settings.flutterFrequency = 1.5f;
        } else {
            settings.kind = WindParticleKind::Smoke;
            settings.dragTime = 0.5f;
            settings.buoyancy = 2.0f;
            settings.acceleration[0] = 0.0f;
            settings.acceleration[1] = 0.0f;
            settings.acceleration[2] = 0.0f;
        }
        settings.wallFriction = 0.5f;
        const WindParticleStepParams params = MakeWindParticleStepParams(settings, desc.voxelSize, DT);

        XorShift32 rng;
        const float extent[3] = { desc.dimX * desc.voxelSize, desc.dimY * desc.voxelSize, desc.dimZ * desc.voxelSize };
        std::vector<WindParticleState> initial;
        initial.reserve(count);
        const uint64_t maxTries = static_cast<uint64_t>(count) * 50;
        for (uint64_t tries = 0; tries < maxTries && initial.size() < count; ++tries) {
            float p[3];
            for (int axis = 0; axis < 3; ++axis) {
                p[axis] = desc.origin[axis] + rng.Next01() * extent[axis];
            }
            if (!(field->Distance(p) > desc.voxelSize)) {
                continue;
            }
            WindParticleState s{};
            std::memcpy(s.position, p, sizeof(p));
            field->Velocity(p, s.velocity);
            s.age = rng.Next01() * 1.0f;
            s.lifetime = 4.0f + rng.Next01() * 4.0f;
            s.angle = 0.0f;
            s.spin = (rng.Next01() - 0.5f) * 12.0f;
            s.phase = rng.Next01() * TWO_PI;
            s.alive = 1;
            const float t = rng.Next01() * TWO_PI;
            s.flutterAxisX = std::cos(t);
            s.flutterAxisZ = std::sin(t);
            s.wallHits = 0;
            s.pad = 0;
            initial.push_back(s);
        }
        if (initial.empty()) {
            return "ERR no free-space particles found";
        }

        std::vector<WindParticleState> cpu = initial;
        const auto cpuStart = std::chrono::steady_clock::now();
        for (uint32_t s = 0; s < steps; ++s) {
            for (WindParticleState& state : cpu) {
                if (state.alive != 0) {
                    StepWindParticle(state, *field, params);
                }
            }
        }
        const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpuStart).count();

        std::string error;
        if (!g_stepper || g_stepperDevice != device) {
            g_stepper = std::make_unique<GpuWindParticleStepper>();
            g_stepperDevice = device;
            g_hasUploadedGeneration = false;
        }
        if (!g_stepper->Initialize(*device, error)) {
            return "ERR " + error;
        }
        if (!g_stepper->HasField() || !g_hasUploadedGeneration || g_uploadedGeneration != overlay.Generation()) {
            if (!g_stepper->UploadField(*sdf, field->Grid(), desc, error)) {
                return "ERR " + error;
            }
            g_uploadedGeneration = overlay.Generation();
            g_hasUploadedGeneration = true;
        }

        std::vector<WindParticleState> gpu = initial;
        const auto gpuStart = std::chrono::steady_clock::now();
        if (!g_stepper->Run(gpu, params, steps, error)) {
            return "ERR " + error;
        }
        const double gpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gpuStart).count();

        const size_t n = (std::min)(cpu.size(), gpu.size());
        uint32_t cpuAlive = 0;
        uint32_t gpuAlive = 0;
        uint32_t aliveMismatch = 0;
        uint32_t exact = 0;
        uint32_t posOver = 0;
        uint64_t cpuWallHits = 0;
        uint64_t gpuWallHits = 0;
        float maxPosErr = 0.0f;
        float maxVelErr = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            const WindParticleState& c = cpu[i];
            const WindParticleState& g = gpu[i];
            const bool ca = c.alive != 0;
            const bool ga = g.alive != 0;
            cpuAlive += ca ? 1u : 0u;
            gpuAlive += ga ? 1u : 0u;
            cpuWallHits += c.wallHits;
            gpuWallHits += g.wallHits;
            if (std::memcmp(&c, &g, sizeof(WindParticleState)) == 0) {
                ++exact;
            }
            if (ca != ga) {
                ++aliveMismatch;
                continue;
            }
            if (!ca) {
                continue;
            }
            bool over = false;
            for (int axis = 0; axis < 3; ++axis) {
                const float dp = std::fabs(c.position[axis] - g.position[axis]);
                maxPosErr = (std::max)(maxPosErr, dp);
                maxVelErr = (std::max)(maxVelErr, std::fabs(c.velocity[axis] - g.velocity[axis]));
                over = over || dp > 1e-3f;
            }
            posOver += over ? 1u : 0u;
        }

        std::ostringstream out;
        out << std::setprecision(4);
        out << "OK gpucheck kind=" << (petals ? "petals" : "smoke") << " count=" << n << " steps=" << steps
            << " cpuAlive=" << cpuAlive << " gpuAlive=" << gpuAlive << " aliveMismatch=" << aliveMismatch
            << " exact=" << exact << " maxPosErr=" << maxPosErr << " maxVelErr=" << maxVelErr
            << " posOver1mm=" << posOver << " cpuWallHits=" << cpuWallHits << " gpuWallHits=" << gpuWallHits
            << " cpuMs=" << cpuMs << " gpuMs=" << gpuMs;
        return out.str();
    }
}
