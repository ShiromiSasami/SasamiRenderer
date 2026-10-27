#include "UIEditBuffer.h"

#include <algorithm>

namespace SasamiEngine::App::UI
{
    bool EditBuffer::TryLoad(unsigned int id, float* values, int count) const
    {
        if (id == 0 || id != m_id || values == nullptr || count != m_count) {
            return false;
        }

        std::copy_n(m_values, std::min(count, kMaxComponents), values);
        return true;
    }

    void EditBuffer::Store(unsigned int id, const float* values, int count)
    {
        if (id == 0 || values == nullptr || count <= 0) {
            return;
        }

        m_count = std::min(count, kMaxComponents);
        m_id    = id;
        std::copy_n(values, m_count, m_values);
    }

    void EditBuffer::Clear()
    {
        m_id    = 0;
        m_count = 0;
    }

    EditBuffer& GetEditBuffer()
    {
        static EditBuffer buffer;
        return buffer;
    }
}
