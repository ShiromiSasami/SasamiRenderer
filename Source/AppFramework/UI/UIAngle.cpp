#include "UIAngle.h"

#include "UIEditBuffer.h"
#include "UICommitPolicy.h"

#include "imgui.h"

namespace SasamiEngine::App::UI
{
    bool Angle(const char* label, float* radians, float degreesMin, float degreesMax, const char* format)
    {
        // Realtime: submit the widget and report every frame it changes. The EditBuffer is
        // deliberately not used -- the caller writes the value straight back, so the getter it
        // reads next frame already returns the new value and there is nothing to snap back from.
        // IsItemDeactivatedAfterEdit is still checked so a Ctrl+click typed entry is not missed.
        if (CurrentCommitPolicy() == CommitPolicy::Realtime) {
            const bool changed = ImGui::SliderAngle(label, radians, degreesMin, degreesMax, format);
            return changed || ImGui::IsItemDeactivatedAfterEdit();
        }

        const ImGuiID id = ImGui::GetID(label);
        GetEditBuffer().TryLoad(id, radians, 1);
        ImGui::SliderAngle(label, radians, degreesMin, degreesMax, format);
        if (ImGui::IsItemActive()) {
            GetEditBuffer().Store(id, radians, 1);
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
