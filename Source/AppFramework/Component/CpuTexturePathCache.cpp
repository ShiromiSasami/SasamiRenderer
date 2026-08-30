#include "Component/CpuTexturePathCache.h"

#include <algorithm>
#include <cwctype>
#include <vector>

#include "Loader/AssetLoader.h"
#include "Loader/DdsTextureLoader.h"

namespace SasamiRenderer
{
    struct CpuTexturePathCache::Slot
    {
        std::mutex m;
        bool done = false;
        std::shared_ptr<const CpuTextureRgba8> texture;
    };

    CpuTexturePathCache& CpuTexturePathCache::Instance()
    {
        static CpuTexturePathCache instance;
        return instance;
    }

    std::shared_ptr<const CpuTextureRgba8> CpuTexturePathCache::GetOrLoad(const std::wstring& resolvedPath)
    {
        // resolvedPath is already canonicalized by ApplicationResourcePaths::ResolveAssetPathWide,
        // so it can be used as-is as the dedup key without further normalization.
        std::shared_ptr<Slot> slot;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& entry = m_slots[resolvedPath];
            if (!entry) {
                entry = std::make_shared<Slot>();
            }
            slot = entry;
        }

        std::lock_guard<std::mutex> slotLock(slot->m);
        if (!slot->done) {
            UINT textureWidth = 0;
            UINT textureHeight = 0;
            std::vector<uint8_t> pixels;

            bool isDds = false;
            if (resolvedPath.size() >= 4) {
                std::wstring extension = resolvedPath.substr(resolvedPath.size() - 4);
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
                isDds = (extension == L".dds");
            }

            const bool loaded = isDds
                ? DdsTextureLoader::LoadRgba8FromDds(resolvedPath, pixels, textureWidth, textureHeight)
                : AssetLoader::LoadRgba8ViaWIC(resolvedPath, pixels, textureWidth, textureHeight);

            if (loaded) {
                auto textureData = std::make_shared<CpuTextureRgba8>();
                textureData->id = m_nextId.fetch_add(1, std::memory_order_relaxed);
                textureData->pixels = std::move(pixels);
                textureData->width = textureWidth;
                textureData->height = textureHeight;
                slot->texture = std::move(textureData);
            }
            slot->done = true;
        }

        return slot->texture;
    }
}
