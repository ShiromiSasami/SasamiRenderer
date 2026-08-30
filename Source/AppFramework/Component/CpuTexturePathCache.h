#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "Renderer/Scene/RenderProxy.h"

namespace SasamiRenderer
{
    // Process-wide dedup of CPU-decoded textures keyed by resolved absolute path.
    // Same path => same CpuTextureRgba8 instance (and id), so SceneSubmitter's
    // id-based GPU texture dedup collapses shared textures to one GPU resource.
    // Thread-safe: a per-path in-flight guard prevents concurrent duplicate decodes.
    class CpuTexturePathCache
    {
    public:
        static CpuTexturePathCache& Instance();

        // resolvedPath: absolute path from ApplicationResourcePaths::ResolveAssetPathWide.
        // Returns nullptr on decode failure (failures are cached to avoid repeated attempts).
        std::shared_ptr<const CpuTextureRgba8> GetOrLoad(const std::wstring& resolvedPath);

    private:
        struct Slot;

        std::mutex m_mutex;
        std::unordered_map<std::wstring, std::shared_ptr<Slot>> m_slots;
        std::atomic<uint64_t> m_nextId{ 1 };
    };
}
