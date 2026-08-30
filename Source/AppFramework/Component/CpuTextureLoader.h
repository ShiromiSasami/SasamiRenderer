#pragma once

#include <memory>
#include <string>

#include "Renderer/Scene/RenderProxy.h"

namespace SasamiRenderer
{
    // Shared WIC texture loading used by MeshComponent/SkinnedMeshComponent.
    // Backed by CpuTexturePathCache: the same resolved path always returns the same
    // CpuTextureRgba8 instance (and therefore the same texture id), so downstream
    // GPU-side id-based dedup actually deduplicates shared textures.
    std::shared_ptr<const CpuTextureRgba8> LoadCpuTextureFromPath(const std::string& path);
}
