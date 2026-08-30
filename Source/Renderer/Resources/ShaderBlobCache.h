#pragma once

#include <d3dcompiler.h>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <wrl/client.h>

namespace SasamiRenderer
{
    // Memoizes resolved shader bytecode blobs keyed by (source rel path, entry, target)
    // so the same shader is disk-loaded/compiled once per run even when several
    // pipeline-setup functions request it. Thread-safe for future parallel PSO builds.
    class ShaderBlobCache
    {
    public:
        using ResolveFn = std::function<Microsoft::WRL::ComPtr<ID3DBlob>()>;

        // Returns the cached blob for key, or runs resolve() once, caches and returns it.
        // A failed resolve (null blob) is NOT cached, so callers may retry.
        Microsoft::WRL::ComPtr<ID3DBlob> GetOrResolve(const std::wstring& key, const ResolveFn& resolve);

    private:
        std::mutex m_mutex;
        std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID3DBlob>> m_blobs;
    };
}
