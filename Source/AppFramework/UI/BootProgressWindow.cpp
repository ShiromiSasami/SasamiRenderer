#include "AppFramework/UI/BootProgressWindow.h"

#include "imgui.h"

#include <cstdio>

namespace SasamiRenderer::BootProgressWindow
{
    namespace
    {
        const char* StateGlyph(BootItemState state)
        {
            switch (state) {
            case BootItemState::Pending:
                return "...";
            case BootItemState::Running:
                return ">";
            case BootItemState::Done:
                return "OK";
            case BootItemState::Failed:
                return "X";
            default:
                return "?";
            }
        }

        void DrawItemRow(const BootStatus::Item& item)
        {
            if (item.state == BootItemState::Failed) {
                ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f),
                                    "[%s] %s",
                                    StateGlyph(item.state),
                                    item.name);
                return;
            }

            if (item.state == BootItemState::Done) {
                ImGui::Text("[%s] %s (%.1f ms)", StateGlyph(item.state), item.name, item.elapsedMs);
            } else {
                ImGui::Text("[%s] %s", StateGlyph(item.state), item.name);
            }
        }

        void DrawFullScreen(const BootStatus& status)
        {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            const ImVec2 center(viewport->Pos.x + viewport->Size.x * 0.5f,
                                 viewport->Pos.y + viewport->Size.y * 0.5f);
            ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

            const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse |
                                            ImGuiWindowFlags_NoResize |
                                            ImGuiWindowFlags_NoMove;

            if (!ImGui::Begin("Initializing", nullptr, flags)) {
                ImGui::End();
                return;
            }

            char overlay[32] = {};
            std::snprintf(overlay, sizeof(overlay), "%u / %u", status.completedCount, status.totalCount);
            ImGui::ProgressBar(status.progress, ImVec2(-1.0f, 0.0f), overlay);

            ImGui::Separator();

            for (uint32_t i = 0; i < status.itemCount; ++i) {
                DrawItemRow(status.items[i]);
            }

            ImGui::End();
        }

        void DrawCompact(const BootStatus& status)
        {
            uint32_t pendingCount = 0;
            for (uint32_t i = 0; i < status.itemCount; ++i) {
                if (status.items[i].state != BootItemState::Done) {
                    ++pendingCount;
                }
            }

            if (pendingCount == 0u) {
                return;
            }

            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            const ImVec2 anchor(viewport->Pos.x + viewport->Size.x, viewport->Pos.y);
            ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
            ImGui::SetNextWindowBgAlpha(0.6f);

            const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                                            ImGuiWindowFlags_NoResize |
                                            ImGuiWindowFlags_NoMove |
                                            ImGuiWindowFlags_NoCollapse |
                                            ImGuiWindowFlags_AlwaysAutoResize;

            if (!ImGui::Begin("BootProgressCompact", nullptr, flags)) {
                ImGui::End();
                return;
            }

            for (uint32_t i = 0; i < status.itemCount; ++i) {
                const BootStatus::Item& item = status.items[i];
                if (item.state == BootItemState::Done) {
                    continue;
                }

                if (item.state == BootItemState::Failed) {
                    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "Failed: %s", item.name);
                } else {
                    ImGui::Text("Initializing: %s...", item.name);
                }
            }

            ImGui::End();
        }
    }

    void Draw(const BootStatus& status, bool fullScreen)
    {
        if (fullScreen) {
            DrawFullScreen(status);
        } else {
            DrawCompact(status);
        }
    }
}
