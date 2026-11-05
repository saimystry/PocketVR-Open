// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/protocol.hpp>
#include <algorithm>
#include <limits>

namespace pocketvr::wire {
bool Writer::u8(uint8_t v) {
    if (offset_ == data_.size())
        return false;
    data_[offset_++] = v;
    return true;
}
bool Writer::u16(uint16_t v) {
    if (data_.size() - offset_ < 2)
        return false;
    for (unsigned i = 0; i < 2; ++i)
        u8(static_cast<uint8_t>(v >> (8 * i)));
    return true;
}
bool Writer::u32(uint32_t v) {
    if (data_.size() - offset_ < 4)
        return false;
    for (unsigned i = 0; i < 4; ++i)
        u8(static_cast<uint8_t>(v >> (8 * i)));
    return true;
}
bool Writer::u64(uint64_t v) {
    if (data_.size() - offset_ < 8)
        return false;
    for (unsigned i = 0; i < 8; ++i)
        u8(static_cast<uint8_t>(v >> (8 * i)));
    return true;
}
bool Writer::f32(float v) {
    return std::isfinite(v) && u32(std::bit_cast<uint32_t>(v));
}
bool Writer::bytes(std::span<const uint8_t> v) {
    if (data_.size() - offset_ < v.size())
        return false;
    std::copy(v.begin(), v.end(), data_.begin() + static_cast<ptrdiff_t>(offset_));
    offset_ += v.size();
    return true;
}
bool Reader::u8(uint8_t &v) {
    if (offset_ == data_.size())
        return false;
    v = data_[offset_++];
    return true;
}
bool Reader::u16(uint16_t &v) {
    uint8_t a{}, b{};
    if (remaining() < 2)
        return false;
    u8(a);
    u8(b);
    v = static_cast<uint16_t>(a | (static_cast<uint16_t>(b) << 8));
    return true;
}
bool Reader::u32(uint32_t &v) {
    if (remaining() < 4)
        return false;
    v = 0;
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t b{};
        u8(b);
        v |= static_cast<uint32_t>(b) << (8 * i);
    }
    return true;
}
bool Reader::u64(uint64_t &v) {
    if (remaining() < 8)
        return false;
    v = 0;
    for (unsigned i = 0; i < 8; ++i) {
        uint8_t b{};
        u8(b);
        v |= static_cast<uint64_t>(b) << (8 * i);
    }
    return true;
}
bool Reader::f32(float &v) {
    uint32_t u{};
    if (!u32(u))
        return false;
    v = std::bit_cast<float>(u);
    return std::isfinite(v);
}
bool Reader::bytes(std::span<uint8_t> v) {
    if (remaining() < v.size())
        return false;
    std::copy_n(data_.begin() + static_cast<ptrdiff_t>(offset_), v.size(), v.begin());
    offset_ += v.size();
    return true;
}
bool encode_header(const Header &h, std::span<uint8_t> out) {
    if (out.size() < header_size || h.counter == 0 || h.session == 0 ||
        static_cast<uint8_t>(h.type) < 1 || static_cast<uint8_t>(h.type) > 10 || h.flags != 1)
        return false;
    Writer w(out);
    return w.u32(magic) && w.u8(major) && w.u8(minor) && w.u8(static_cast<uint8_t>(h.type)) &&
           w.u8(h.flags) && w.u32(h.session) && w.u16(h.device) && w.u16(0) && w.u64(h.counter) &&
           w.u64(h.send_ns);
}
std::optional<Header> decode_header(std::span<const uint8_t> packet) {
    if (packet.size() < header_size || packet.size() > max_datagram)
        return {};
    Reader r(packet);
    uint32_t m{};
    uint8_t maj{}, min{}, type{};
    uint16_t reserved{};
    Header h;
    if (!r.u32(m) || !r.u8(maj) || !r.u8(min) || !r.u8(type) || !r.u8(h.flags) ||
        !r.u32(h.session) || !r.u16(h.device) || !r.u16(reserved) || !r.u64(h.counter) ||
        !r.u64(h.send_ns))
        return {};
    if (m != magic || maj != major || min != minor || type < 1 || type > 10 || h.flags != 1 ||
        reserved || h.session == 0 || h.counter == 0)
        return {};
    h.type = static_cast<Type>(type);
    return h;
}
static bool put_vec(Writer &w, Vec3 v) {
    return v.finite() && w.f32(static_cast<float>(v.x)) && w.f32(static_cast<float>(v.y)) &&
           w.f32(static_cast<float>(v.z));
}
static bool get_vec(Reader &r, Vec3 &v) {
    float x{}, y{}, z{};
    if (!r.f32(x) || !r.f32(y) || !r.f32(z))
        return false;
    v = {x, y, z};
    return true;
}
static bool put_pose(Writer &w, const Pose &p) {
    const auto &q = p.orientation;
    return q.finite() && std::abs(q.norm2() - 1) < 0.01 && put_vec(w, p.position) &&
           w.f32(static_cast<float>(q.x)) && w.f32(static_cast<float>(q.y)) &&
           w.f32(static_cast<float>(q.z)) && w.f32(static_cast<float>(q.w)) &&
           put_vec(w, p.angular_velocity);
}
static bool get_pose(Reader &r, Pose &p) {
    float x{}, y{}, z{}, w{};
    if (!get_vec(r, p.position) || !r.f32(x) || !r.f32(y) || !r.f32(z) || !r.f32(w) ||
        !get_vec(r, p.angular_velocity))
        return false;
    p.orientation = {x, y, z, w};
    if (std::abs(p.orientation.norm2() - 1) > 0.01 || p.angular_velocity.norm() > 100 ||
        p.position.norm() > 10000)
        return false;
    p.orientation = p.orientation.unit();
    return true;
}
bool encode_pose(const PosePacket &p, std::span<uint8_t> out) {
    if (out.size() != pose_size || p.tracking > 2)
        return false;
    Writer w(out);
    return w.u32(p.sequence) && w.u64(p.sensor_ns) && put_pose(w, p.pose) &&
           put_vec(w, p.linear_accel) && w.u8(p.tracking);
}
std::optional<PosePacket> decode_pose(std::span<const uint8_t> data) {
    if (data.size() != pose_size)
        return {};
    Reader r(data);
    PosePacket p;
    if (!r.u32(p.sequence) || !r.u64(p.sensor_ns) || !get_pose(r, p.pose) ||
        !get_vec(r, p.linear_accel) || !r.u8(p.tracking) || p.tracking > 2 ||
        p.linear_accel.norm() > 1000)
        return {};
    return p;
}
bool valid_video(const VideoHeader &h) {
    return h.frame_bytes > 0 && h.frame_bytes <= 8 * 1024 * 1024 && h.packet_count > 0 &&
           h.packet_count <= 8192 && h.packet_index < h.packet_count && h.k > 0 && h.k <= 32 &&
           h.m <= 8 && h.k + h.m <= 40 && h.shard < h.k + h.m && h.shard_bytes > 0 &&
           h.shard_bytes <= max_shard && h.flags <= 1;
}
bool encode_video_header(const VideoHeader &h, std::span<uint8_t> out) {
    if (out.size() < video_header_size || !valid_video(h))
        return false;
    Writer w(out);
    return w.u64(h.frame_id) && w.u64(h.render_ns) && w.u32(h.frame_bytes) &&
           w.u32(h.projection_id) && w.u16(h.packet_index) && w.u16(h.packet_count) &&
           w.u16(h.slice) && w.u16(h.group) && w.u16(h.shard_bytes) && w.u8(h.k) && w.u8(h.m) &&
           w.u8(h.shard) && w.u8(h.flags) && put_pose(w, h.render_pose);
}
std::optional<VideoHeader> decode_video_header(std::span<const uint8_t> data) {
    if (data.size() < video_header_size)
        return {};
    Reader r(data);
    VideoHeader h;
    if (!r.u64(h.frame_id) || !r.u64(h.render_ns) || !r.u32(h.frame_bytes) ||
        !r.u32(h.projection_id) || !r.u16(h.packet_index) || !r.u16(h.packet_count) ||
        !r.u16(h.slice) || !r.u16(h.group) || !r.u16(h.shard_bytes) || !r.u8(h.k) || !r.u8(h.m) ||
        !r.u8(h.shard) || !r.u8(h.flags) || !get_pose(r, h.render_pose) || !valid_video(h))
        return {};
    return h;
}
Bytes ControlFramer::frame(std::span<const uint8_t> message) {
    if (message.empty() || message.size() > max_control)
        throw std::invalid_argument("Control message size");
    Bytes out(4 + message.size());
    Writer w(out);
    w.u32(static_cast<uint32_t>(message.size()));
    w.bytes(message);
    return out;
}
bool ControlFramer::feed(std::span<const uint8_t> bytes, std::vector<Bytes> &messages) {
    if (failed_)
        return false;
    for (auto byte : bytes) {
        pending_.push_back(byte);
        if (expected_ == 0 && pending_.size() == 4) {
            Reader r(pending_);
            r.u32(expected_);
            pending_.clear();
            if (expected_ == 0 || expected_ > limit_ || expected_ > max_control) {
                failed_ = true;
                return false;
            }
        } else if (expected_ > 0 && pending_.size() == expected_) {
            messages.push_back(std::move(pending_));
            pending_ = Bytes{};
            expected_ = 0;
        }
    }
    return true;
}
} // namespace pocketvr::wire
