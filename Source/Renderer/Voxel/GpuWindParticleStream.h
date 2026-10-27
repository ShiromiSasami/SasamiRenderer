#pragma once

#include "Renderer/RHI/RhiDevice.h"
#include "Renderer/Voxel/GpuWindParticleStepper.h"
#include "Renderer/Voxel/WindDrivenParticles.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SasamiEngine::Render::Voxel
{
    // 6c-2: a persistent GPU particle buffer stepped inside the frame's own command list (no submit,
    // no wait). Each Record copies the queued spawns into the buffer, records one
    // GpuWindParticleStepper::RecordStep and copies the buffer into a readback ring slot. ReadLatest
    // returns the slot written READ_LAG records ago.
    //
    // Assumptions (not verified): the GPU has finished a record READ_LAG records after it was made
    // (the renderer keeps 2 back buffers, so at most 2 frames should be in flight), and Record and
    // ReadLatest run on the same thread. Spawns overwrite slots round-robin, so when the buffer is
    // full the oldest slots are recycled even if their particles are still alive.
    // DX12 only, like GpuWindParticleStepper.
    class GpuWindParticleStream
    {
    public:
        static constexpr uint32_t RING_SLOTS = 4;
        static constexpr uint32_t READ_LAG = 3;

        ~GpuWindParticleStream();

        // `capacity` particles in the GPU buffer; at most `maxSpawnPerRecord` spawns per Record.
        bool Initialize(IRhiDevice& device, uint32_t capacity, uint32_t maxSpawnPerRecord, std::string& outError);
        bool IsInitialized() const { return m_device != nullptr; }
        uint32_t Capacity() const { return m_capacity; }
        void Release();

        // Queues spawns for the next Record. Spawns beyond maxSpawnPerRecord are dropped and counted.
        void QueueSpawn(const WindParticleState* states, uint32_t count);
        // Params of the next Record; particleCount is overwritten with Capacity().
        void SetParams(const WindParticleStepParamsGpu& params);
        bool HasParams() const { return m_hasParams; }

        // Records spawn copy + step + readback copy into `encoder`. Returns false (and records nothing)
        // when not initialized, without params or when the stepper has no field.
        bool Record(IRhiCommandEncoder& encoder, const GpuWindParticleStepper& stepper, std::string& outError);

        // Copies the readback slot of record (RecordCount() - READ_LAG) into `outStates` (all
        // Capacity() entries, dead ones included) once per record. False when there is none new yet.
        bool ReadLatest(std::vector<WindParticleState>& outStates);

        uint64_t RecordCount() const { return m_recordCount; }
        uint64_t DroppedSpawns() const { return m_droppedSpawns; }
        // Diagnostics: spawns accepted so far, successful ReadLatest calls, alive count of the last read.
        uint64_t QueuedSpawns() const { return m_queuedSpawns; }
        uint64_t ReadCount() const { return m_readCount; }
        uint32_t LastReadAlive() const { return m_lastReadAlive; }
        const WindParticleStepParamsGpu& Params() const { return m_params; }

    private:
        IRhiDevice* m_device = nullptr;
        uint32_t m_capacity = 0;
        uint32_t m_maxSpawnPerRecord = 0;

        RhiBufferHandle m_particles{};                       // GpuOnly, UnorderedAccess between records
        RhiBufferHandle m_spawnRing{};                       // CpuToGpu, RING_SLOTS * maxSpawnPerRecord states
        RhiBufferHandle m_paramsRing{};                      // CpuToGpu, RING_SLOTS * 256-byte slots
        RhiBufferHandle m_readbackRing[RING_SLOTS]{};        // GpuToCpu, Capacity() states each

        std::vector<WindParticleState> m_pendingSpawns;
        WindParticleStepParamsGpu m_params{};
        bool m_hasParams = false;
        uint32_t m_spawnCursor = 0;                          // next slot in m_particles
        uint64_t m_recordCount = 0;
        uint64_t m_lastReadRecord = UINT64_MAX;
        uint64_t m_droppedSpawns = 0;
        uint64_t m_queuedSpawns = 0;
        uint64_t m_readCount = 0;
        uint32_t m_lastReadAlive = 0;
    };
}
