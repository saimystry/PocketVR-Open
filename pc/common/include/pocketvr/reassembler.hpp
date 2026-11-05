// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "fec.hpp"
#include "protocol.hpp"
#include <algorithm>
#include <functional>

namespace pocketvr {
constexpr uint16_t stream_shard_bytes = 1272;
constexpr uint8_t stream_group_k = 16;
// Parity shards per group for a chosen percentage. Rounds to the nearest shard: rounding
// down turned the default 10% into 6%, and gave small frames no protection at all.
inline uint8_t stream_parity_shards(size_t data_packets, unsigned percent) {
    if (!data_packets || !percent)
        return 0;
    const auto groups = (data_packets + stream_group_k - 1) / stream_group_k;
    const auto nearest = (data_packets * percent + 50 * groups) / (100 * groups);
    return static_cast<uint8_t>(std::clamp<size_t>(nearest, 1, 4));
}
struct FrameView {
    wire::VideoHeader header;
    std::span<const uint8_t> bytes;
};
struct ReassemblyStats {
    uint64_t completed{}, recovered{}, dropped{}, duplicates{}, rejected{};
    bool needs_idr{};
};
class Reassembler {
  public:
    explicit Reassembler(size_t max_frame_bytes = 4 * 1024 * 1024, size_t slots = 4,
                         uint64_t deadline_ns = 33'333'334, FecKind kind = FecKind::reed_solomon,
                         uint32_t fps = 60);
    // Returned bytes stay valid until this frame's slot is reused by a subsequent push.
    std::optional<FrameView> push(const wire::VideoHeader &header, std::span<const uint8_t> shard,
                                  uint64_t now_ns);
    void expire(uint64_t now_ns);
    void reset();
    void format_rate(uint32_t fps); // Socket worker only; retain cumulative diagnostics.
    const ReassemblyStats &stats() const {
        return stats_;
    }
    void clear_idr_request() {
        stats_.needs_idr = false;
    }

  private:
    struct Group {
        uint8_t k{}, m{};
        std::array<uint8_t, 40> present{};
        bool complete{};
    };
    struct Slot {
        bool active{}, complete{};
        wire::VideoHeader header{};
        uint64_t first_ns{};
        size_t received{};
        wire::Bytes data, parity;
        std::vector<Group> groups;
    };
    std::vector<Slot> slots_;
    size_t max_frame_, max_packets_, max_groups_;
    uint64_t deadline_, frame_period_, newest_render_{};
    bool have_newest_{};
    FecKind kind_;
    ReassemblyStats stats_;
    std::array<uint64_t, 128> expired_ids_{};
    size_t expired_count_{}, expired_next_{};
    void discard(Slot &slot);
};
using ShardSink = std::function<bool(const wire::VideoHeader &, std::span<const uint8_t>)>;
bool packetize_frame(std::span<const uint8_t> frame, wire::VideoHeader metadata,
                     uint8_t parity_count, const ShardSink &sink,
                     FecKind kind = FecKind::reed_solomon);
} // namespace pocketvr
