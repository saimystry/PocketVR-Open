// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pocketvr {
// All times are nanoseconds in the PC clock; the phone converts its own before sending.
struct PcFrameTiming {
    uint64_t frame_id{}, render_ns{}, send_start_ns{}, send_end_ns{};
    uint32_t bytes{};
    double encode_ms{};
};
struct PhoneFrameTiming {
    uint64_t frame_id{}, received_ns{}, queued_ns{}, decoded_ns{}, displayed_ns{};
};
struct FrameLatency {
    uint64_t frame_id{};
    uint32_t bytes{};
    double encode_ms{}, before_send_ms{}, send_ms{}, network_ms{}, queue_ms{}, decode_ms{}, display_ms{}, total_ms{};
};
// Moves a full log aside, so sessions cannot grow it forever while the newest run and
// the one before it stay available. Returns the bytes already in the file at `path`.
inline uint64_t prepare_latency_file(const std::filesystem::path &path, uint64_t limit) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return 0;
    if (size <= limit)
        return size;
    auto previous = path;
    previous.replace_extension(".previous.csv");
    std::filesystem::rename(path, previous, error);
    if (error)
        std::filesystem::remove(path, error);
    return 0;
}
inline const char *latency_csv_header() {
    return "frame_id,bytes,encode_ms,before_send_ms,send_ms,network_ms,queue_ms,decode_ms,display_ms,total_ms\n";
}
inline std::string latency_csv_row(const FrameLatency &f) {
    const auto text = [](double value) { return std::to_string(value); };
    return std::to_string(f.frame_id) + ',' + std::to_string(f.bytes) + ',' + text(f.encode_ms) + ',' +
           text(f.before_send_ms) + ',' + text(f.send_ms) + ',' + text(f.network_ms) + ',' + text(f.queue_ms) +
           ',' + text(f.decode_ms) + ',' + text(f.display_ms) + ',' + text(f.total_ms) + '\n';
}
// Joins the two halves of a frame's journey, which arrive up to a second apart.
class LatencyJoin final {
  public:
    void add_pc(const PcFrameTiming &frame) {
        pc_[frame.frame_id % pc_.size()] = frame;
    }
    std::optional<FrameLatency> add_phone(const PhoneFrameTiming &phone) {
        const auto &pc = pc_[phone.frame_id % pc_.size()];
        if (!phone.frame_id || pc.frame_id != phone.frame_id || pc.send_start_ns < pc.render_ns ||
            pc.send_end_ns < pc.send_start_ns || phone.queued_ns < phone.received_ns ||
            phone.decoded_ns < phone.queued_ns || phone.displayed_ns < phone.decoded_ns ||
            phone.displayed_ns < pc.render_ns)
            return {};
        const auto ms = [](uint64_t from, uint64_t to) { return static_cast<double>(to - from) / 1e6; };
        FrameLatency frame;
        frame.frame_id = phone.frame_id;
        frame.bytes = pc.bytes;
        frame.encode_ms = pc.encode_ms;
        frame.before_send_ms = ms(pc.render_ns, pc.send_start_ns);
        frame.send_ms = ms(pc.send_start_ns, pc.send_end_ns);
        // Clock estimates can put arrival a little before the last send.
        frame.network_ms = phone.received_ns > pc.send_end_ns ? ms(pc.send_end_ns, phone.received_ns) : 0;
        frame.queue_ms = ms(phone.received_ns, phone.queued_ns);
        frame.decode_ms = ms(phone.queued_ns, phone.decoded_ns);
        frame.display_ms = ms(phone.decoded_ns, phone.displayed_ns);
        frame.total_ms = ms(pc.render_ns, phone.displayed_ns);
        recent_[joined_++ % recent_.size()] = frame;
        return frame;
    }
    nlohmann::json summary() const {
        const auto count = static_cast<size_t>(std::min<uint64_t>(joined_, recent_.size()));
        nlohmann::json result{{"samples", count}};
        const auto stage = [&](const char *name, double FrameLatency::*field) {
            std::vector<double> values;
            values.reserve(count);
            for (size_t i = 0; i < count; ++i)
                values.push_back(recent_[i].*field);
            std::sort(values.begin(), values.end());
            const auto at = [&](double share) {
                return values.empty() ? 0.0 : values[static_cast<size_t>(share * static_cast<double>(values.size() - 1))];
            };
            result[name] = {{"p50_ms", at(0.5)}, {"p95_ms", at(0.95)}};
        };
        stage("encode", &FrameLatency::encode_ms);
        stage("before_send", &FrameLatency::before_send_ms);
        stage("send", &FrameLatency::send_ms);
        stage("network", &FrameLatency::network_ms);
        stage("queue", &FrameLatency::queue_ms);
        stage("decode", &FrameLatency::decode_ms);
        stage("display", &FrameLatency::display_ms);
        stage("total", &FrameLatency::total_ms);
        double bits{};
        for (size_t i = 0; i < count; ++i)
            bits += static_cast<double>(recent_[i].bytes) * 8;
        result["average_frame_kilobits"] = count ? bits / static_cast<double>(count) / 1000 : 0.0;
        return result;
    }

  private:
    std::array<PcFrameTiming, 256> pc_{};
    std::array<FrameLatency, 240> recent_{};
    uint64_t joined_{};
};
// When a frame's next packet should leave. A burst overflows Wi-Fi queues and turns
// into loss, so packets leave at the rate that fits a typical frame into the spread
// window. A small frame finishes early instead of being stretched over the whole
// window; a large one speeds up to stay inside it. Zero sends at once.
inline uint64_t paced_send_ns(uint64_t start_ns, uint32_t index, uint32_t total, uint64_t interval_ns,
                              unsigned spread_percent, uint32_t typical_total = 0) {
    if (!total || !spread_percent)
        return start_ns;
    const auto window = interval_ns / 100 * std::min(spread_percent, 100U);
    return start_ns + window / std::max(total, typical_total) * std::min(index, total);
}
} // namespace pocketvr
