#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace SasamiRenderer
{
    // Fixed-capacity work-stealing deque.
    //
    // "Owner thread" below means the single JobWorker that owns this queue's
    // PopBottom side (its own worker loop). PushBottom, however, is called by
    // whichever thread is kicking jobs (JobSystem::Kick/KickN runs on the
    // caller's thread, not on the owning worker's thread), so unlike the
    // textbook Chase-Lev deque this Push side is NOT restricted to a single
    // thread. A mutex therefore serializes PushBottom against PopBottom (and
    // against other concurrent PushBottom calls, e.g. two threads kicking
    // jobs to the same worker at once); without it, both sides do plain
    // (non-atomic-RMW) load+store sequences on m_bottom and can race, corrupt
    // the queue, and silently drop a job -- observed in practice as a
    // JobCounter::Wait() that hangs forever because the count never reaches
    // zero. Steal remains lock-free (CAS-based against m_top only), which is
    // what actually matters for scaling steal contention across workers.
    //
    // Based on Chase & Lev, "Dynamic Circular Work-Stealing Deque" (SPAA 2005),
    // with the C++ atomics fixed per Le, Pop, Cohen, Nardelli, "Correct and
    // Efficient Work-Stealing for Weak Memory Models" (PPoPP 2013). Capacity is
    // fixed (no dynamic array growth/resizing) to keep the implementation and
    // memory-ordering reasoning tractable for a first version -- callers must
    // size Capacity generously versus expected per-worker in-flight job counts;
    // PushBottom returns false on overflow rather than corrupting state.
    template <typename T, size_t Capacity>
    class WorkStealingDeque
    {
        static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

    public:
        WorkStealingDeque() = default;
        WorkStealingDeque(const WorkStealingDeque&) = delete;
        WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;

        // Any thread; serialized against PopBottom and other PushBottom calls.
        bool PushBottom(T value)
        {
            std::lock_guard<std::mutex> lock(m_bottomMutex);

            const int64_t b = m_bottom.load(std::memory_order_relaxed);
            const int64_t t = m_top.load(std::memory_order_acquire);
            if (b - t >= static_cast<int64_t>(Capacity)) {
                return false; // full
            }
            m_buffer[static_cast<size_t>(b) & kMask] = value;
            m_bottom.store(b + 1, std::memory_order_release);
            return true;
        }

        // Owner-thread only; serialized against PushBottom.
        std::optional<T> PopBottom()
        {
            std::lock_guard<std::mutex> lock(m_bottomMutex);

            int64_t b = m_bottom.load(std::memory_order_relaxed) - 1;
            m_bottom.store(b, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            int64_t t = m_top.load(std::memory_order_relaxed);

            if (t > b) {
                // Deque was already empty; restore bottom and bail.
                m_bottom.store(b + 1, std::memory_order_relaxed);
                return std::nullopt;
            }

            T value = m_buffer[static_cast<size_t>(b) & kMask];
            if (t == b) {
                // Last element: race against concurrent thieves via CAS on top.
                if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                    // Lost the race -- a thief took it first.
                    m_bottom.store(b + 1, std::memory_order_relaxed);
                    return std::nullopt;
                }
                m_bottom.store(b + 1, std::memory_order_relaxed);
            }
            return value;
        }

        // Any thread.
        std::optional<T> Steal()
        {
            int64_t t = m_top.load(std::memory_order_acquire);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            int64_t b = m_bottom.load(std::memory_order_acquire);

            if (t >= b) {
                return std::nullopt; // empty
            }

            T value = m_buffer[static_cast<size_t>(t) & kMask];
            if (!m_top.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                // Lost the race against another thief or the owner's PopBottom.
                return std::nullopt;
            }
            return value;
        }

    private:
        static constexpr size_t kMask = Capacity - 1;

        std::array<T, Capacity> m_buffer{};
        alignas(64) std::atomic<int64_t> m_top{ 0 };
        alignas(64) std::atomic<int64_t> m_bottom{ 0 };
        std::mutex m_bottomMutex;
    };
}
