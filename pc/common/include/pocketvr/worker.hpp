// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

namespace pocketvr {
// NDK r26 has C++20 language support but no stop_token/jthread library headers.
// Cancellation is polled by control waits at most every 20 ms, never by a GC.
class StopToken {
  public:
    bool stop_requested() const noexcept {
        return state_ && state_->load(std::memory_order_acquire);
    }

  private:
    friend class Worker;
    std::shared_ptr<std::atomic<bool>> state_;
};
class Worker {
  public:
    Worker() = default;
    template <class F>
    explicit Worker(F action) : state_(std::make_shared<std::atomic<bool>>(false)) {
        StopToken token;
        token.state_ = state_;
        thread_ = std::thread([token, action = std::move(action)]() mutable {
            if constexpr (std::is_invocable_v<F, StopToken>)
                action(token);
            else
                action();
        });
    }
    ~Worker() {
        finish();
    }
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;
    Worker(Worker &&) noexcept = default;
    Worker &operator=(Worker &&other) noexcept {
        if (this != &other) {
            finish();
            state_ = std::move(other.state_);
            thread_ = std::move(other.thread_);
        }
        return *this;
    }
    bool request_stop() noexcept {
        return state_ && !state_->exchange(true, std::memory_order_acq_rel);
    }
    bool joinable() const noexcept {
        return thread_.joinable();
    }
    void join() {
        thread_.join();
    }
    StopToken token() const noexcept {
        StopToken token;
        token.state_ = state_;
        return token;
    }

  private:
    std::shared_ptr<std::atomic<bool>> state_;
    std::thread thread_;
    void finish() {
        request_stop();
        if (thread_.joinable())
            thread_.join();
    }
};
template <class Condition, class Lock, class Rep, class Period, class Predicate>
bool wait_with_stop(Condition &condition, Lock &lock, const StopToken &stop,
                    std::chrono::duration<Rep, Period> duration, Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (!stop.stop_requested()) {
        if (ready())
            return true;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            break;
        condition.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(20)), ready);
    }
    return !stop.stop_requested() && ready();
}
} // namespace pocketvr
