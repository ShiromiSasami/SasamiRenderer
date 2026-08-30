#pragma once

#include <cstddef>
#include <cstdint>

namespace SasamiRenderer
{
    // Incremental FNV-1a byte hash shared by scene/ray-tracing dirty-state hashing.
    inline void HashBytes(uint64_t& hash, const void* data, size_t size)
    {
        static constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
        static constexpr uint64_t kFnvPrime       = 1099511628211ull;
        if (hash == 0ull) {
            hash = kFnvOffsetBasis;
        }
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) {
            hash ^= static_cast<uint64_t>(bytes[i]);
            hash *= kFnvPrime;
        }
    }
}
