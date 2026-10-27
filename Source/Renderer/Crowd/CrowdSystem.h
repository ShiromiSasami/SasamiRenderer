#pragma once

#include "Renderer/Crowd/CrowdGpuCuller.h"
#include "Renderer/Crowd/CrowdSimulation.h"
#include "Renderer/Crowd/CrowdStore.h"
#include "Renderer/Crowd/CrowdVatGpuAsset.h"
#include "Renderer/Crowd/GpuCrowdUpdater.h"
#include "Renderer/RHI/RhiDevice.h"

#include <cstdint>
#include <string>

namespace SasamiEngine::Render::Crowd
{
    // Who advances the agents each frame. Gpu keeps the state resident (GpuCrowdUpdater);
    // Cpu steps the CrowdStore with StepCrowdCpu(CpuParallel) and re-uploads it (reference path);
    // Off freezes the crowd in place.
    enum class CrowdUpdateMode
    {
        Off,
        Cpu,
        Gpu,
    };

    // How the Gpu mode's step reaches the GPU (TODO 8d). Recorded: Update only accumulates the
    // frame's delta time and CrowdRenderPass records the step into the frame encoder (no stall).
    // Immediate: the 8c path, Update submits its own encoder and waits for idle (kept for A/B).
    enum class CrowdGpuStepMode
    {
        Recorded,
        Immediate,
    };

    // Owns the crowd's state between frames: the VAT asset on the GPU, the agents (CPU mirror +
    // GPU copy), the update mode and the GPU culler. CrowdRenderPass calls RecordFrameWork.
    // Known debt: the Cpu mode still waits for idle before re-uploading (Upload recreates the
    // buffer), and CrowdGpuStepMode::Immediate stalls on purpose.
    class CrowdSystem
    {
    public:
        bool Initialize(IRhiDevice& device, std::string& outError);
        bool IsInitialized() const { return m_device != nullptr; }
        void Shutdown();

        // Waits for the GPU, then replaces the asset. Agents are kept but their clip indices are
        // wrapped into the new asset's clip count on the next Spawn only.
        bool SetAsset(const std::vector<SkinnedMesh>& meshes, const VatBakedAnimation& baked, std::string& outError);
        bool HasAsset() const { return m_asset.IsValid(); }
        const CrowdVatGpuAsset& Asset() const { return m_asset; }

        // Waits for the GPU, then spawns `count` agents (SpawnCrowd) on a square of half size
        // `halfExtent` around the origin, using the asset's clip durations, and uploads them.
        // count 0 clears the crowd. Requires an asset.
        bool Spawn(uint32_t count, float halfExtent, uint32_t seed, std::string& outError);
        uint32_t AgentCount() const { return m_store.Count(); }

        // Advances the crowd by deltaTime with the current mode (no-op without agents).
        // Called once per frame by the renderer while the crowd is enabled. In Gpu + Recorded mode
        // it only adds deltaTime to the pending step (clamped to kMaxDeltaTime).
        bool Update(float deltaTime, std::string& outError);

        void SetGpuStepMode(CrowdGpuStepMode mode) { m_gpuStepMode = mode; }
        CrowdGpuStepMode GetGpuStepMode() const { return m_gpuStepMode; }

        // Called by CrowdRenderPass inside its Execute, before the draw. Records into `encoder`
        // (no submit): the pending GPU step (Gpu + Recorded with pending time > 0), then, when
        // culling is enabled and the asset has LODs, the culler's reset + cull. viewProj is the
        // row-major camera matrix (16 floats), cameraPos 3 floats.
        // On return, outCulled tells the pass whether to draw indirectly from Culler() (true) or
        // with one DrawIndexed of every agent (false). The pass must call EndFrameWork after the
        // draw, which restores the culler's buffer states.
        bool RecordFrameWork(IRhiCommandEncoder& encoder, const float* viewProj, const float* cameraPos,
                             bool& outCulled, std::string& outError);
        // Records the pending GPU step (Gpu + Recorded with pending time > 0) and consumes it, so a
        // later RecordFrameWork in the same frame does not step twice. The shadow pass calls this
        // first so the shadow and the drawn crowd use the same agent state. No-op otherwise.
        bool RecordPendingStep(IRhiCommandEncoder& encoder, std::string& outError);
        void EndFrameWork(IRhiCommandEncoder& encoder);

        CrowdCullSettings& CullSettings() { return m_cullSettings; }
        const CrowdCullSettings& CullSettings() const { return m_cullSettings; }
        CrowdGpuCuller& Culler() { return m_culler; }
        const CrowdGpuCuller& Culler() const { return m_culler; }

        // Switching Gpu -> Cpu reads the GPU state back first so the CPU path continues from it.
        bool SetUpdateMode(CrowdUpdateMode mode, std::string& outError);
        CrowdUpdateMode GetUpdateMode() const { return m_mode; }

        // Uniform scale applied to the model so its baked bounds are `targetHeight` tall, and the
        // vertical offset that puts the bounds' bottom on the agent's posY.
        void  SetTargetHeight(float height);
        float GetTargetHeight() const { return m_targetHeight; }
        float ModelScale() const;
        float ModelYOffset() const;

        // Resident agent buffer for the pass (UnorderedAccess at rest); invalid when empty.
        RhiBufferHandle AgentBuffer() const { return m_updater.GetAgentBuffer(); }
        const CrowdSimParams& SimParams() const { return m_params; }

    private:
        IRhiDevice* m_device = nullptr;
        CrowdVatGpuAsset m_asset;
        GpuCrowdUpdater m_updater;
        CrowdGpuCuller m_culler;
        CrowdCullSettings m_cullSettings{};
        CrowdGpuStepMode m_gpuStepMode = CrowdGpuStepMode::Recorded;
        float m_pendingDeltaTime = 0.0f;
        CrowdStore m_store;
        CrowdSimParams m_params{};
        CrowdUpdateMode m_mode = CrowdUpdateMode::Gpu;
        float m_targetHeight = 1.0f;
    };
}

namespace SasamiEngine { using namespace Render; }
