#include "UIVolume.h"

#include "UIEditBuffer.h"
#include "UICommitPolicy.h"

#include "imgui.h"

namespace SasamiEngine::App::UI
{
    bool Volume(const char* label, float* value, float min, float max,
                const char* format, bool logarithmic)
    {
        const ImGuiSliderFlags flags = logarithmic ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None;

        // Realtime: submit the widget and report every frame it changes. The EditBuffer is
        // deliberately not used -- the caller writes the value straight back, so the getter it
        // reads next frame already returns the new value and there is nothing to snap back from.
        // IsItemDeactivatedAfterEdit is still checked so a Ctrl+click typed entry is not missed.
        if (CurrentCommitPolicy() == CommitPolicy::Realtime) {
            const bool changed = ImGui::SliderFloat(label, value, min, max, format, flags);
            return changed || ImGui::IsItemDeactivatedAfterEdit();
        }


        const ImGuiID id = ImGui::GetID(label);
        GetEditBuffer().TryLoad(id, value, 1);
        ImGui::SliderFloat(label, value, min, max, format, flags);
        if (ImGui::IsItemActive()) {
            GetEditBuffer().Store(id, value, 1);
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
