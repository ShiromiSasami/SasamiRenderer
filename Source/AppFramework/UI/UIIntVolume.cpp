#include "UIIntVolume.h"

#include "UIEditBuffer.h"
#include "UICommitPolicy.h"

#include "imgui.h"

#include <cmath>

namespace SasamiEngine::App::UI
{
    bool IntVolume(const char* label, int* value, int min, int max, const char* format)
    {
        // Realtime: submit the widget and report every frame it changes. The EditBuffer is
        // deliberately not used -- the caller writes the value straight back, so the getter it
        // reads next frame already returns the new value and there is nothing to snap back from.
        // IsItemDeactivatedAfterEdit is still checked so a Ctrl+click typed entry is not missed.
        if (CurrentCommitPolicy() == CommitPolicy::Realtime) {
            const bool changed = ImGui::SliderInt(label, value, min, max, format);
            return changed || ImGui::IsItemDeactivatedAfterEdit();
        }

        const ImGuiID id = ImGui::GetID(label);

        // EditBuffer stores floats; every int in this UI is well below 2^24,
        // so the round trip through float is exact.
        float temp = static_cast<float>(*value);
        if (GetEditBuffer().TryLoad(id, &temp, 1)) {
            *value = static_cast<int>(std::lround(temp));
        }

        ImGui::SliderInt(label, value, min, max, format);
        if (ImGui::IsItemActive()) {
            temp = static_cast<float>(*value);
            GetEditBuffer().Store(id, &temp, 1);
            return false;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            GetEditBuffer().Clear();
            return true;
        }
        if (ImGui::IsItemDeactivated()) {
            GetEditBuffer().Clear();
        }
        return false;
    }
}
