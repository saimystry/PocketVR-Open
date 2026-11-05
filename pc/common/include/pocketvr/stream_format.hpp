// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace pocketvr {
struct StreamFormat {
    uint32_t width{}, height{}, fps{};
    bool operator==(const StreamFormat &) const = default;
};
inline StreamFormat parse_stream_format(const nlohmann::json &value) {
    for (const auto key : {"width", "height", "fps"})
        if (!value.at(key).is_number_integer() || value.at(key) <= 0 || value.at(key) > 4096)
            throw std::invalid_argument("Invalid stream format integer");
    const StreamFormat format{value.at("width").get<uint32_t>(), value.at("height").get<uint32_t>(),
                              value.at("fps").get<uint32_t>()};
    if ((format.width % 4) || (format.height % 2) ||
        (format.fps != 30 && format.fps != 45 && format.fps != 60))
        throw std::invalid_argument("Invalid stereo stream dimensions/rate");
    return format;
}
inline nlohmann::json stream_format_json(StreamFormat format) {
    return {{"width", format.width}, {"height", format.height}, {"fps", format.fps}};
}
// Exact advertised tuples only. Preserve the startup aspect ratio and never
// exceed the user's configured format. No inferred codec capacity or upscaling.
inline std::vector<StreamFormat> stream_format_ladder(const nlohmann::json &codecs,
                                                      std::string_view mime, StreamFormat ceiling) {
    if (!codecs.is_array() || codecs.size() > 32) throw std::invalid_argument("Codec capability limit");
    std::vector<StreamFormat> formats;
    for (const auto &codec : codecs) {
        if (codec.value("mime", std::string{}) != mime) continue;
        const auto sizes = codec.value("sizes", nlohmann::json::array());
        if (!sizes.is_array() || sizes.size() > 128) throw std::invalid_argument("Codec size capability limit");
        for (const auto &size : sizes) {
            const auto f = parse_stream_format(size);
            if (f.width <= ceiling.width && f.height <= ceiling.height && f.fps <= ceiling.fps &&
                uint64_t{f.width} * ceiling.height == uint64_t{ceiling.width} * f.height &&
                std::find(formats.begin(), formats.end(), f) == formats.end()) formats.push_back(f);
        }
    }
    std::sort(formats.begin(), formats.end(), [](auto a, auto b) {
        return a.fps != b.fps ? a.fps > b.fps : a.width > b.width;
    });
    // Only use the highest-FPS resolution steps, then lower rates at the
    // smallest supported resolution. This preserves motion cadence first.
    std::vector<StreamFormat> ladder;
    for (const auto f : formats) {
        if (f.fps == ceiling.fps || (!ladder.empty() && f.width == ladder.back().width))
            ladder.push_back(f);
    }
    if (ladder.empty() || ladder.front() != ceiling) return {ceiling};
    return ladder;
}
struct FormatFeedback {
    uint64_t sequence{}, decoded{}, accepted{}, drops{}, decoder_drops{}, late{};
    double decode_p95_ms{}, encode_p95_ms{};
    bool fresh{}, clock_ready{}, encode_fresh{}, decode_fresh{}, bitrate_floor{};
    bool recovery_allowed{true};
    int thermal{-1};
};
class StreamFormatController {
    std::vector<StreamFormat> ladder_;
    uint64_t last_sequence_{}, previous_ms_{}, pressure_since_{}, clean_since_{}, hold_until_{};
    FormatFeedback previous_{};
    bool baseline_{}, pressure_{}, clean_{};
    // Every switch blanks the picture, so a format that had to be left is not
    // retried soon: 1, then 2 minutes, then never again this session.
    std::array<uint64_t, 16> retry_after_{};
    std::array<unsigned, 16> departures_{};
    void left_under_pressure(size_t index, uint64_t now) {
        const auto count = ++departures_[index];
        retry_after_[index] = count >= 3 ? UINT64_MAX : now + (60'000ULL << (count - 1));
    }
  public:
    std::string_view reason{"waiting for fresh format feedback"};
    explicit StreamFormatController(std::vector<StreamFormat> ladder) : ladder_(std::move(ladder)) {
        if (ladder_.empty() || ladder_.size() > 16) throw std::invalid_argument("Format ladder limit");
    }
    void settled(uint64_t now_ms) {
        baseline_ = pressure_ = clean_ = false; hold_until_ = now_ms + 5000;
    }
    std::optional<StreamFormat> update(uint64_t now, StreamFormat current, bool enabled,
                                        const FormatFeedback &f) {
        const auto entry = std::find(ladder_.begin(), ladder_.end(), current);
        if (entry == ladder_.end()) return {};
        if (!enabled) { reason = "fixed configured format"; return current == ladder_.front() ?
            std::nullopt : std::optional(ladder_.front()); }
        if (!f.fresh || !f.clock_ready || !f.sequence || !std::isfinite(f.decode_p95_ms) ||
            !std::isfinite(f.encode_p95_ms) || f.decode_p95_ms < 0 || f.encode_p95_ms < 0) {
            baseline_ = pressure_ = clean_ = false; reason = "waiting for fresh format feedback"; return {};
        }
        if (baseline_ && f.sequence <= last_sequence_) return {};
        const bool reset = !baseline_ || now <= previous_ms_ || now - previous_ms_ > 3000 ||
            f.decoded < previous_.decoded || f.accepted < previous_.accepted || f.drops < previous_.drops ||
            f.decoder_drops < previous_.decoder_drops || f.late < previous_.late;
        const auto progress = reset ? 0 : f.decoded - previous_.decoded;
        const auto accepted = reset ? 0 : f.accepted - previous_.accepted;
        const auto drops = reset ? 0 : f.drops - previous_.drops;
        const auto elapsed_ms = reset ? 0 : now - previous_ms_;
        // A few missed vsyncs are normal; only sustained loss of frames counts.
        const bool decode_pressure = !reset &&
            (f.decoder_drops - previous_.decoder_drops) + (f.late - previous_.late) >= 3;
        previous_ = f; previous_ms_ = now; last_sequence_ = f.sequence; baseline_ = true;
        if (reset || !progress || now < hold_until_ || f.thermal >= 4) {
            pressure_ = clean_ = false; reason = "holding format until fresh active video is stable"; return {};
        }
        const double budget = 1000.0 / current.fps;
        const bool congestion = f.bitrate_floor && static_cast<double>(drops) > .02 *
            (static_cast<double>(accepted) + static_cast<double>(drops));
        // Hardware stages overlap frames: a Pixel 4a takes about 20 ms per frame and still
        // delivers a steady 60 fps. Stage time alone is therefore no reason to switch, and
        // every switch blanks the picture. It only counts once frames stop getting through.
        // Arrivals judge the decoder, because a game below 60 fps sends fewer frames anyway.
        const auto delivered = static_cast<double>(progress);
        const bool decoder_behind = delivered < .9 * static_cast<double>(accepted);
        const bool encoder_behind =
            delivered < .9 * current.fps * static_cast<double>(elapsed_ms) / 1000.0;
        const bool slow = (f.decode_fresh && f.decode_p95_ms > budget && decoder_behind) ||
            (f.encode_fresh && f.encode_p95_ms > budget && encoder_behind);
        const bool urgent = decode_pressure || f.thermal >= 2;
        const bool overload = urgent || slow;
        if (overload || congestion) {
            clean_ = false; reason = overload ? "sustained processing/thermal pressure" : "sustained pressure at bitrate floor";
            if (!pressure_) { pressure_ = true; pressure_since_ = now; }
            if (now - pressure_since_ >= (urgent ? 2000U : congestion ? 4000U : 5000U)) {
                auto next = entry + 1;
                if (f.thermal >= 2) {
                    const auto cap = f.thermal >= 3 ? 30U : 45U;
                    while (next != ladder_.end() && next->fps > cap) ++next;
                }
                if (next != ladder_.end()) {
                    left_under_pressure(static_cast<size_t>(entry - ladder_.begin()), now);
                    return *next;
                }
            }
        } else {
            pressure_ = false; reason = "holding negotiated format";
            // Recovery requires spare capacity for the next, more demanding rate.
            const auto next = entry == ladder_.begin() ? entry : entry - 1;
            // Stage time follows pixel count, so a larger step that would not fit
            // a frame is never tried: trying it would only blank the picture.
            const double growth = static_cast<double>(next->width) * next->height /
                (static_cast<double>(current.width) * current.height);
            const double next_budget = 1000.0 / next->fps;
            const bool headroom = f.recovery_allowed && f.encode_fresh && f.decode_fresh && f.thermal < 2 && !drops &&
                f.decode_p95_ms < .7 * next_budget && f.encode_p95_ms < .7 * next_budget &&
                f.decode_p95_ms * growth < .85 * next_budget && f.encode_p95_ms * growth < .85 * next_budget;
            if (!headroom || now < retry_after_[static_cast<size_t>(next - ladder_.begin())]) clean_ = false;
            else {
                if (!clean_) { clean_ = true; clean_since_ = now; }
                if (now - clean_since_ >= 15000 && entry != ladder_.begin()) {
                    reason = "sustained format recovery headroom"; return *next;
                }
            }
        }
        return {};
    }
};
} // namespace pocketvr
