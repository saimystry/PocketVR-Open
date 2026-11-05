// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "windows.hpp"
#include <chrono>
#include <cstdint>
#include <thread>

namespace pocketvr::driver {
// Streaming threads share the CPU with a game that can use all of it. Failure only
// leaves the default priority, so it is never an error.
inline void raise_thread_priority(int priority = THREAD_PRIORITY_ABOVE_NORMAL) noexcept {
    (void)SetThreadPriority(GetCurrentThread(), priority);
}
// A plain Sleep rounds up to the system timer tick, which can be 15.6 ms. This
// waits to well under a millisecond. Waits are not interruptible, so keep them short.
class PreciseTimer {
    win::Handle timer_{CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS)};

  public:
    void sleep(uint64_t ns) const {
        if (!ns)
            return;
        LARGE_INTEGER delay{};
        delay.QuadPart = -static_cast<LONGLONG>((ns + 99) / 100);
        if (timer_ && SetWaitableTimer(timer_.get(), &delay, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer_.get(), static_cast<DWORD>(ns / 1'000'000 + 20));
        else
            std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    }
};
} // namespace pocketvr::driver
