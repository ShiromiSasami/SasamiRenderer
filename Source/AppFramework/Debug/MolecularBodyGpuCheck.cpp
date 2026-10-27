#include "AppFramework/Debug/MolecularBodyGpuCheck.h"

#include "AppFramework/ApplicationCore.h"
#include "AppFramework/Debug/ParticleCharacterModel.h"
#include "Renderer/ParticleCharacter/GpuMolecularBodyStepper.h"
#include "Renderer/ParticleCharacter/MolecularBody.h"
#include "Renderer/RHI/GraphicsDevice.h"
#include "Renderer/Runtime/Renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace SasamiEngine::App::Debug
{
    namespace
    {
        // Rebuilt when the device changes.
        std::unique_ptr<Render::ParticleCharacter::GpuMolecularBodyStepper> g_stepper;
        Render::IRHIDevice* g_stepperDevice = nullptr;
    }

    std::string RunMolecularBodyGpuCheck(ApplicationCore& app, const std::string& model, const std::string& mode,
                                         uint32_t count, uint32_t steps)
    {
        using namespace Render::ParticleCharacter;

        if (count == 0 || steps < 3 || static_cast<uint64_t>(count) * steps > 4000000ull) {
            return "ERR count >= 1, steps >= 3 and count*steps <= 4000000";
        }
        Render::IRHIDevice* device = app.GetRenderer().GetRhiDevice();
        if (!device) {
            return "ERR no RHI device";
        }

        ParticleCharacterModel baked;
        std::string error;
        if (!LoadParticleCharacterModel(model, count, 1, 0, baked, error)) {
            return "ERR " + error;
        }

        // The targets of every step up front, laid out as the GPU reads them (step s at s * n * 3).
        constexpr float DT = 1.0f / 60.0f;
        const size_t n = baked.set.samples.size();
        const size_t stride = n * 3;
        std::vector<float> targets(stride * steps), normals(stride * steps);
        std::vector<float> poseP, poseN;
        for (uint32_t s = 0; s < steps; ++s) {
            if (!EvaluateParticleCharacterPose(baked, static_cast<float>(s) * DT, poseP, poseN)) {
                return "ERR pose evaluation failed";
            }
            InterpolateSamples(baked.set, poseP.data(), poseN.data(), 3, targets.data() + s * stride,
                               normals.data() + s * stride);
        }

        float bmin[3], bmax[3];
        for (int k = 0; k < 3; ++k) {
            bmin[k] = (std::numeric_limits<float>::max)();
            bmax[k] = (std::numeric_limits<float>::lowest)();
        }
        for (size_t i = 0; i < n; ++i) {
            for (int k = 0; k < 3; ++k) {
                bmin[k] = (std::min)(bmin[k], targets[i * 3 + k]);
                bmax[k] = (std::max)(bmax[k], targets[i * 3 + k]);
            }
        }
        const float height = bmax[1] - bmin[1];
        if (!(height > 0.0f)) {
            return "ERR model has no height";
        }

        // Same settings and schedule as pchar.sim.
        MolecularBodySettings settings;
        settings.force = mode == "swirl" ? MolecularForce::Swirl
                       : mode == "melt" ? MolecularForce::Melt
                                        : MolecularForce::Scatter;
        settings.scatterSpeed = 1.5f * height;
        settings.gravity = 9.8f * height / 1.6f;
        settings.swirlRise = 0.5f * height;
        settings.floorY = bmin[1];
        settings.centre[0] = 0.5f * (bmin[0] + bmax[0]);
        settings.centre[1] = bmin[1];
        settings.centre[2] = 0.5f * (bmin[2] + bmax[2]);

        MolecularBody body;
        body.Reset(targets.data(), static_cast<uint32_t>(n), 1);

        std::vector<MolecularParticleGpu> particles(n);
        for (size_t i = 0; i < n; ++i) {
            MolecularParticleGpu& p = particles[i];
            std::memcpy(p.position, &body.Positions()[i * 3], sizeof(p.position));
            std::memcpy(p.velocity, &body.Velocities()[i * 3], sizeof(p.velocity));
            p.bond = body.Bonds()[i];
            p.delayHash = body.DelayHashes()[i];
            p.released = body.Released()[i];
            p.floorHits = 0;
            p.pad[0] = 0;
            p.pad[1] = 0;
        }

        // CPU run; each step's params capture the goal / timer exactly as that Step sees them.
        const uint32_t third = steps / 3;
        std::vector<MolecularStepParamsGpu> params(steps);
        double cpuMs = 0.0;
        for (uint32_t s = 0; s < steps; ++s) {
            bool goalChanged = false;
            if (s == third) {
                body.SetBondGoal(0.0f);
                goalChanged = true;
            } else if (s == 2 * third) {
                body.SetBondGoal(1.0f);
                goalChanged = true;
            }
            params[s] = MakeMolecularStepParamsGpu(settings, DT, body.BondGoal(), body.SinceGoalChange(),
                                                   static_cast<uint32_t>(n), goalChanged);
            const auto start = std::chrono::steady_clock::now();
            body.Step(targets.data() + s * stride, normals.data() + s * stride, DT, settings, true);
            cpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        }

        if (!g_stepper || g_stepperDevice != device) {
            g_stepper = std::make_unique<GpuMolecularBodyStepper>();
            g_stepperDevice = device;
        }
        if (!g_stepper->Initialize(*device, error)) {
            return "ERR " + error;
        }
        double gpuMs = 0.0;
        if (!g_stepper->Run(particles, targets.data(), normals.data(), params.data(), steps, gpuMs, error)) {
            return "ERR " + error;
        }

        const std::vector<float>& cpuPos = body.Positions();
        const std::vector<float>& cpuVel = body.Velocities();
        uint32_t posMismatch = 0;
        uint32_t bondMismatch = 0;
        uint32_t releasedMismatch = 0;
        float maxPosErr = 0.0f;
        float maxVelErr = 0.0f;
        float maxBondErr = 0.0f;
        uint64_t gpuFloorHits = 0;
        for (size_t i = 0; i < n; ++i) {
            const MolecularParticleGpu& p = particles[i];
            if (std::memcmp(p.position, &cpuPos[i * 3], sizeof(p.position)) != 0) {
                ++posMismatch;
            }
            for (int k = 0; k < 3; ++k) {
                maxPosErr = (std::max)(maxPosErr, std::fabs(p.position[k] - cpuPos[i * 3 + k]));
                maxVelErr = (std::max)(maxVelErr, std::fabs(p.velocity[k] - cpuVel[i * 3 + k]));
            }
            if (std::memcmp(&p.bond, &body.Bonds()[i], sizeof(float)) != 0) {
                ++bondMismatch;
            }
            maxBondErr = (std::max)(maxBondErr, std::fabs(p.bond - body.Bonds()[i]));
            if ((p.released != 0) != (body.Released()[i] != 0)) {
                ++releasedMismatch;
            }
            gpuFloorHits += p.floorHits;
        }

        std::ostringstream out;
        out << "OK model=" << model << " mode=" << mode << " count=" << n << " steps=" << steps
            << " posMismatch=" << posMismatch << " bondMismatch=" << bondMismatch
            << " releasedMismatch=" << releasedMismatch
            << " floorHits cpu=" << body.FloorHits() << " gpu=" << gpuFloorHits
            << std::scientific << std::setprecision(2)
            << " maxPosErr=" << maxPosErr << " maxVelErr=" << maxVelErr << " maxBondErr=" << maxBondErr
            << std::fixed << std::setprecision(3)
            << " height=" << height << " cpuMs=" << cpuMs << " gpuMs=" << gpuMs;
        return out.str();
    }
}
