#include "Renderer/Voxel/GpuWindParticleStream.h"

#include "Renderer/Voxel/GpuVoxelBuffers.h"

#include <algorithm>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    namespace
    {
        using namespace GpuVoxelBuffers;

        constexpr uint64_t kParamsSlotBytes = 256;

        std::string MakeError(const char* message)
        {
            return std::string("GpuWindParticleStream: ") + message;
        }

        void Transition(IRhiCommandEncoder& encoder, RhiBufferHandle buffer, RhiResourceState before,
                        RhiResourceState after)
        {
            RhiResourceTransitionDesc transition{};
            transition.resource = buffer;
            transition.before = before;
            transition.after = after;
            encoder.TransitionResources(&transition, 1);
        }
    }

    GpuWindParticleStream::~GpuWindParticleStream()
    {
        Release();
    }

    bool GpuWindParticleStream::Initialize(IRhiDevice& device, uint32_t capacity, uint32_t maxSpawnPerRecord,
                                           std::string& outError)
    {
        if (m_device == &device && m_capacity == capacity && m_maxSpawnPerRecord == maxSpawnPerRecord) {
            return true;
        }
        Release();

        if (capacity == 0 || maxSpawnPerRecord == 0 || maxSpawnPerRecord > capacity) {
            outError = MakeError("invalid capacity or spawn limit");
            return false;
        }

        const uint64_t particleBytes = static_cast<uint64_t>(capacity) * sizeof(WindParticleState);
        const std::vector<uint8_t> zeros(static_cast<size_t>(particleBytes), 0);

        m_device = &device;
        m_particles = device.CreateRhiBuffer(
            MakeBufferDesc(particleBytes, sizeof(WindParticleState),
                           RhiBufferUsageFlags::UnorderedAccess | RhiBufferUsageFlags::CopySource |
                               RhiBufferUsageFlags::CopyDest | RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::GpuOnly, RhiResourceState::UnorderedAccess),
            zeros.data());
        m_spawnRing = device.CreateRhiBuffer(
            MakeBufferDesc(static_cast<uint64_t>(RING_SLOTS) * maxSpawnPerRecord * sizeof(WindParticleState),
                           sizeof(WindParticleState), RhiBufferUsageFlags::CopySource, RhiMemoryUsage::CpuToGpu,
                           RhiResourceState::CopySource),
            nullptr);
        m_paramsRing = device.CreateRhiBuffer(
            MakeBufferDesc(static_cast<uint64_t>(RING_SLOTS) * kParamsSlotBytes, sizeof(WindParticleStepParamsGpu),
                           RhiBufferUsageFlags::ShaderResource | RhiBufferUsageFlags::Structured,
                           RhiMemoryUsage::CpuToGpu, RhiResourceState::ShaderResource),
            nullptr);
        bool ok = m_particles.IsValid() && m_spawnRing.IsValid() && m_paramsRing.IsValid();
        for (uint32_t i = 0; i < RING_SLOTS; ++i) {
            m_readbackRing[i] = device.CreateRhiBuffer(
                MakeBufferDesc(particleBytes, 0, RhiBufferUsageFlags::CopyDest, RhiMemoryUsage::GpuToCpu,
                               RhiResourceState::CopyDest),
                nullptr);
            ok = ok && m_readbackRing[i].IsValid();
        }
        if (!ok) {
            Release();
            outError = MakeError("failed to create the buffers");
            return false;
        }

        m_capacity = capacity;
        m_maxSpawnPerRecord = maxSpawnPerRecord;
        m_spawnCursor = 0;
        m_recordCount = 0;
        m_lastReadRecord = UINT64_MAX;
        m_droppedSpawns = 0;
        m_queuedSpawns = 0;
        m_readCount = 0;
        m_lastReadAlive = 0;
        m_hasParams = false;
        m_pendingSpawns.clear();
        m_pendingSpawns.reserve(maxSpawnPerRecord);
        return true;
    }

    // The caller must only Release when the GPU is idle or the stream is no longer recorded: the
    // buffers are destroyed immediately, not through the renderer's deferred destruction.
    void GpuWindParticleStream::Release()
    {
        if (m_device != nullptr) {
            RhiBufferHandle* buffers[] = { &m_particles, &m_spawnRing, &m_paramsRing };
            for (RhiBufferHandle* buffer : buffers) {
                if (buffer->IsValid()) {
                    m_device->DestroyRhiResource(*buffer);
                }
            }
            for (RhiBufferHandle& buffer : m_readbackRing) {
                if (buffer.IsValid()) {
                    m_device->DestroyRhiResource(buffer);
                }
            }
        }
        m_particles = {};
        m_spawnRing = {};
        m_paramsRing = {};
        for (RhiBufferHandle& buffer : m_readbackRing) {
            buffer = {};
        }
        m_device = nullptr;
        m_capacity = 0;
        m_maxSpawnPerRecord = 0;
        m_pendingSpawns.clear();
        m_params = {};
        m_hasParams = false;
        m_spawnCursor = 0;
        m_recordCount = 0;
        m_lastReadRecord = UINT64_MAX;
        m_droppedSpawns = 0;
        m_queuedSpawns = 0;
        m_readCount = 0;
        m_lastReadAlive = 0;
    }

    void GpuWindParticleStream::QueueSpawn(const WindParticleState* states, uint32_t count)
    {
        if (m_device == nullptr || states == nullptr || count == 0) {
            return;
        }
        const uint32_t pending = static_cast<uint32_t>(m_pendingSpawns.size());
        const uint32_t room = m_maxSpawnPerRecord - (std::min)(pending, m_maxSpawnPerRecord);
        const uint32_t accepted = (std::min)(count, room);
        m_pendingSpawns.insert(m_pendingSpawns.end(), states, states + accepted);
        m_droppedSpawns += count - accepted;
        m_queuedSpawns += accepted;
    }

    void GpuWindParticleStream::SetParams(const WindParticleStepParamsGpu& params)
    {
        m_params = params;
        m_params.particleCount = m_capacity;
        m_hasParams = true;
    }

    bool GpuWindParticleStream::Record(IRhiCommandEncoder& encoder, const GpuWindParticleStepper& stepper,
                                       std::string& outError)
    {
        if (m_device == nullptr) {
            outError = MakeError("not initialized");
            return false;
        }
        if (!m_hasParams) {
            outError = MakeError("no params set");
            return false;
        }
        if (!stepper.HasField()) {
            outError = MakeError("the stepper has no field");
            return false;
        }

        const uint32_t slot = static_cast<uint32_t>(m_recordCount % RING_SLOTS);
        const uint64_t paramsOffset = static_cast<uint64_t>(slot) * kParamsSlotBytes;
        if (!m_device->UpdateRhiBuffer(m_paramsRing, paramsOffset, &m_params, sizeof(m_params))) {
            outError = MakeError("failed to update the params ring");
            return false;
        }
        const RhiGpuAddress paramsBase = m_device->GetRhiBufferGpuAddress(m_paramsRing);
        if (paramsBase == 0) {
            outError = MakeError("the params ring has no GPU address");
            return false;
        }
        const RhiGpuAddress paramsAddress = paramsBase + paramsOffset;

        const uint32_t spawnCount = static_cast<uint32_t>(m_pendingSpawns.size());
        const uint64_t spawnSlotOffset =
            static_cast<uint64_t>(slot) * m_maxSpawnPerRecord * sizeof(WindParticleState);
        if (spawnCount > 0
            && !m_device->UpdateRhiBuffer(m_spawnRing, spawnSlotOffset, m_pendingSpawns.data(),
                                          static_cast<uint64_t>(spawnCount) * sizeof(WindParticleState))) {
            outError = MakeError("failed to update the spawn ring");
            return false;
        }

        encoder.BeginDebugLabel("Voxel.WindParticleStream");
        if (spawnCount > 0) {
            Transition(encoder, m_particles, RhiResourceState::UnorderedAccess, RhiResourceState::CopyDest);

            const uint32_t firstCount = (std::min)(spawnCount, m_capacity - m_spawnCursor);
            RhiBufferCopyDesc copy{};
            copy.source = m_spawnRing;
            copy.destination = m_particles;
            copy.sourceOffsetInBytes = spawnSlotOffset;
            copy.destinationOffsetInBytes = static_cast<uint64_t>(m_spawnCursor) * sizeof(WindParticleState);
            copy.sizeInBytes = static_cast<uint64_t>(firstCount) * sizeof(WindParticleState);
            encoder.CopyBuffer(copy);
            if (firstCount < spawnCount) {
                copy.sourceOffsetInBytes = spawnSlotOffset + copy.sizeInBytes;
                copy.destinationOffsetInBytes = 0;
                copy.sizeInBytes = static_cast<uint64_t>(spawnCount - firstCount) * sizeof(WindParticleState);
                encoder.CopyBuffer(copy);
            }
            m_spawnCursor = static_cast<uint32_t>((static_cast<uint64_t>(m_spawnCursor) + spawnCount) % m_capacity);

            Transition(encoder, m_particles, RhiResourceState::CopyDest, RhiResourceState::UnorderedAccess);
            m_pendingSpawns.clear();
        }

        if (!stepper.RecordStep(encoder, paramsAddress, m_particles, m_capacity, outError)) {
            encoder.EndDebugLabel();
            return false;
        }

        Transition(encoder, m_particles, RhiResourceState::UnorderedAccess, RhiResourceState::CopySource);
        RhiBufferCopyDesc readback{};
        readback.source = m_particles;
        readback.destination = m_readbackRing[slot];
        readback.sizeInBytes = static_cast<uint64_t>(m_capacity) * sizeof(WindParticleState);
        encoder.CopyBuffer(readback);
        Transition(encoder, m_particles, RhiResourceState::CopySource, RhiResourceState::UnorderedAccess);
        encoder.EndDebugLabel();

        ++m_recordCount;
        return true;
    }

    bool GpuWindParticleStream::ReadLatest(std::vector<WindParticleState>& outStates)
    {
        if (m_device == nullptr || m_recordCount < READ_LAG) {
            return false;
        }
        const uint64_t target = m_recordCount - READ_LAG;
        if (target == m_lastReadRecord) {
            return false;
        }
        outStates.resize(m_capacity);
        const uint64_t bytes = static_cast<uint64_t>(m_capacity) * sizeof(WindParticleState);
        if (!m_device->ReadRhiBuffer(m_readbackRing[target % RING_SLOTS], 0, outStates.data(), bytes)) {
            return false;
        }
        m_lastReadRecord = target;
        ++m_readCount;
        m_lastReadAlive = static_cast<uint32_t>(
            std::count_if(outStates.begin(), outStates.end(), [](const WindParticleState& state) { return state.alive != 0; }));
        return true;
    }
}
