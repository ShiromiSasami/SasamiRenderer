#pragma once

// Byte-blob (de)serialization of a ClusterDag: header + raw POD arrays. Used by
// ClusterDagCache to persist built DAGs to disk; kept separate so the on-disk format
// can be tested/reasoned about without touching filesystem code.

#include "Renderer/VirtualGeometry/ClusterDag.h"

#include <cstddef>
#include <span>
#include <vector>

namespace SasamiEngine::Render
{
    class ClusterDagSerializer
    {
    public:
        static constexpr uint32_t kFormatVersion = 1;

        static std::vector<std::byte> Serialize(const ClusterDag& dag);

        // Returns false (and leaves outDag empty) on magic/version/struct-size/count mismatch
        // or truncated input. Never throws.
        static bool Deserialize(std::span<const std::byte> bytes, ClusterDag& outDag);
    };
}

namespace SasamiEngine { using namespace Render; }
