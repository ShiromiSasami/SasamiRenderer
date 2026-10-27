#pragma once

#include <array>
#include <cstdint>

namespace SasamiEngine::Foundation
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

// Lets the other layers name Foundation types unqualified. The directive sits on SasamiEngine rather
// than on a layer, so anything nested under it -- SasamiEngine::Render, SasamiEngine::App -- finds these
// names by enclosing-namespace lookup.
//
// The SasamiRenderer -> SasamiEngine::<layer> namespace migration it was written for is finished; what
// is left is deciding whether the layers should qualify Foundation:: explicitly instead. That is not a
// mechanical change (roughly 76 files name these types unqualified), so the directive stays until then.
namespace SasamiEngine { using namespace Foundation; }
