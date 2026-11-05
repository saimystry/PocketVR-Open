// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pocketvr/settings.hpp>
#include <pocketvr/transport.hpp>
#include <pocketvr/tracking.hpp>
#include "windows.hpp"
#include "input.hpp"
#include "encoded_dump.hpp"
#include <atomic>
#include <mutex>
#include <memory>

namespace pocketvr::driver {
using pocketvr::PoseSnapshot;
using pocketvr::PoseState;
class State {
  public:
    explicit State(std::filesystem::path config = {});
    EncodedDump encoded_dump;
    nlohmann::json request(const nlohmann::json &input);
    Settings settings_copy() const;
    std::shared_ptr<const ViewerGeometry> viewer_geometry() const {
        return viewer_.load();
    }
    void record_frame(const wire::VideoHeader &frame);
    PoseState head;
    std::atomic<uint64_t> pose_sequence{};
    std::array<PoseState, 2> controller_tracking;
    std::array<std::atomic<bool>, 2> controller_override{};
    InputMailbox desktop_input;
    InputMailbox phone_input;
    SpscQueue<TimedHaptic,32> haptics;
    HapticMailbox desktop_haptics;
    std::atomic<unsigned> haptic_owner{}; // 0 none, 1 PC gamepad, 2 phone input.
    void route_haptic(TimedHaptic event);
    void phone_controls(const RawInput &input, uint64_t now);
    std::atomic<double> ipd_m{0.0639}, render_scale{1}, prediction_ms{40}, recenter_yaw{}, target_lead_ms{4};
    std::atomic<bool> settings_changed{true}, standby{};
    std::atomic<uint64_t> frames{}, running_start_ns{};
    std::atomic<int64_t> pacing_pending_ns{}; // Phase change still to apply; positive is later.
    std::atomic<bool> session_connected{}, test_pattern{};
    void menu_text(std::vector<std::string> lines);
    std::pair<uint64_t, std::vector<std::string>> menu_snapshot() const;
    std::atomic<bool> gpu_lost{}, restart_requested{}, restart_direct{};
    std::atomic<double> eye_height_m{1.65};
    std::atomic<uint64_t> last_stream_ns{};

  private:
    mutable std::mutex settings_mutex_, menu_mutex_;
    std::vector<std::string> menu_lines_;
    uint64_t menu_revision_{};
    nlohmann::json load_profiles() const;
    std::mutex history_mutex_;
    std::mutex bridge_mutex_;
    PoseHistory history_;
    Settings settings_;
    std::atomic<std::shared_ptr<const ViewerGeometry>> viewer_;
    std::atomic<std::shared_ptr<const InputMapping>> input_mapping_;
    std::atomic<bool> phone_recenter_held_{};
    std::filesystem::path config_;
    void apply_settings();
};
} // namespace pocketvr::driver
