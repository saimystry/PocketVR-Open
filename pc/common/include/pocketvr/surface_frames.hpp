// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pocketvr/tracking.hpp>
#include <pocketvr/protocol.hpp>

namespace pocketvr {
struct SurfaceFrame {
    wire::VideoHeader header{};
    uint64_t timestamp_ns{}, session{}, received_ns{}, queued_ns{};
};
// One decoder producer, one GL consumer. Match the actual latched texture's
// timestamp, never an assumed newest output: SurfaceTexture may skip buffers.
class SurfaceFrames final {
  public:
    bool publish(const SurfaceFrame &frame) {
        return frame.timestamp_ns && frame.header.frame_id && pending_.push(frame);
    }
    std::optional<SurfaceFrame> match(uint64_t timestamp_ns, uint64_t session) {
        SurfaceFrame incoming;
        while (pending_.pop(incoming)) {
            recent_[next_] = incoming;
            next_ = (next_ + 1) % recent_.size();
        }
        if (!timestamp_ns || !session)
            return {};
        for (const auto &frame : recent_)
            if (frame.timestamp_ns == timestamp_ns && frame.session == session)
                return frame;
        return {};
    }

  private:
    SpscQueue<SurfaceFrame, 65> pending_;
    std::array<SurfaceFrame, 64> recent_{};
    size_t next_{};
};
inline Quat late_sampling_rotation(const wire::VideoHeader &frame, const PoseSnapshot &pose,
                                   uint64_t display_ns, bool enabled) {
    if (!enabled || !pose.tracking || !pose.timestamp_ns || display_ns < pose.timestamp_ns ||
        display_ns - pose.timestamp_ns > 250'000'000)
        return {};
    const auto predicted = predict(pose.pose.orientation, pose.pose.angular_velocity,
                                   static_cast<double>(display_ns - pose.timestamp_ns) * 1e-9);
    const auto delta = sampling_rotation(frame.render_pose.orientation, predicted);
    // Reprojection corrects a small late rotation, not an origin discontinuity.
    // A reset/recenter mismatch or a long-held frame must never rotate rays
    // through/behind the camera and stretch an entire eye into a black wedge.
    if (std::abs(delta.w) < std::cos(15 * pi / 180)) // 30 degree rotation
        return {};
    return delta;
}
inline Quat late_surface_rotation(const SurfaceFrame &frame, const PoseSnapshot &pose,
                                  uint64_t display_ns, bool enabled) {
    if (!frame.timestamp_ns || display_ns < frame.timestamp_ns ||
        display_ns - frame.timestamp_ns > 250'000'000)
        return {};
    return late_sampling_rotation(frame.header, pose, display_ns, enabled);
}
} // namespace pocketvr
