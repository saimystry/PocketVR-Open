// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace pocketvr {
class ClockSync {
  public:
    bool observe(int64_t phone_send, int64_t pc_receive, int64_t pc_send, int64_t phone_receive);
    std::optional<int64_t> phone_to_pc_ns(int64_t phone) const;
    std::optional<int64_t> pc_to_phone_ns(int64_t pc) const;
    double skew_ppm() const {
        return (slope_ - 1) * 1e6;
    }
    int64_t rtt_ns() const {
        return best_rtt_;
    }
    void reset();

  private:
    struct Sample {
        double phone{}, pc{};
        int64_t rtt{};
    };
    std::array<Sample, 128> samples_{};
    size_t count_{}, next_{};
    double slope_{1}, anchor_phone_{}, anchor_pc_{};
    int64_t best_rtt_{};
};
class PacingPll {
  public:
    explicit PacingPll(double target_ms = 4) : target_ms_(target_ms) {}
    double update(double arrival_lead_ms, double frame_period_ms = 1000.0 / 60);
    // One measured lead per interval (about a second). Returns the phase change to
    // spread over that interval; positive moves PC vsync later. The integral term
    // learns steady drift between the PC timer and the phone's real refresh rate.
    double report(double arrival_lead_ms, double target_lead_ms, double frame_period_ms = 1000.0 / 60);
    double phase_ms() const {
        return phase_ms_;
    }
    void reset() {
        phase_ms_ = 0;
        filtered_error_ = 0;
        drift_ms_ = 0;
        initialized_ = false;
    }

  private:
    double target_ms_, phase_ms_{}, filtered_error_{}, drift_ms_{};
    bool initialized_{};
};
} // namespace pocketvr
