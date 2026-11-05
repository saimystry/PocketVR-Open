// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "math.hpp"
#include <bit>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pocketvr::wire {
using Bytes = std::vector<uint8_t>;
// Stays under a 1500-byte link once IPv4 and UDP headers are added.
constexpr size_t max_datagram = 1400, header_size = 32, tag_size = 16,
                 max_payload = max_datagram - header_size - tag_size;
constexpr uint8_t major = 1, minor = 0;
// Checked in hello. Raise it when phone and PC must change together, such as the
// shard size: otherwise a mismatch connects and then shows no picture.
constexpr int session_protocol = 2;
constexpr uint32_t magic = 0x31525650; // PVR1 in little endian.
enum class Type : uint8_t {
    video = 1,
    fec = 2,
    pose = 3,
    input = 4,
    vsync = 5,
    stats = 6,
    haptic = 7,
    ping = 8,
    pong = 9,
    keyframe = 10
};
struct Header {
    Type type{Type::pose};
    uint8_t flags{1};
    uint32_t session{};
    uint16_t device{};
    uint64_t counter{};
    uint64_t send_ns{};
};
class Writer {
  public:
    explicit Writer(std::span<uint8_t> data) : data_(data) {}
    bool u8(uint8_t v);
    bool u16(uint16_t v);
    bool u32(uint32_t v);
    bool u64(uint64_t v);
    bool f32(float v);
    bool bytes(std::span<const uint8_t> v);
    size_t size() const {
        return offset_;
    }

  private:
    std::span<uint8_t> data_;
    size_t offset_{};
};
class Reader {
  public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}
    bool u8(uint8_t &v);
    bool u16(uint16_t &v);
    bool u32(uint32_t &v);
    bool u64(uint64_t &v);
    bool f32(float &v);
    bool bytes(std::span<uint8_t> v);
    size_t remaining() const {
        return data_.size() - offset_;
    }

  private:
    std::span<const uint8_t> data_;
    size_t offset_{};
};
bool encode_header(const Header &h, std::span<uint8_t> out);
std::optional<Header> decode_header(std::span<const uint8_t> packet);
struct PosePacket {
    uint32_t sequence{};
    uint64_t sensor_ns{};
    Pose pose{};
    Vec3 linear_accel{};
    uint8_t tracking{1};
};
constexpr size_t pose_size = 65;
bool encode_pose(const PosePacket &p, std::span<uint8_t> out);
std::optional<PosePacket> decode_pose(std::span<const uint8_t> data);
struct VideoHeader {
    uint64_t frame_id{}, render_ns{};
    uint32_t frame_bytes{}, projection_id{};
    uint16_t packet_index{}, packet_count{}, slice{}, group{}, shard_bytes{};
    uint8_t k{}, m{}, shard{}, flags{};
    Pose render_pose{};
};
constexpr size_t video_header_size = 78;
constexpr size_t max_shard = max_payload - video_header_size;
bool encode_video_header(const VideoHeader &h, std::span<uint8_t> out);
std::optional<VideoHeader> decode_video_header(std::span<const uint8_t> data);
bool valid_video(const VideoHeader &h);
constexpr uint32_t max_control = 64 * 1024;
class ControlFramer {
  public:
    explicit ControlFramer(size_t capacity = max_control) : limit_(capacity) {}
    bool feed(std::span<const uint8_t> bytes, std::vector<Bytes> &messages);
    bool failed() const {
        return failed_;
    }
    static Bytes frame(std::span<const uint8_t> message);

  private:
    size_t limit_;
    Bytes pending_;
    uint32_t expected_{};
    bool failed_{};
};
} // namespace pocketvr::wire
