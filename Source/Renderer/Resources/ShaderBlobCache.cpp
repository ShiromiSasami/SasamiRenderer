#include "Renderer/Resources/ShaderBlobCache.h"

namespace SasamiRenderer
{
    using Microsoft::WRL::ComPtr;

    ComPtr<ID3DBlob> ShaderBlobCache::GetOrResolve(const std::wstring& key, const ResolveFn& resolve)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_blobs.find(key);
            if (it != m_blobs.end())
                return it->second;
        }

        ComPtr<ID3DBlob> blob = resolve();
        if (blob.Get() == nullptr)
            return blob;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_blobs.try_emplace(key, blob).first->second;
        }
    }
}
