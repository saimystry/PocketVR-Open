// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "reassembler.hpp"
#include "tracking.hpp"
#include <atomic>

namespace pocketvr {
// Every later frame depends on this one, so dropping a slightly late frame costs a
// freeze until the next keyframe. Only a frame this old is worth abandoning.
inline constexpr uint64_t decoder_abandon_ns = 250'000'000;
inline constexpr uint64_t keyframe_request_gap_ns = 500'000'000;
// A keyframe request can reach the PC on both the fast media path and the reliable
// control path. Both must not produce a keyframe, because each one is expensive.
class KeyframeRequestGate final {
    std::atomic<uint64_t> last_ns_{};

  public:
    bool accept(uint64_t now_ns) noexcept {
        auto last = last_ns_.load();
        return (!last || now_ns < last || now_ns - last >= 250'000'000) &&
               last_ns_.compare_exchange_strong(last, now_ns);
    }
};
// One network producer and one decoder consumer. Full queues reject complete
// access units; ownership returns only after the decoder has copied the bytes.
class MediaQueue final {
  public:
    struct Lease {
        uint8_t slot{};
        wire::VideoHeader header{};
        std::span<const uint8_t> bytes;
        uint64_t received_ns{};
    };
    explicit MediaQueue(size_t capacity = 8 * 1024 * 1024) : capacity_(capacity) {
        if (!capacity || capacity > 8 * 1024 * 1024)
            throw std::invalid_argument("Invalid media queue capacity");
        for (uint8_t i = 0; i < slots_.size(); ++i) {
            slots_[i].bytes.resize(capacity);
            free_.push(i);
        }
    }
    bool push(FrameView frame, uint64_t received_ns) {
        if (frame.bytes.empty() || frame.bytes.size() > capacity_ ||
            frame.header.frame_bytes != frame.bytes.size())
            return false;
        uint8_t index{};
        if (!free_.pop(index))
            return false;
        auto &slot = slots_[index];
        std::copy(frame.bytes.begin(), frame.bytes.end(), slot.bytes.begin());
        slot.header = frame.header;
        slot.received_ns = received_ns;
        slot.reading = false;
        return ready_.push(index);
    }
    std::optional<Lease> pop() {
        uint8_t index{};
        if (!ready_.pop(index))
            return {};
        auto &slot = slots_[index];
        slot.reading = true;
        return Lease{index, slot.header, std::span(slot.bytes).first(slot.header.frame_bytes),
                     slot.received_ns};
    }
    void release(uint8_t index) {
        if (index >= slots_.size() || !slots_[index].reading)
            throw std::logic_error("Invalid decoder lease");
        slots_[index].reading = false;
        if (!free_.push(index))
            throw std::logic_error("Duplicate decoder release");
    }

  private:
    struct Slot {
        wire::Bytes bytes;
        wire::VideoHeader header;
        uint64_t received_ns{};
        bool reading{};
    };
    std::array<Slot, 3> slots_;
    size_t capacity_;
    SpscQueue<uint8_t, 4> free_, ready_;
};
} // namespace pocketvr
