// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <openvr_driver.h>
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>
#include <pocketvr/input.hpp>
#include <pocketvr/tracking.hpp>

namespace pocketvr::driver {
using HandInput = HandState;
struct DesktopInput {
    std::array<HandInput, 2> hands{};
    bool active{}, gamepad{}, recenter{};
    uint64_t received_ns{};
};
// The desktop owns a short, renewable input lease. A stopped/crashed sender
// cannot leave a trigger held. Input must be neutral before each new lease arms.
class InputMailbox {
  public:
    bool submit(const nlohmann::json &request, uint64_t now);
    bool submit(DesktopInput next, uint64_t now);
    DesktopInput read(uint64_t now) const;
    void release();
  private:
    mutable std::mutex mutex_;
    DesktopInput latest_;
    bool armed_{};
};
class HapticMailbox {
  public:
    bool push(TimedHaptic event) noexcept;
    std::vector<TimedHaptic> take(uint64_t now, bool allowed);
  private:
    std::mutex mutex_;
    std::array<TimedHaptic, 32> events_{};
    size_t head_{}, count_{};
};
class InputArbiter {
  public:
    DesktopInput read(InputMailbox &desktop, InputMailbox &phone, uint64_t now) {
        auto input=desktop.read(now);
        int source=1;
        if (!input.active) { input=phone.read(now); source=2; }
        if (!input.active) return {};
        if (owner_ && owner_!=source) {
            if (source==1) desktop.release(); else phone.release();
            input={}; // New owner must provide neutral input after this handoff.
        }
        owner_=source;
        return input;
    }
    void reset() { owner_=0; }
    int owner() const { return owner_; }
  private:
    int owner_{};
};
class ControllerInputArmer {
    bool previous_external_{}, armed_{};
  public:
    bool allow(bool externally_owned, bool tracked, const HandState &hand) {
        const bool changed = externally_owned != previous_external_;
        if (!externally_owned || changed || !tracked) armed_ = false;
        const bool neutral = hand.trigger == 0 && hand.x == 0 && hand.y == 0 &&
            !hand.grip && !hand.menu && !hand.system && !hand.pad;
        if (externally_owned && tracked && neutral) armed_ = true;
        previous_external_ = externally_owned;
        return tracked && !changed && (!externally_owned || armed_);
    }
};
vr::DriverPose_t controller_pose(vr::DriverPose_t head, bool left, bool gamepad,
                                 bool active);
vr::DriverPose_t tracked_controller_pose(const vr::DriverPose_t &head, const PoseSnapshot &sample,
                                        uint64_t now, double prediction_ms, double recenter_yaw);
class VirtualController final : public vr::ITrackedDeviceServerDriver {
  public:
    explicit VirtualController(bool left) : left_(left) {
        pose_.qRotation.w = pose_.qWorldFromDriverRotation.w = pose_.qDriverFromHeadRotation.w = 1;
    }
    vr::EVRInitError Activate(uint32_t index) override;
    void Deactivate() override;
    void EnterStandby() override {}
    void *GetComponent(const char *) override { return nullptr; }
    void DebugRequest(const char *, char *response, uint32_t size) override {
        if (response && size) response[0] = '\0';
    }
    vr::DriverPose_t GetPose() override;
    void update(const vr::DriverPose_t &head, const DesktopInput &input,
                const PoseSnapshot &external = {}, bool externally_owned = false,
                double prediction_ms = 40, double recenter_yaw = 0);
    std::optional<HapticPulse> haptic(const vr::VREvent_HapticVibration_t &event) const;
  private:
    bool left_;
    ControllerInputArmer input_armer_;
    std::atomic<uint32_t> index_{vr::k_unTrackedDeviceIndexInvalid};
    std::array<vr::VRInputComponentHandle_t, 9> handles_{};
    std::atomic<vr::VRInputComponentHandle_t> haptic_{};
    std::atomic<vr::PropertyContainerHandle_t> container_{};
    std::mutex pose_mutex_;
    vr::DriverPose_t pose_{};
};
} // namespace pocketvr::driver
