#include "UIColor.h"

#include "UIEditBuffer.h"
#include "UICommitPolicy.h"

#include "imgui.h"

namespace SasamiEngine::App::UI
{
    bool Color3(const char* label, float* rgb)
    {
        // Realtime: submit the widget and report every frame it changes. The EditBuffer is
        // deliberately not used -- the caller writes the value straight back, so the getter it
        // reads next frame already returns the new value and there is nothing to snap back from.
        // IsItemDeactivatedAfterEdit is still checked so a typed entry is not missed.
        if (CurrentCommitPolicy() == CommitPolicy::Realtime) {
            const bool changed = ImGui::ColorEdit3(label, rgb);
            return changed || ImGui::IsItemDeactivatedAfterEdit();
        }

        const ImGuiID id = ImGui::GetID(label);
        GetEditBuffer().TryLoad(id, rgb, 3);
        ImGui::ColorEdit3(label, rgb);
        if (ImGui::IsItemActive()) {
            GetEditBuffer().Store(id, rgb, 3);
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

    bool Color4(const char* label, float* rgba)
    {
        // Realtime: submit the widget and report every frame it changes. The EditBuffer is
        // deliberately not used -- the caller writes the value straight back, so the getter it
        // reads next frame already returns the new value and there is nothing to snap back from.
        // IsItemDeactivatedAfterEdit is still checked so a typed entry is not missed.
        if (CurrentCommitPolicy() == CommitPolicy::Realtime) {
            const bool changed = ImGui::ColorEdit4(label, rgba);
            return changed || ImGui::IsItemDeactivatedAfterEdit();
        }

        const ImGuiID id = ImGui::GetID(label);
        GetEditBuffer().TryLoad(id, rgba, 4);
        ImGui::ColorEdit4(label, rgba);
        if (ImGui::IsItemActive()) {
            GetEditBuffer().Store(id, rgba, 4);
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
