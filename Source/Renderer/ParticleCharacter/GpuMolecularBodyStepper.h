#pragma once

#include "Renderer/ParticleCharacter/MolecularBody.h"
#include "Renderer/RHI/RhiDevice.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Render::ParticleCharacter
{
    // The HLSL StepParams of MolecularBodyStep_CS.hlsl (112 bytes). One entry per step: the CPU fills
    // bondGoal / sinceGoalChange exactly as MolecularBody would see them in that Step.
    struct MolecularStepParamsGpu
    {
        float halflife;
        float bondRate;
        float bondDelaySpread;
        float bondDelayPerHeight;
        uint32_t force;              // MolecularForce as uint
        float scatterSpeed;
        float swirlRate;
        float swirlRise;
        float gravity;
        float drag;
        float floorY;
        float restitution;
        float centre[3];
        float dt;
        float bondGoal;
        float sinceGoalChange;
        uint32_t count;
        float fourLn2;               // 4 ln 2, the constant of Anim::HalflifeToDamping
        uint32_t resetReleased;      // 1 on the step right after a SetBondGoal: clear `released` first
        uint32_t pad[7];
    };
    static_assert(sizeof(MolecularStepParamsGpu) == 112, "MolecularStepParamsGpu must match the HLSL StepParams layout");

    // The HLSL MolecularParticle (48 bytes).
    struct MolecularParticleGpu
    {
        float position[3];
        float bond;
        float velocity[3];
        float delayHash;
        uint32_t released;
        uint32_t floorHits;          // per particle, summed on the CPU
        uint32_t pad[2];
    };
    static_assert(sizeof(MolecularParticleGpu) == 48, "MolecularParticleGpu must match the HLSL MolecularParticle layout");

    MolecularStepParamsGpu MakeMolecularStepParamsGpu(const MolecularBodySettings& settings, float dt, float bondGoal,
                                                      float sinceGoalChange, uint32_t count, bool resetReleased);

    // GPU twin of MolecularBody::Step (Shaders/Compute/ParticleCharacter/MolecularBodyStep_CS.hlsl),
    // one thread per particle. Stage 2a validation path: Run records every step into one encoder
    // (inline root descriptors, offset per step), submits once, waits and reads back. DXIL, so only
    // the DX12 backend is supported for now and Initialize fails elsewhere.
    class GpuMolecularBodyStepper
    {
    public:
        ~GpuMolecularBodyStepper();

        bool Initialize(IRhiDevice& device, std::string& outError);
        bool IsInitialized() const { return m_device != nullptr; }

        // `particles`: the initial state (in/out). targets / normals: steps * count * 3 floats, step s at
        // s * count * 3. params: `steps` entries.
        bool Run(std::vector<MolecularParticleGpu>& particles, const float* targets, const float* normals,
                 const MolecularStepParamsGpu* params, uint32_t steps, double& outGpuMs, std::string& outError);

    private:
        IRhiDevice* m_device = nullptr;
        RhiPipelineLayoutHandle m_layout{};   // inline roots: 0 targets t0, 1 normals t1, 2 params t2, 3 particles u0
        RhiPipelineHandle m_pipeline{};
    };
}
