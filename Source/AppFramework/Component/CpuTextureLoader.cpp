#include "Component/CpuTextureLoader.h"

#include "ApplicationResourcePaths.h"
#include "Component/CpuTexturePathCache.h"

namespace SasamiRenderer
{
    std::shared_ptr<const CpuTextureRgba8> LoadCpuTextureFromPath(const std::string& path)
    {
        if (path.empty()) {
            return nullptr;
        }

        const std::wstring resolvedPath = ApplicationResourcePaths::ResolveAssetPathWide(path);
        return CpuTexturePathCache::Instance().GetOrLoad(resolvedPath);
    }
}
