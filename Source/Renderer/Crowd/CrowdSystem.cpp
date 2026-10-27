#include "Renderer/Crowd/CrowdSystem.h"

#include "Foundation/Tools/DebugOutput.h"

#include <algorithm>

namespace SasamiEngine::Render::Crowd
{
    namespace
    {
        constexpr float kMinHeight = 0.01f;
        constexpr float kMinBoundsHeight = 1e-6f;
        constexpr float kMinHalfExtent = 0.5f;
        constexpr float kMaxDeltaTime = 0.1f;
        constexpr float kMinSpeed = 0.6f;
        constexpr float kMaxSpeed = 1.6f;
    }

    bool CrowdSystem::Initialize(IRhiDevice& device, std::string& outError)
    {
        if (m_device != nullptr)
        {
            return true;
        }
        if (!m_updater.Initialize(device, outError))
        {
            return false;
        }
        std::string cullerError;
        if (!m_culler.Initialize(device, cullerError))
        {
            // Not fatal: CrowdRenderPass falls back to one DrawIndexed of every agent.
            DebugLog((cullerError + "\n").c_str());
        }
        m_device = &device;
        return true;
    }

    void CrowdSystem::Shutdown()
    {
        if (m_device != nullptr)
        {
            m_device->WaitForRhiIdle();
        }
        m_asset.Release();
        m_culler.Shutdown();
        m_updater.Shutdown();
        m_pendingDeltaTime = 0.0f;
        m_store.Resize(0);
        m_device = nullptr;
    }

    bool CrowdSystem::SetAsset(const std::vector<SkinnedMesh>& meshes, const VatBakedAnimation& baked,
                               std::string& outError)
    {
        if (m_device == nullptr)
        {
            outError = "CrowdSystem: not initialized";
            return false;
        }
        m_device->WaitForRhiIdle();
        if (!m_asset.Upload(*m_device, meshes, baked, outError))
        {
            return false;
        }

        const uint32_t clipCount = (std::min)(m_asset.ClipCount(), CROWD_MAX_CLIPS);
        m_params.clipCount = clipCount;
        for (uint32_t i = 0; i < CROWD_MAX_CLIPS; ++i)
        {
            m_params.clipDurations[i] = (i < clipCount) ? m_asset.ClipDurations()[i] : 1.0f;
        }

        if (m_store.Count() > 0 && clipCount > 0)
        {
            for (uint32_t& clip : m_store.clip)
            {
                clip = (std::min)(clip, clipCount - 1);
            }
            return m_updater.Upload(m_store, outError);
        }
        return true;
    }

    bool CrowdSystem::Spawn(uint32_t count, float halfExtent, uint32_t seed, std::string& outError)
    {
        if (m_device == nullptr)
        {
            outError = "CrowdSystem: not initialized";
            return false;
        }
        if (!m_asset.IsValid())
        {
            outError = "CrowdSystem: no asset";
            return false;
        }

        halfExtent = (std::max)(halfExtent, kMinHalfExtent);
        m_params.boundsMinX = -halfExtent;
        m_params.boundsMaxX = halfExtent;
        m_params.boundsMinZ = -halfExtent;
        m_params.boundsMaxZ = halfExtent;
        m_device->WaitForRhiIdle();
        m_pendingDeltaTime = 0.0f;

        if (count == 0)
        {
            m_store.Resize(0);
            return true;
        }
        SpawnCrowd(m_store, count, m_params, kMinSpeed, kMaxSpeed, seed);
        if (!m_updater.Upload(m_store, outError))
        {
            return false;
        }
        if (m_culler.IsInitialized())
        {
            return m_culler.EnsureCapacity(count, outError);
        }
        return true;
    }

