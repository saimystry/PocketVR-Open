// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>

namespace pocketvr {
// Authenticated reliable-control reports use receive time for freshness, like
// phone.stats. A bounded sender sample age prevents reusing an old UI reading.
class ThermalInbox {
    int status_{-1};
    uint64_t sequence_{}, received_{}, rejected_{};
  public:
    void initial(int status, uint64_t now) {
        if (status >= 0 && status <= 6) { status_ = status; received_ = now; }
    }
    bool accept(const nlohmann::json &message, uint64_t now) {
        try {
            if (message.size() != 4 || message.at("type") != "phone.thermal" ||
                !message.at("seq").is_number_integer() || message.at("seq") <= 0 || message.at("seq") > INT64_MAX ||
                !message.at("status").is_number_integer() || message.at("status") < 0 || message.at("status") > 6 ||
                !message.at("sample_age_ms").is_number_integer() || message.at("sample_age_ms") < 0 || message.at("sample_age_ms") > 1500 || !now)
                throw std::invalid_argument("Invalid thermal report");
            const auto sequence = message.at("seq").get<uint64_t>();
            if (sequence <= sequence_) { ++rejected_; return false; }
            sequence_ = sequence; status_ = message.at("status").get<int>(); received_ = now;
            return true;
        } catch (const std::exception &) { ++rejected_; return false; }
    }
    int level() const { return status_; }
    bool fresh(uint64_t now) const { return received_ && now >= received_ && now - received_ <= 3'000'000'000; }
    uint64_t rejected() const { return rejected_; }
};
struct LatencySummary {
    size_t samples{};
    double p50_ms{}, p95_ms{};
};
template <size_t Capacity = 120> class RollingLatency {
    static_assert(Capacity > 0);
    std::array<double, Capacity> samples_{};
    size_t next_{}, count_{};

  public:
    // Caller owns synchronization; invalid measurements never enter the window.
    void add(double milliseconds) {
        if (!std::isfinite(milliseconds) || milliseconds < 0 || milliseconds > 60'000)
            return;
        samples_[next_] = milliseconds;
        next_ = (next_ + 1) % Capacity;
        count_ = std::min(count_ + 1, Capacity);
    }
    LatencySummary summary() const {
        if (!count_)
            return {};
        auto sorted = samples_;
        std::sort(sorted.begin(), sorted.begin() + count_);
        // Nearest-rank percentiles, including windows of one sample.
        return {count_, sorted[(count_ + 1) / 2 - 1], sorted[(95 * count_ + 99) / 100 - 1]};
    }
};

inline nlohmann::json empty_phone_stats() {
    return {{"clock_ready", false},
            {"rtt_ms", 0.0},
            {"media_packets", 0},
            {"media_bytes", 0},
            {"accepted_frames", 0},
            {"sink_rejected_frames", 0},
            {"reassembly_dropped_frames", 0},
            {"fec_recovered_groups", 0},
            {"decoder_submitted", 0},
            {"decoder_decoded", 0},
            {"decoder_dropped", 0},
            {"decode_samples", 0},
            {"decode_p50_ms", 0.0},
            {"decode_p95_ms", 0.0},
            {"display_fps", 0.0},
            {"video_fps", 0.0},
            {"render_cpu_ms", 0.0},
            {"repeated_frames", 0},
            {"missed_vsync", 0},
            {"arrival_lead_ms", 0.0},
            {"arrival_samples", 0}};
}
inline void validate_phone_stats(const nlohmann::json &value) {
    const auto fields = empty_phone_stats();
    if (!value.is_object() || value.size() != fields.size())
        throw std::invalid_argument("Incomplete phone stats");
    for (const auto &[key, unused] : fields.items()) {
        (void)unused;
        const auto &field = value.at(key);
        if (key == "clock_ready") {
            if (!field.is_boolean())
                throw std::invalid_argument("Invalid clock status");
        } else if (key == "rtt_ms" || key == "decode_p50_ms" || key == "decode_p95_ms" || key == "arrival_lead_ms" ||
                   key == "display_fps" || key == "video_fps" || key == "render_cpu_ms") {
            const auto maximum = key == "display_fps" || key == "video_fps" ? 240.0 : 60'000.0;
            if (!field.is_number() || !std::isfinite(field.get<double>()) ||
                field.get<double>() < 0 || field.get<double>() > maximum)
                throw std::invalid_argument("Invalid phone timing");
        } else if (!field.is_number_integer() || field < 0 ||
                   field > (key == "decode_samples" ? 120 : INT64_MAX))
            throw std::invalid_argument("Invalid phone counter");
    }
    if (value.at("decode_p50_ms") > value.at("decode_p95_ms") ||
        (value.at("decode_samples") == 0 &&
         (value.at("decode_p50_ms") != 0 || value.at("decode_p95_ms") != 0)))
        throw std::invalid_argument("Invalid decode percentiles");
}

// The encrypted control thread is the writer; desktop RPC may read concurrently.
// Reception time is PC-local: phone clocks cannot keep an old report fresh.
class PhoneStatsInbox {
    mutable std::mutex mutex_;
    nlohmann::json values_;
    uint64_t sequence_{}, received_ns_{}, rejected_{};

  public:
    bool accept(const nlohmann::json &message, uint64_t now_ns) {
        try {
            const auto &seq = message.at("seq");
            if (!seq.is_number_integer() || seq <= 0 || seq > INT64_MAX || !now_ns)
                throw std::invalid_argument("Invalid stats sequence");
            const auto &values = message.at("values");
            validate_phone_stats(values);
            std::lock_guard lock(mutex_);
            if (seq.get<uint64_t>() <= sequence_) {
                ++rejected_;
                return false;
            }
            values_ = values;
            sequence_ = seq.get<uint64_t>();
            received_ns_ = now_ns;
            return true;
        } catch (const nlohmann::json::exception &) {
        } catch (const std::invalid_argument &) {
        }
        std::lock_guard lock(mutex_);
        ++rejected_;
        return false;
    }
    nlohmann::json status(uint64_t now_ns) const {
        std::lock_guard lock(mutex_);
        const bool fresh =
            received_ns_ && now_ns >= received_ns_ && now_ns - received_ns_ <= 3'000'000'000;
        return {{"available", fresh},
                {"seq", sequence_},
                {"rejected", rejected_},
                {"age_ms", received_ns_ && now_ns >= received_ns_
                               ? nlohmann::json(static_cast<double>(now_ns - received_ns_) / 1e6)
                               : nlohmann::json(nullptr)},
                {"values", fresh ? values_ : nlohmann::json(nullptr)}};
    }
};
} // namespace pocketvr
