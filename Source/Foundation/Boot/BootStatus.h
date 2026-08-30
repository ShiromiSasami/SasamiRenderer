#pragma once

#include <array>
#include <cstdint>

namespace SasamiRenderer
{
    // Application boot phase, advanced by StartupCoordinator (AppFramework/Boot).
    enum class BootPhase : uint8_t
    {
        CreatingRendererCore,  // renderer core init steps run between message pumps
        LoadingWithBootScreen, // boot frames render; init tasks drain in the background
        SceneReady,            // scene renders; remaining features light up as tasks finish
        Running                // all init tasks done; boot UI hidden
    };

    enum class BootItemState : uint8_t
    {
        Pending,
        Running,
        Done,
        Failed
    };

    // Snapshot of boot progress for UI display. Assembled on the main thread by
    // InitTaskScheduler::CollectStatus / StartupCoordinator::GetBootStatusSnapshot;
    // plain copyable data, no synchronization required by readers.
    struct BootStatus
    {
        struct Item
        {
            char name[48] = {};
            BootItemState state = BootItemState::Pending;
            float elapsedMs = 0.0f;
        };

        static constexpr uint32_t kMaxItems = 48u;

        BootPhase phase = BootPhase::CreatingRendererCore;
        uint32_t completedCount = 0u;
        uint32_t totalCount = 0u;
        float progress = 0.0f; // completedCount / totalCount (0 when totalCount == 0)
        std::array<Item, kMaxItems> items{};
        uint32_t itemCount = 0u;
    };
}
