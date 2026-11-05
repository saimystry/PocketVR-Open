// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pocketvr/input.hpp>
#include <pocketvr/transport.hpp>
#include <algorithm>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <cmath>

namespace pocketvr::desktop {
struct Motors {
    uint16_t left{}, right{};
    bool operator==(const Motors &) const = default;
};
class RumbleEnvelope {
    struct Motor { uint64_t expires{}; uint16_t value{}; };
    std::array<Motor, 2> motors_{};
  public:
    void clear() { motors_ = {}; }
    void submit(const TimedHaptic &event, uint64_t now) {
        const auto &p = event.pulse;
        if (p.hand > 1 || p.duration_ms > 500 || !std::isfinite(p.amplitude) ||
            !std::isfinite(p.frequency) || p.amplitude < 0 || p.amplitude > 1 ||
            p.frequency < 0 || p.frequency > 1000 || now < event.timestamp_ns ||
            now - event.timestamp_ns >= 100'000'000) return;
        const auto expires = event.timestamp_ns + static_cast<uint64_t>(p.duration_ms) * 1'000'000;
        if (expires < event.timestamp_ns) return;
        motors_[p.hand] = {expires, static_cast<uint16_t>(std::lround(p.amplitude * 65535))};
    }
    Motors sample(uint64_t now) const {
        return {now < motors_[0].expires ? motors_[0].value : uint16_t{},
                now < motors_[1].expires ? motors_[1].value : uint16_t{}};
    }
};
// Independent actuator timer: stalled input RPC cannot leave XInput motors on.
// All backend calls are serialized on this worker, including zero on teardown.
class RumbleOutput {
  public:
    using Sink = std::function<void(int, Motors)>;
    explicit RumbleOutput(Sink sink) : sink_(std::move(sink)),
        worker_([this](std::stop_token stop) { run(stop); }) {}
    ~RumbleOutput() {
        worker_.request_stop(); wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    void update(int device, bool enabled, std::span<const TimedHaptic> events) {
        std::lock_guard lock(mutex_);
        const auto now = monotonic_ns();
        if (!enabled) device = -1; // Never touch a pad this helper does not own.
        const bool same_device = device == device_;
        if (!enabled || !same_device || now < heartbeat_ || now - heartbeat_ >= 250'000'000) envelope_.clear();
        device_ = device; enabled_ = enabled && device >= 0 && device < 4;
        heartbeat_ = now;
        if (enabled_ && same_device) for (const auto &event : events.first(std::min<size_t>(events.size(), 32))) envelope_.submit(event, now);
        wake_.notify_one();
    }
    void stop() {
        std::lock_guard lock(mutex_);
        enabled_ = false; device_ = -1; envelope_.clear(); wake_.notify_one();
    }
  private:
    Sink sink_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    RumbleEnvelope envelope_;
    int device_{-1}; bool enabled_{};
    uint64_t heartbeat_{};
    std::jthread worker_;
    void write(int device, Motors value) noexcept { try { if (device >= 0) sink_(device, value); } catch (...) {} }
    void run(std::stop_token stop) noexcept {
        int previous = -1;
        Motors previous_value;
        while (!stop.stop_requested()) {
            int device; Motors value;
            {
                std::unique_lock lock(mutex_);
                const auto now = monotonic_ns();
                device = device_;
                if (enabled_ && now >= heartbeat_ && now - heartbeat_ < 250'000'000) value = envelope_.sample(now);
            }
            if (device != previous) {
                write(previous, {}); previous_value = {};
            }
            if (device != previous || value != previous_value) write(device, value);
            previous = device; previous_value = value;
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(5));
        }
        write(previous, {});
    }
};
} // namespace pocketvr::desktop
