// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "session.hpp"
#include "reassembler.hpp"
#include "tracking.hpp"
#include "timing.hpp"
#include "input.hpp"
#include <memory>

namespace pocketvr {
struct PhoneLinkStats {
    bool running{}, clock_ready{};
    uint64_t received{}, frames{}, dropped{}, poses{};
    int64_t rtt_ns{};
    uint64_t received_bytes{}, reassembly_dropped{}, fec_recovered{};
};
// One sensor producer, one socket worker. The frame callback must return promptly;
// its view is borrowed from the preallocated reassembly pool.
class PhoneLink final {
  public:
    using FrameSink = std::function<bool(FrameView)>;
    using KeyframeFlag = std::shared_ptr<std::atomic<bool>>;
    // Setting the keyframe flag sends a request at once from this worker. Being
    // unreliable, it only gets ahead of the request on the control channel.
    PhoneLink(Endpoint server, const SessionMaterial &material, FrameSink sink = {},
              std::unique_ptr<ITransport> transport = {}, uint32_t fps = 60,
              KeyframeFlag keyframe_wanted = {});
    ~PhoneLink();
    PhoneLink(const PhoneLink &) = delete;
    PhoneLink &operator=(const PhoneLink &) = delete;
    bool pose(const wire::PosePacket &phone_clock_pose);
    // Add to a phone time to get the PC time; valid once the clock is ready.
    int64_t pc_offset_ns() const { return pc_offset_ns_.load(); }
    bool input(const RawInput &value) { return valid_input(value) && inputs_.push(value); }
    std::optional<HapticPulse> haptic() {
        TimedHaptic event;
        while (haptics_.pop(event)) {
            const auto now=monotonic_ns();
            if (now>=event.timestamp_ns&&now-event.timestamp_ns<100'000'000) return event.pulse;
        }
        return {};
    }
    bool take_idr_request() {
        return needs_idr_.exchange(false);
    }
    PhoneLinkStats stats() const;
    void video_boundary(uint64_t render_ns, uint32_t fps) {
        if (!render_ns || (fps != 30 && fps != 45 && fps != 60))
            throw std::invalid_argument("Video format boundary");
        video_fps_.store(fps); video_boundary_.store(render_ns);
    }

  private:
    Endpoint server_;
    std::unique_ptr<ITransport> transport_;
    PacketCipher cipher_;
    Reassembler assembler_;
    FrameSink sink_;
    KeyframeFlag keyframe_wanted_;
    SpscQueue<wire::PosePacket, 8> poses_;
    SpscQueue<RawInput, 8> inputs_;
    SpscQueue<TimedHaptic,16> haptics_;
    std::atomic<bool> running_{}, clock_ready_{}, needs_idr_{};
    std::atomic<int64_t> pc_offset_ns_{};
    std::atomic<uint64_t> received_{}, frames_{}, dropped_{}, sent_poses_{};
    std::atomic<int64_t> rtt_{};
    std::atomic<uint64_t> received_bytes_{}, reassembly_dropped_{}, fec_recovered_{};
    std::atomic<uint64_t> video_boundary_{};
    std::atomic<uint32_t> video_fps_{60};
    Worker worker_;
    void run(StopToken stop) noexcept;
    bool send(wire::Type type, std::span<const uint8_t> payload);
    void want_keyframe() noexcept {
        needs_idr_.store(true);
        keyframe_wanted_->store(true);
    }
};
} // namespace pocketvr
