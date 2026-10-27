#pragma once

namespace SasamiEngine::App::UI
{
    // Holds the in-progress value of the one widget currently being edited.
    //
    // The UI parts commit to the application only when an edit finishes, but callers
    // re-read the current value from a getter every frame. Without somewhere to keep
    // the uncommitted value, a slider would snap back to the old value on the next
    // frame and could not be dragged at all. ImGui only ever has one active item, so
    // a single slot is enough.
    //
    // Values are kept as floats; the integer part is exact below 2^24, which covers
    // every integer control in this UI.
    class EditBuffer
    {
    public:
        static constexpr int kMaxComponents = 4;

        // Overwrites values[0..count) with the in-progress value when `id` is the widget
        // being edited. Returns false and leaves values untouched otherwise.
        bool TryLoad(unsigned int id, float* values, int count) const;

        void Store(unsigned int id, const float* values, int count);
        void Clear();

    private:
        unsigned int m_id    = 0;   // 0 is ImGui's "no item" id
        int          m_count = 0;
        float        m_values[kMaxComponents]{};
    };

    // Frame-spanning UI state. The UI is built from the main thread only.
    EditBuffer& GetEditBuffer();
}
namespace SasamiEngine { using namespace App; }
