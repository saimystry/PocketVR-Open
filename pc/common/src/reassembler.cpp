// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/reassembler.hpp>
#include <algorithm>
#include <limits>
#include <vector>

namespace pocketvr {
Reassembler::Reassembler(size_t bytes, size_t slots, uint64_t deadline, FecKind kind, uint32_t fps)
    : max_frame_(bytes), max_packets_((bytes + stream_shard_bytes - 1) / stream_shard_bytes),
      max_groups_((max_packets_ + stream_group_k - 1) / stream_group_k), deadline_(deadline),
      kind_(kind) {
    if (fps != 30 && fps != 45 && fps != 60)
        throw std::invalid_argument("Reassembler frame rate");
    frame_period_ = (1'000'000'000ULL + fps - 1) / fps;
    if (bytes == 0 || bytes > 8 * 1024 * 1024 || slots < 2 || slots > 8 || deadline < 1'000'000 ||
        deadline > 1'000'000'000)
        throw std::invalid_argument("Reassembler limits");
    slots_.resize(slots);
    for (auto &s : slots_) {
        s.data.resize(max_packets_ * stream_shard_bytes);
        s.parity.resize(max_groups_ * 8 * stream_shard_bytes);
        s.groups.resize(max_groups_);
    }
}
void Reassembler::discard(Slot &s) {
    if (s.active && !s.complete) {
        ++stats_.dropped;
        stats_.needs_idr = true;
        expired_ids_[expired_next_] = s.header.frame_id;
        expired_next_ = (expired_next_ + 1) % expired_ids_.size();
        expired_count_ = std::min(expired_count_ + 1, expired_ids_.size());
    }
    s.active = false;
}
void Reassembler::expire(uint64_t now) {
    for (auto &s : slots_)
        if (s.active && !s.complete && now >= s.first_ns && now - s.first_ns > deadline_)
            discard(s);
}
void Reassembler::reset() {
    for (auto &s : slots_)
        s.active = false;
    stats_ = {};
    have_newest_ = false;
    newest_render_ = 0;
    expired_count_ = 0;
    expired_next_ = 0;
}
void Reassembler::format_rate(uint32_t fps) {
    if (fps != 30 && fps != 45 && fps != 60) throw std::invalid_argument("Reassembler frame rate");
    const auto totals = stats_;
    reset();
    stats_ = totals; stats_.needs_idr = true;
    frame_period_ = (1'000'000'000ULL + fps - 1) / fps;
}
static bool same_frame(const wire::VideoHeader &a, const wire::VideoHeader &b) {
    const auto &x = a.render_pose;
    const auto &y = b.render_pose;
    return a.frame_id == b.frame_id && a.frame_bytes == b.frame_bytes &&
           a.render_ns == b.render_ns && a.projection_id == b.projection_id &&
           a.packet_count == b.packet_count && a.flags == b.flags &&
           (x.position - y.position).norm() < 1e-6 &&
           (x.angular_velocity - y.angular_velocity).norm() < 1e-6 &&
           std::abs(x.orientation.x - y.orientation.x) < 1e-7 &&
           std::abs(x.orientation.y - y.orientation.y) < 1e-7 &&
           std::abs(x.orientation.z - y.orientation.z) < 1e-7 &&
           std::abs(x.orientation.w - y.orientation.w) < 1e-7;
}
std::optional<FrameView> Reassembler::push(const wire::VideoHeader &h,
                                           std::span<const uint8_t> shard, uint64_t now) {
    expire(now);
    if (std::find(expired_ids_.begin(),
                  expired_ids_.begin() + static_cast<ptrdiff_t>(expired_count_),
                  h.frame_id) != expired_ids_.begin() + static_cast<ptrdiff_t>(expired_count_)) {
        ++stats_.rejected;
        return {};
    }
    const auto base = static_cast<size_t>(h.group) * stream_group_k;
    const auto expected_count =
        (static_cast<size_t>(h.frame_bytes) + stream_shard_bytes - 1) / stream_shard_bytes;
    if (!wire::valid_video(h) || h.frame_bytes > max_frame_ || h.packet_count != expected_count ||
        h.shard_bytes != stream_shard_bytes || shard.size() != stream_shard_bytes ||
        base >= h.packet_count || h.k != std::min<size_t>(stream_group_k, h.packet_count - base) ||
        h.packet_index != (h.shard < h.k ? base + h.shard : base) ||
        (kind_ == FecKind::xor_parity && h.m != 1)) {
        ++stats_.rejected;
        return {};
    }
    if (have_newest_ && h.render_ns < newest_render_ && newest_render_ - h.render_ns > frame_period_) {
        ++stats_.rejected;
        return {};
    }
    Slot *target = nullptr;
    for (auto &s : slots_)
        if (s.active && s.header.frame_id == h.frame_id) {
            target = &s;
            break;
        }
    if (!target) {
        for (auto &s : slots_)
            if (!s.active) {
                target = &s;
                break;
            }
        if (!target) {
            target =
                &*std::min_element(slots_.begin(), slots_.end(), [](const auto &a, const auto &b) {
                    return a.first_ns < b.first_ns;
                });
            discard(*target);
        }
        target->active = true;
        target->complete = false;
        target->header = h;
        target->first_ns = now;
        target->received = 0;
        std::fill(target->groups.begin(), target->groups.end(), Group{});
    }
    auto &s = *target;
    if (!same_frame(s.header, h)) {
        ++stats_.rejected;
        return {};
    }
    if (s.complete) {
        ++stats_.duplicates;
        return {};
    }
    auto &group = s.groups[h.group];
    if (group.k && (group.k != h.k || group.m != h.m)) {
        ++stats_.rejected;
        return {};
    }
    group.k = h.k;
    group.m = h.m;
    if (group.present[h.shard] || group.complete) {
        ++stats_.duplicates;
        return {};
    }
    auto destination =
        h.shard < h.k ? std::span<uint8_t>(s.data).subspan((base + h.shard) * stream_shard_bytes,
                                                           stream_shard_bytes)
                      : std::span<uint8_t>(s.parity).subspan(
                            (static_cast<size_t>(h.group) * 8 + h.shard - h.k) * stream_shard_bytes,
                            stream_shard_bytes);
    std::copy(shard.begin(), shard.end(), destination.begin());
    group.present[h.shard] = 1;
    size_t present = 0;
    for (size_t i = 0; i < static_cast<size_t>(h.k + h.m); ++i)
        present += group.present[i] != 0;
    if (present >= h.k) {
        std::array<std::span<uint8_t>, 40> spans{};
        bool missing = false;
        for (size_t i = 0; i < h.k; ++i) {
            spans[i] = std::span<uint8_t>(s.data).subspan((base + i) * stream_shard_bytes,
                                                          stream_shard_bytes);
            missing |= !group.present[i];
        }
        for (size_t i = 0; i < h.m; ++i)
            spans[h.k + i] = std::span<uint8_t>(s.parity).subspan(
                (static_cast<size_t>(h.group) * 8 + i) * stream_shard_bytes, stream_shard_bytes);
        if (missing && !shared_fec(h.k, h.m, kind_)
                            .recover(std::span(spans).first(h.k + h.m),
                                     std::span(group.present).first(h.k + h.m))) {
            discard(s);
            return {};
        }
        if (missing)
            ++stats_.recovered;
        group.complete = true;
        s.received += h.k;
    }
    if (s.received == h.packet_count) {
        s.complete = true;
        ++stats_.completed;
        if (!have_newest_ || h.render_ns > newest_render_) {
            newest_render_ = h.render_ns;
            have_newest_ = true;
        }
        for (auto &old : slots_)
            if (&old != &s && old.active && old.header.render_ns < newest_render_ &&
                newest_render_ - old.header.render_ns > frame_period_)
                discard(old);
        return FrameView{s.header, std::span<const uint8_t>(s.data).first(h.frame_bytes)};
    }
    return {};
}
bool packetize_frame(std::span<const uint8_t> frame, wire::VideoHeader metadata, uint8_t m,
                     const ShardSink &sink, FecKind kind) {
    if (frame.empty() || frame.size() > 8 * 1024 * 1024 || m > 8 ||
        (kind == FecKind::xor_parity && m != 1))
        return false;
    metadata.frame_bytes = static_cast<uint32_t>(frame.size());
    metadata.packet_count =
        static_cast<uint16_t>((frame.size() + stream_shard_bytes - 1) / stream_shard_bytes);
    metadata.shard_bytes = stream_shard_bytes;
    const size_t packets = metadata.packet_count;
    const size_t groups = (packets + stream_group_k - 1) / stream_group_k;
    const auto group_header = [&](size_t group) {
        const auto base = group * stream_group_k;
        auto h = metadata;
        h.k = static_cast<uint8_t>(std::min<size_t>(stream_group_k, packets - base));
        h.m = m;
        h.group = static_cast<uint16_t>(group);
        h.packet_index = static_cast<uint16_t>(base);
        return h;
    };
    std::array<uint8_t, stream_shard_bytes> padded{};
    const auto data_shard = [&](size_t index) -> std::span<const uint8_t> {
        const auto offset = index * stream_shard_bytes;
        const auto size = std::min<size_t>(stream_shard_bytes, frame.size() - offset);
        if (size == stream_shard_bytes)
            return frame.subspan(offset, size);
        padded.fill(0);
        std::copy_n(frame.begin() + static_cast<ptrdiff_t>(offset), size, padded.begin());
        return padded;
    };
    // One shard from each group in turn, so a burst of lost packets costs each group
    // one shard instead of destroying a single group. Data goes first and parity is
    // only worked out afterwards, so a frame without loss never waits for FEC.
    for (size_t shard = 0; shard < stream_group_k; ++shard)
        for (size_t group = 0; group < groups; ++group) {
            const auto index = group * stream_group_k + shard;
            if (index >= packets)
                continue;
            auto h = group_header(group);
            h.shard = static_cast<uint8_t>(shard);
            h.packet_index = static_cast<uint16_t>(index);
            if (!sink(h, data_shard(index)))
                return false;
        }
    if (!m)
        return true;
    // Kept between frames so steady streaming does not allocate.
    static thread_local std::vector<uint8_t> parity;
    parity.resize(groups * m * stream_shard_bytes);
    const auto parity_shard = [&](size_t group, size_t shard) {
        return std::span<uint8_t>(parity).subspan((group * m + shard) * stream_shard_bytes,
                                                  stream_shard_bytes);
    };
    std::array<std::array<uint8_t, stream_shard_bytes>, stream_group_k> data;
    std::array<std::span<uint8_t>, stream_group_k + 8> spans{};
    for (size_t group = 0; group < groups; ++group) {
        const auto k = group_header(group).k;
        for (size_t i = 0; i < k; ++i) {
            const auto source = data_shard(group * stream_group_k + i);
            std::copy(source.begin(), source.end(), data[i].begin());
            spans[i] = data[i];
        }
        for (size_t i = 0; i < m; ++i)
            spans[k + i] = parity_shard(group, i);
        if (!shared_fec(k, m, kind).encode(std::span(spans).first(static_cast<size_t>(k) + m)))
            return false;
    }
    for (size_t shard = 0; shard < m; ++shard)
        for (size_t group = 0; group < groups; ++group) {
            auto h = group_header(group);
            h.shard = static_cast<uint8_t>(h.k + shard);
            if (!sink(h, parity_shard(group, shard)))
                return false;
        }
    return true;
}
} // namespace pocketvr
