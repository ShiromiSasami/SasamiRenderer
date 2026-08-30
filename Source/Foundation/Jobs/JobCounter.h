#pragma once

#include <atomic>
#include <cstdint>

namespace SasamiRenderer
{
    // Fork-join synchronization primitive. Reset() to the number of jobs about
    // to be kicked, Decrement() once per completed job, Wait() blocks the
    // calling OS thread until the count reaches zero.
    //
    // Built on C++20 std::atomic::wait/notify (futex-backed on Windows via
    // WaitOnAddress) instead of condition_variable + mutex.
    class JobCounter
    {
    public:
        JobCounter() = default;
        JobCounter(const JobCounter&) = delete;
        JobCounter& operator=(const JobCounter&) = delete;

        void Reset(int32_t count)
        {
            m_value.store(count, std::memory_order_release);
        }

        void Decrement()
        {
            if (m_value.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                m_value.notify_all();
            }
        }

        void Wait() const
        {
            int32_t v;
            while ((v = m_value.load(std::memory_order_acquire)) != 0) {
                m_value.wait(v, std::memory_order_acquire);
            }
        }

        int32_t Get() const
        {
            return m_value.load(std::memory_order_acquire);
        }

    private:
        std::atomic<int32_t> m_value{ 0 };
    };
}
