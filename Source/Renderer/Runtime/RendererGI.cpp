// RendererGI.cpp
// Baking and persisting the irradiance probe grid: bake state and its log, the probe cache on
// disk, and fitting the grid to the scene.
//
// Split from Renderer.cpp because none of it runs while drawing -- it is the offline half of the
// renderer, driven by explicit requests rather than by the frame loop.
#include "Renderer/Runtime/Renderer.h"

// Renderer.h pulls in the Windows headers, which define min and max as macros and break every
// std::min / std::max below. Renderer.cpp undefines them for the same reason.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace SasamiEngine::Render
{
    namespace
    {
        bool NearlyEqual(float a, float b)
        {
            return std::fabs(a - b) <= 1.0e-4f;
        }

        constexpr uint32_t kGIProbeCacheMagic = 0x43504753u; // "SGPC", little endian
        constexpr uint32_t kGIProbeCacheVersion = 3u; // v3: probe bake writes at full weight (see IrradianceProbeGrid::FillUpdateCB)

        constexpr uint32_t kGIProbeCacheCoeffCount = 9u;

        struct GIProbeCacheFileHeader
        {
            uint32_t magic = kGIProbeCacheMagic;
            uint32_t version = kGIProbeCacheVersion;
            uint32_t headerSize = 0u;
            uint32_t coeffCount = kGIProbeCacheCoeffCount;
            uint32_t countX = 0u;
            uint32_t countY = 0u;
            uint32_t countZ = 0u;
            uint32_t reserved = 0u;
            float origin[3] = {};
            float spacing[3] = {};
            uint64_t stateHash = 0u;
            uint64_t dataSizeBytes = 0u;
        };
    }

    void Renderer::RefreshGIBakeStatus(GIBakeState state, uint32_t bvhMissingMask)
    {
        const uint32_t total = m_probeGrid.GetTotalProbeCount();
        const uint32_t completed = m_probeGrid.GetBakedProbeCount();
        const uint32_t probesPerStep = IrradianceProbeGrid::GetProbesPerBakeStep();
        const uint32_t remaining = (total > completed) ? (total - completed) : 0u;

        m_giBakeStatus.state = state;
        m_giBakeStatus.requested = m_giBakeRequested;
        m_giBakeStatus.progress = (total > 0u)
            ? std::min(1.0f, static_cast<float>(completed) / static_cast<float>(total))
            : 0.0f;
        m_giBakeStatus.completedProbes = completed;
        m_giBakeStatus.totalProbes = total;
        m_giBakeStatus.probesPerStep = probesPerStep;
        m_giBakeStatus.estimatedFramesRemaining = (probesPerStep > 0u)
            ? ((remaining + probesPerStep - 1u) / probesPerStep)
            : 0u;
        m_giBakeStatus.bvhMissingMask = (state == GIBakeState::WaitingForBvh)
            ? bvhMissingMask
            : 0u;
        m_giBakeStatus.sceneInstances = static_cast<uint32_t>(m_rayTracingScene.instances.size());
        m_giBakeStatus.sceneTriangles = m_rayTracingScene.TriangleCount();
        m_giBakeStatus.sceneGeometryVersion = m_rayTracingScene.geometryVersion;
        m_giBakeStatus.sceneMaterialVersion = m_rayTracingScene.materialVersion;
        m_giBakeStatus.sceneInstanceVersion = m_rayTracingScene.instanceVersion;

        if (state == GIBakeState::WaitingForProbeGrid ||
            state == GIBakeState::WaitingForBvh) {
            ++m_giBakeStatus.stalledFrames;
        } else {
            m_giBakeStatus.stalledFrames = 0u;
        }
    }

    void Renderer::SetGIBakePhase(const char* phase)
    {
        std::snprintf(m_giBakeStatus.currentPhase,
                      sizeof(m_giBakeStatus.currentPhase),
                      "%s",
                      phase ? phase : "");
    }

    void Renderer::AddGIBakeLog(const char* phase, const char* format, ...)
    {
        GIBakeStatus::LogEntry entry{};
        entry.sequence = ++m_giBakeLogSequence;
        entry.bakeFrame = m_giBakeFrameIndex;
        entry.state = m_giBakeStatus.state;
        std::snprintf(entry.phase, sizeof(entry.phase), "%s", phase ? phase : "");

        va_list args;
        va_start(args, format);
        std::vsnprintf(entry.message, sizeof(entry.message), format ? format : "", args);
        va_end(args);

        if (m_giBakeStatus.logCount < GIBakeStatus::kLogCapacity) {
            m_giBakeStatus.logEntries[m_giBakeStatus.logCount++] = entry;
            return;
        }

        for (uint32_t i = 1u; i < GIBakeStatus::kLogCapacity; ++i) {
            m_giBakeStatus.logEntries[i - 1u] = m_giBakeStatus.logEntries[i];
        }
        m_giBakeStatus.logEntries[GIBakeStatus::kLogCapacity - 1u] = entry;
    }

    Renderer::GIBakeStatus Renderer::GetGIBakeStatus() const
    {
        GIBakeStatus status = m_giBakeStatus;
        const uint32_t total = m_probeGrid.GetTotalProbeCount();
        const uint32_t completed = m_probeGrid.GetBakedProbeCount();
        const uint32_t probesPerStep = IrradianceProbeGrid::GetProbesPerBakeStep();
        const uint32_t remaining = (total > completed) ? (total - completed) : 0u;

        status.requested = m_giBakeRequested;
        status.progress = (total > 0u)
            ? std::min(1.0f, static_cast<float>(completed) / static_cast<float>(total))
            : 0.0f;
        status.completedProbes = completed;
        status.totalProbes = total;
        status.probesPerStep = probesPerStep;
        status.estimatedFramesRemaining = (probesPerStep > 0u)
            ? ((remaining + probesPerStep - 1u) / probesPerStep)
            : 0u;
        status.sceneInstances = static_cast<uint32_t>(m_rayTracingScene.instances.size());
        status.sceneTriangles = m_rayTracingScene.TriangleCount();
        status.sceneGeometryVersion = m_rayTracingScene.geometryVersion;
        status.sceneMaterialVersion = m_rayTracingScene.materialVersion;
        status.sceneInstanceVersion = m_rayTracingScene.instanceVersion;
        if (!m_giBakeRequested && m_probeGrid.IsBaked()) {
            status.state = GIBakeState::Completed;
        } else if (!m_giBakeRequested && status.state != GIBakeState::Failed) {
            status.state = GIBakeState::Idle;
        }

        return status;
    }

    void Renderer::RequestGIBake()
    {
        m_probeGrid.ResetBakeState();
        m_giBakeFrameIndex = 0u;
        m_giBakeLastLoggedMissingMask = 0xFFFFFFFFu;
        m_giBakeLastLoggedCompletedProbes = 0xFFFFFFFFu;
        SetGIBakePhase("Request queued");
        m_giBakeRequested = true;
        AddGIBakeLog("Request", "Bake requested. scene=%u instances / %u triangles",
                     static_cast<uint32_t>(m_rayTracingScene.instances.size()),
                     m_rayTracingScene.TriangleCount());
        RefreshGIBakeStatus(m_probeGrid.IsInitialized()
            ? GIBakeState::Baking
            : GIBakeState::WaitingForProbeGrid);
    }

    void Renderer::ResetAndRebakeGI()
    {
        m_probeGrid.ResetBakeState();
        m_giBakeFrameIndex   = 0u;
        m_giBakeLastLoggedMissingMask = 0xFFFFFFFFu;
        m_giBakeLastLoggedCompletedProbes = 0xFFFFFFFFu;
        // Do not reallocate the probe buffer here. The current frame may already
        // have recorded lighting commands that read the existing buffer.
        m_giBakeClearPending = false;
        m_giBakeRequested    = true;
        SetGIBakePhase("Rebuild queued");
        AddGIBakeLog("Request", "Rebuild queued. scene=%u instances / %u triangles, versions=%llu/%llu/%llu",
                     static_cast<uint32_t>(m_rayTracingScene.instances.size()),
                     m_rayTracingScene.TriangleCount(),
                     static_cast<unsigned long long>(m_rayTracingScene.geometryVersion),
                     static_cast<unsigned long long>(m_rayTracingScene.materialVersion),
                     static_cast<unsigned long long>(m_rayTracingScene.instanceVersion));
        RefreshGIBakeStatus(m_probeGrid.IsInitialized()
            ? GIBakeState::Baking
            : GIBakeState::WaitingForProbeGrid);
    }

    void Renderer::CancelGIBake()
    {
        m_giBakeRequested = false;
        SetGIBakePhase("Canceled");
        AddGIBakeLog("Cancel", "Bake canceled at %u/%u probes.",
                     m_probeGrid.GetBakedProbeCount(),
                     m_probeGrid.GetTotalProbeCount());
        RefreshGIBakeStatus(m_probeGrid.HasEverBaked()
            ? GIBakeState::Completed
            : GIBakeState::Idle);
    }

    bool Renderer::SaveGIProbeCache(const std::string& path, uint64_t stateHash)
    {
        if (!m_device || path.empty() || !m_probeGrid.IsInitialized() || !m_probeGrid.HasEverBaked()) {
            return false;
        }

        std::vector<uint8_t> probeData;
        if (!m_probeGrid.ExportProbeData(*m_device, probeData) || probeData.empty()) {
            return false;
        }

        GIProbeCacheFileHeader header{};
        header.headerSize = sizeof(GIProbeCacheFileHeader);
        header.countX = m_probeGrid.GetCountX();
        header.countY = m_probeGrid.GetCountY();
        header.countZ = m_probeGrid.GetCountZ();
        header.origin[0] = m_probeGrid.GetOriginX();
        header.origin[1] = m_probeGrid.GetOriginY();
        header.origin[2] = m_probeGrid.GetOriginZ();
        header.spacing[0] = m_probeGrid.GetSpacingX();
        header.spacing[1] = m_probeGrid.GetSpacingY();
        header.spacing[2] = m_probeGrid.GetSpacingZ();
        header.stateHash = stateHash;
        header.dataSizeBytes = static_cast<uint64_t>(probeData.size());

        // Write to a temporary and rename, matching StaticMeshCache::Save: an interrupted
        // save must never destroy the previously baked cache or leave a half-written file
        // in its place.
        const std::filesystem::path finalPath(path);
        std::error_code ec;
        if (finalPath.has_parent_path()) {
            std::filesystem::create_directories(finalPath.parent_path(), ec);
            if (ec) {
                return false;
            }
        }

        std::filesystem::path tempPath = finalPath;
        tempPath += ".tmp";

        {
            std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                return false;
            }

            out.write(reinterpret_cast<const char*>(&header), sizeof(header));
            out.write(reinterpret_cast<const char*>(probeData.data()),
                      static_cast<std::streamsize>(probeData.size()));
            // Flush explicitly: a failure inside the destructor's flush would otherwise go
            // unnoticed and rename a short cache into place.
            out.flush();
            if (!out.good()) {
                out.close();
                std::error_code cleanupEc;
                std::filesystem::remove(tempPath, cleanupEc);
                return false;
            }
        }

        ec.clear();
        std::filesystem::remove(finalPath, ec);
        ec.clear();
        std::filesystem::rename(tempPath, finalPath, ec);
        if (ec) {
            std::error_code cleanupEc;
            std::filesystem::remove(tempPath, cleanupEc);
            return false;
        }
        return true;
    }

    bool Renderer::LoadGIProbeCache(const std::string& path, uint64_t stateHash)
    {
        if (!m_device || path.empty() || !m_probeGrid.IsInitialized()) {
            return false;
        }

        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            return false;
        }

        GIProbeCacheFileHeader header{};
        in.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!in.good() ||
            header.magic != kGIProbeCacheMagic ||
            header.version != kGIProbeCacheVersion ||
            header.headerSize != sizeof(GIProbeCacheFileHeader) ||
            header.coeffCount != kGIProbeCacheCoeffCount ||
            header.stateHash != stateHash ||
            header.countX != m_probeGrid.GetCountX() ||
            header.countY != m_probeGrid.GetCountY() ||
            header.countZ != m_probeGrid.GetCountZ() ||
            !NearlyEqual(header.origin[0], m_probeGrid.GetOriginX()) ||
            !NearlyEqual(header.origin[1], m_probeGrid.GetOriginY()) ||
            !NearlyEqual(header.origin[2], m_probeGrid.GetOriginZ()) ||
            !NearlyEqual(header.spacing[0], m_probeGrid.GetSpacingX()) ||
            !NearlyEqual(header.spacing[1], m_probeGrid.GetSpacingY()) ||
            !NearlyEqual(header.spacing[2], m_probeGrid.GetSpacingZ()) ||
            header.dataSizeBytes != m_probeGrid.GetProbeDataSizeBytes()) {
            return false;
        }

        std::vector<uint8_t> probeData(static_cast<size_t>(header.dataSizeBytes));
        in.read(reinterpret_cast<char*>(probeData.data()),
                static_cast<std::streamsize>(probeData.size()));
        if (!in.good()) {
            return false;
        }

        if (!m_probeGrid.ImportProbeData(*m_device, probeData.data(), header.dataSizeBytes)) {
            return false;
        }

        m_giBakeRequested = false;
        m_giBakeFrameIndex = 0u;
        SetGIBakePhase("Loaded GI probe cache");
        RefreshGIBakeStatus(GIBakeState::Completed);
        AddGIBakeLog("Cache", "Loaded GI probe cache. probes=%u/%u",
                     m_probeGrid.GetBakedProbeCount(),
                     m_probeGrid.GetTotalProbeCount());
        return true;
    }

    void Renderer::FitProbeGridToScene(float bMinX, float bMinY, float bMinZ,
                                        float bMaxX, float bMaxY, float bMaxZ,
                                        float margin)
    {
        m_probeGrid.FitToSceneBounds(bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ, margin);
        if (m_probeGrid.IsInitialized() && m_device) {
            m_probeGrid.ReallocAndClearProbeBuffer(*m_device);
        } else {
            m_probeGrid.ResetBakeState();
        }
        RefreshGIBakeStatus(m_giBakeRequested
            ? GIBakeState::Baking
            : GIBakeState::Idle);
    }

    bool Renderer::GetSceneWorldBounds(float outMin[3], float outMax[3]) const
    {
        if (m_rayTracingScene.instances.empty()) {
            return false;
        }

        float minB[3] = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
        float maxB[3] = { -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max() };
        for (const auto& instance : m_rayTracingScene.instances) {
            for (int axis = 0; axis < 3; ++axis) {
                minB[axis] = std::min(minB[axis], instance.worldBoundsMin[axis]);
                maxB[axis] = std::max(maxB[axis], instance.worldBoundsMax[axis]);
            }
        }

        if (minB[0] > maxB[0] || minB[1] > maxB[1] || minB[2] > maxB[2]) {
            return false;
        }

        outMin[0] = minB[0]; outMin[1] = minB[1]; outMin[2] = minB[2];
        outMax[0] = maxB[0]; outMax[1] = maxB[1]; outMax[2] = maxB[2];
        return true;
    }

    bool Renderer::FitProbeGridToSceneAuto(float margin, std::uint32_t probeBudget)
    {
        float sceneMin[3];
        float sceneMax[3];
        if (!GetSceneWorldBounds(sceneMin, sceneMax)) {
            return false;
        }

        m_probeGrid.FitToSceneBoundsWithBudget(sceneMin[0], sceneMin[1], sceneMin[2],
                                                sceneMax[0], sceneMax[1], sceneMax[2],
                                                margin, probeBudget);
        if (m_probeGrid.IsInitialized() && m_device) {
            m_probeGrid.ReallocAndClearProbeBuffer(*m_device);
        } else {
            m_probeGrid.ResetBakeState();
        }
        RefreshGIBakeStatus(m_giBakeRequested
            ? GIBakeState::Baking
            : GIBakeState::Idle);
        return true;
    }

    float Renderer::SetProbeGridSpacing(float targetSpacing, std::uint32_t probeBudget)
    {
        // A zero or negative spacing would divide by zero when the grid counts are derived.
        const float requested = (targetSpacing > 0.01f) ? targetSpacing : 0.01f;
        m_probeGrid.SetGridSpacing(requested, requested, requested);

        // Reuses the existing fit path, which also reallocates and clears the probe buffer.
        // With the base spacing already set, the budget can only coarsen from here.
        if (!FitProbeGridToSceneAuto(2.0f, probeBudget)) {
            // No scene bounds yet: the spacing is stored and will take effect at the next fit.
            return m_probeGrid.GetSpacingX();
        }

        // The old probe data describes a different grid, so it cannot be reused.
        ResetAndRebakeGI();
        return m_probeGrid.GetSpacingX();
    }
}