    bool CrowdSystem::Update(float deltaTime, std::string& outError)
    {
        if (m_device == nullptr || m_store.Count() == 0 || m_mode == CrowdUpdateMode::Off || deltaTime <= 0.0f)
        {
            return true;
        }

        if (m_mode == CrowdUpdateMode::Gpu && m_gpuStepMode == CrowdGpuStepMode::Recorded)
        {
            m_pendingDeltaTime = (std::min)(m_pendingDeltaTime + deltaTime, kMaxDeltaTime);
            return true;
        }

        m_params.deltaTime = (std::min)(deltaTime, kMaxDeltaTime);
        if (m_mode == CrowdUpdateMode::Gpu)
        {
            return m_updater.Step(m_params, 1, outError);
        }

        StepCrowdCpu(m_store, m_params, CrowdUpdateBackend::CpuParallel);
        m_device->WaitForRhiIdle();
        return m_updater.Upload(m_store, outError);
    }

    bool CrowdSystem::RecordPendingStep(IRhiCommandEncoder& encoder, std::string& outError)
    {
        if (m_device == nullptr || m_store.Count() == 0)
        {
            return true;
        }
        if (m_mode == CrowdUpdateMode::Gpu && m_gpuStepMode == CrowdGpuStepMode::Recorded
            && m_pendingDeltaTime > 0.0f)
        {
            m_params.deltaTime = m_pendingDeltaTime;
            m_pendingDeltaTime = 0.0f;
            return m_updater.RecordStep(encoder, m_params, outError);
        }
        return true;
    }

    bool CrowdSystem::RecordFrameWork(IRhiCommandEncoder& encoder, const float* viewProj, const float* cameraPos,
                                      bool& outCulled, std::string& outError)
    {
        outCulled = false;
        if (m_device == nullptr || m_store.Count() == 0)
        {
            return true;
        }

        if (!RecordPendingStep(encoder, outError))
        {
            return false;
        }

        const uint32_t agentCount = m_updater.AgentCount();
        if (m_cullSettings.enabled && m_culler.IsInitialized() && m_asset.IsValid() && m_asset.LodCount() > 0
            && viewProj != nullptr && cameraPos != nullptr && agentCount > 0
            && m_culler.Capacity() >= agentCount)
        {
            CrowdCullInputs inputs{};
            inputs.viewProj = viewProj;
            inputs.cameraPos = cameraPos;
            inputs.modelScale = ModelScale();
            inputs.modelYOffset = ModelYOffset();
            inputs.agentCount = agentCount;
            inputs.agentBuffer = m_updater.GetAgentBuffer();
            inputs.asset = &m_asset;
            if (!m_culler.Record(encoder, inputs, m_cullSettings, outError))
            {
                return false;
            }
            outCulled = true;
        }
        return true;
    }

    void CrowdSystem::EndFrameWork(IRhiCommandEncoder& encoder)
    {
        m_culler.RestoreAfterDraw(encoder);
    }

    bool CrowdSystem::SetUpdateMode(CrowdUpdateMode mode, std::string& outError)
    {
        if (m_mode == CrowdUpdateMode::Gpu && mode != CrowdUpdateMode::Gpu)
        {
            m_pendingDeltaTime = 0.0f;
        }
        if (m_mode == CrowdUpdateMode::Gpu && mode != CrowdUpdateMode::Gpu && m_store.Count() > 0
            && m_device != nullptr)
        {
            if (!m_updater.Readback(m_store, outError))
            {
                return false;
            }
        }
        m_mode = mode;
        return true;
    }

    void CrowdSystem::SetTargetHeight(float height)
    {
        m_targetHeight = (std::max)(height, kMinHeight);
    }

    float CrowdSystem::ModelScale() const
    {
        if (!m_asset.IsValid())
        {
            return 1.0f;
        }
        const float height = m_asset.BoundsMax()[1] - m_asset.BoundsMin()[1];
        if (height <= kMinBoundsHeight)
        {
            return 1.0f;
        }
        return m_targetHeight / height;
    }

    float CrowdSystem::ModelYOffset() const
    {
        if (!m_asset.IsValid())
        {
            return 0.0f;
        }
        return -m_asset.BoundsMin()[1] * ModelScale();
    }
}
