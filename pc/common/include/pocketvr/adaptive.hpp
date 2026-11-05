// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <string_view>

namespace pocketvr {
// Single producer; phase accumulation preserves 45-on-60 cadence instead of
// greedily rounding it down to 30. A zero limit pauses input, not tracking.
class FrameCadence {
    uint64_t next_{};
    unsigned previous_ = UINT32_MAX;
  public:
    bool accept(uint64_t now_ns, unsigned fps) {
        if (fps != previous_) { previous_ = fps; next_ = now_ns; }
        if (!fps) return false;
        if (fps == UINT32_MAX) return true;
        const uint64_t period = 1'000'000'000 / fps;
        if (now_ns < next_) return false;
        next_ = now_ns - next_ > period * 4 ? now_ns + period : next_ + period;
        return true;
    }
};
struct GuardThermal { int status{-1}; bool fresh{}; };
struct GuardDecision { unsigned fps{}; bool changed{}; std::string_view reason{"normal"}; };
class StreamLoadGuard {
  public:
    explicit StreamLoadGuard(unsigned configured_fps);
    GuardDecision update(uint64_t now_ms, bool enabled, GuardThermal thermal,
                         double encode_p95_ms, bool encode_fresh);
    unsigned fps() const { return cap_; }
  private:
    unsigned configured_{}, cap_{};
    uint64_t overload_since_{}, recovery_since_{};
    bool overloaded_{}, recovering_{};
};
struct FecFeedback {
    uint64_t sequence{}, accepted{}, reassembly_dropped{}, recovered_groups{};
    bool clock_ready{};
};
enum class FecReason { waiting, baseline, fixed, idle, pressure, useful, clean, steady };
struct FecDecision { unsigned ratio{}; FecReason reason{FecReason::waiting}; bool changed{}; };
// Responds to observed reassembly/recovery counters, not an invented packet-loss
// percentage. Holds useful protection and retreats only after clean active video.
class StreamFecController {
  public:
    StreamFecController(unsigned floor, bool enabled);
    FecDecision update(uint64_t now_ms, const FecFeedback *fresh);
    unsigned ratio() const { return ratio_; }
    static std::string_view reason(FecReason reason);
  private:
    unsigned floor_{}, ratio_{};
    bool enabled_{}, baseline_{}, clean_{};
    uint64_t previous_ms_{}, clean_since_{}, last_change_{};
    FecFeedback previous_{};
};
struct BitrateFeedback {
    uint64_t sequence{}, accepted{}, reassembly_dropped{}, sink_rejected{}, decoded{},
        decoder_dropped{}, missed_vsync{};
    bool clock_ready{};
};
enum class BitrateReason { waiting, baseline, fixed, idle, pressure, recovery, steady };
struct BitrateDecision {
    uint32_t bits_per_second{};
    BitrateReason reason{BitrateReason::waiting};
    bool changed{};
};
// Production bitrate-only control. Counter deltas are frame/display pressure,
// never presented as measured packet loss. No inferred RTT/one-way latency.
class StreamBitrateController {
  public:
    StreamBitrateController(uint32_t initial, uint32_t minimum, uint32_t maximum, bool enabled);
    BitrateDecision update(uint64_t now_ms, const BitrateFeedback *fresh);
    uint32_t bitrate() const {
        return bitrate_;
    }
    static std::string_view reason(BitrateReason reason);

  private:
    uint32_t bitrate_, minimum_, maximum_;
    bool enabled_, baseline_{}, clean_{};
    BitrateFeedback previous_{};
    uint64_t previous_ms_{}, clean_since_{}, last_cut_{};
};
struct Quality {
    double bitrate_mbps{60};
    double scale{1};
    unsigned fps{60};
    unsigned fec_percent{10};
};
struct QualityMetrics {
    double loss_percent{}, jitter_ms{}, rtt_ms{}, decode_p95_ms{}, encode_p95_ms{}, gpu_ms{},
        lead_ms{4};
    uint64_t display_late{};
    unsigned thermal_status{};
    bool bursty{};
};
enum class QualityReason {
    none,
    congestion,
    clean_increase,
    burst_protection,
    thermal,
    processing_overload,
    recovery
};
struct QualityDecision {
    Quality value;
    QualityReason reason{QualityReason::none};
    bool request_idr{};
};
class AdaptiveController {
  public:
    AdaptiveController(Quality start = {}, double minimum_mbps = 15, double maximum_mbps = 120);
    QualityDecision update(uint64_t now_ms, const QualityMetrics &metrics);
    void enabled(bool value) {
        enabled_ = value;
    }
    const Quality &quality() const {
        return quality_;
    }
    static std::string_view reason(QualityReason reason);

  private:
    Quality quality_;
    double minimum_, maximum_;
    bool enabled_{true}, started_{};
    uint64_t last_update_{}, last_cut_{}, last_step_{}, clean_since_{}, overload_since_{},
        last_late_{};
    bool overloaded_{};
};
} // namespace pocketvr
