// SPDX-License-Identifier: Apache-2.0
#include "input.hpp"
#include "boundary.hpp"
#include <pocketvr/math.hpp>
#include <pocketvr/transport.hpp>
#include <cmath>

namespace pocketvr::driver {
bool HapticMailbox::push(TimedHaptic event) noexcept {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || count_ == events_.size()) return false;
    events_[(head_ + count_) % events_.size()] = event; ++count_;
    return true;
}
std::vector<TimedHaptic> HapticMailbox::take(uint64_t now, bool allowed) {
    std::vector<TimedHaptic> result;
    std::lock_guard lock(mutex_);
    result.reserve(allowed ? count_ : 0);
    while (count_) {
        const auto event = events_[head_]; head_ = (head_ + 1) % events_.size(); --count_;
        if (allowed && now >= event.timestamp_ns && now - event.timestamp_ns < 100'000'000)
            result.push_back(event);
    }
    return result;
}
namespace {
constexpr uint64_t lease_ns = 250'000'000;
float scalar(const nlohmann::json &j, const char *name, float low, float high) {
    const auto v = j.at(name).get<float>();
    if (!std::isfinite(v) || v < low || v > high)
        throw std::invalid_argument("Desktop input axis outside range");
    return v;
}
void check(vr::EVRInputError error) {
    if (error != vr::VRInputError_None)
        throw std::runtime_error("SteamVR rejected controller input component");
}
}
bool InputMailbox::submit(const nlohmann::json &request, uint64_t now) {
    DesktopInput next;
    next.active = request.at("active").get<bool>();
    next.gamepad = request.at("gamepad").get<bool>();
    next.received_ns = now;
    const auto &hands = request.at("hands");
    if (!hands.is_array() || hands.size() != 2)
        throw std::invalid_argument("Desktop input needs exactly two hands");
    for (size_t i = 0; i < 2; ++i) {
        const auto &j = hands.at(i);
        auto &h = next.hands[i];
        h.trigger = scalar(j, "trigger", 0, 1);
        h.x = scalar(j, "x", -1, 1);
        h.y = scalar(j, "y", -1, 1);
        h.grip = j.at("grip").get<bool>();
        h.menu = j.at("menu").get<bool>();
        h.system = j.at("system").get<bool>();
        h.pad = j.at("pad").get<bool>();
    }
    return submit(next,now);
}
bool InputMailbox::submit(DesktopInput next, uint64_t now) {
    next.received_ns=now;
    bool neutral=!next.recenter;
    for (const auto &h:next.hands) {
        if (!std::isfinite(h.trigger)||!std::isfinite(h.x)||!std::isfinite(h.y)||
            h.trigger<0||h.trigger>1||std::abs(h.x)>1||std::abs(h.y)>1)
            throw std::invalid_argument("Input axis outside range");
        neutral=neutral&&h.trigger==0&&h.x==0&&h.y==0&&!h.grip&&!h.menu&&!h.system&&!h.pad;
    }
    std::lock_guard lock(mutex_);
    if (!next.active || !latest_.active || next.gamepad != latest_.gamepad ||
        now < latest_.received_ns || now - latest_.received_ns > lease_ns)
        armed_ = false;
    if (next.active && neutral) armed_ = true;
    latest_ = next;
    return armed_;
}
DesktopInput InputMailbox::read(uint64_t now) const {
    std::lock_guard lock(mutex_);
    if (!armed_ || !latest_.active || now < latest_.received_ns ||
        now - latest_.received_ns > lease_ns)
        return {};
    return latest_;
}
void InputMailbox::release() {
    std::lock_guard lock(mutex_);
    armed_ = false;
    latest_ = {};
}
vr::DriverPose_t controller_pose(vr::DriverPose_t head, bool left, bool gamepad,
                                 bool active) {
    head.deviceIsConnected = active && head.deviceIsConnected;
    head.poseIsValid = head.deviceIsConnected && head.poseIsValid;
    if (!head.poseIsValid) head.result = vr::TrackingResult_Running_OutOfRange;
    // Mouse right-hand ray follows the exact head forward axis. Gamepad hands
    // use a head-relative arm approximation, never claim measured hand poses.
    const Quat q{head.qRotation.x, head.qRotation.y, head.qRotation.z, head.qRotation.w};
    const auto offset = q.rotate(gamepad || left ? Vec3{left ? -0.2 : 0.2, -0.25, -0.35}
                                               : Vec3{0, 0, -0.1});
    head.vecPosition[0] += offset.x;
    head.vecPosition[1] += offset.y;
    head.vecPosition[2] += offset.z;
    // Head angular velocity does not describe a measured hand translation.
    for (double &v : head.vecVelocity) v = 0;
    for (double &v : head.vecAcceleration) v = 0;
    return head;
}
vr::DriverPose_t tracked_controller_pose(const vr::DriverPose_t &head, const PoseSnapshot &sample,
                                        uint64_t now, double prediction_ms, double recenter_yaw) {
    vr::DriverPose_t result{};
    result.qWorldFromDriverRotation.w = result.qDriverFromHeadRotation.w = result.qRotation.w = 1;
    const auto age = now >= sample.timestamp_ns ? now - sample.timestamp_ns : 0;
    result.deviceIsConnected = sample.timestamp_ns && age < 2'000'000'000 && head.deviceIsConnected;
    result.poseIsValid = result.deviceIsConnected && head.poseIsValid && sample.tracking && age <= 250'000'000;
    result.result = result.poseIsValid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
    const auto horizon = result.poseIsValid ? std::clamp(std::clamp(prediction_ms, 0.0, 80.0) * 0.001 +
        static_cast<double>(age) / 1e9, 0.0, 0.08) : 0.0;
    auto orientation = predict(sample.pose.orientation, sample.pose.angular_velocity, horizon);
    auto position = sample.pose.position + sample.linear_velocity * horizon;
    auto velocity = sample.linear_velocity;
    if (!sample.space) {
        const auto origin = Quat::exponential({0, recenter_yaw, 0});
        orientation = (origin * orientation).unit(); position = origin.rotate(position); velocity = origin.rotate(velocity);
    }
    result.qRotation = {orientation.w, orientation.x, orientation.y, orientation.z};
    result.vecPosition[0] = position.x; result.vecPosition[1] = position.y; result.vecPosition[2] = position.z;
    if (result.poseIsValid) {
        result.vecVelocity[0] = velocity.x; result.vecVelocity[1] = velocity.y; result.vecVelocity[2] = velocity.z;
        const auto omega = orientation.rotate(sample.pose.angular_velocity); // OpenVR expects driver-world axes.
        result.vecAngularVelocity[0] = omega.x; result.vecAngularVelocity[1] = omega.y; result.vecAngularVelocity[2] = omega.z;
        const auto offset = sample.timestamp_ns <= now ? -static_cast<double>(age) / 1e9 :
            static_cast<double>(sample.timestamp_ns - now) / 1e9;
        result.poseTimeOffset = offset + horizon;
    }
    return result;
}
vr::EVRInitError VirtualController::Activate(uint32_t index) {
    return boundary("Controller::Activate", [&] {
        const auto c = vr::VRProperties()->TrackedDeviceToPropertyContainer(index);
        auto *p = vr::VRProperties();
        p->SetStringProperty(c, vr::Prop_ModelNumber_String, "PocketVR desktop controller");
        p->SetStringProperty(c, vr::Prop_ManufacturerName_String, "PocketVR");
        p->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "pocketvr");
        p->SetStringProperty(c, vr::Prop_SerialNumber_String, left_ ? "PocketVR-Left" : "PocketVR-Right");
        p->SetStringProperty(c, vr::Prop_ControllerType_String, "vive_controller");
        p->SetStringProperty(c, vr::Prop_RenderModelName_String, "vr_controller_vive_1_5");
        p->SetStringProperty(c, vr::Prop_InputProfilePath_String,
                             "{pocketvr}/input/pocketvr_controller_profile.json");
        p->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32,
                            left_ ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        p->SetInt32Property(c, vr::Prop_Axis0Type_Int32, vr::k_eControllerAxis_TrackPad);
        p->SetInt32Property(c, vr::Prop_Axis1Type_Int32, vr::k_eControllerAxis_Trigger);
        auto *in = vr::VRDriverInput();
        constexpr std::array names{"/input/trigger/click", "/input/grip/click",
            "/input/application_menu/click", "/input/system/click",
            "/input/trackpad/click", "/input/trackpad/touch"};
        for (size_t i = 0; i < names.size(); ++i)
            check(in->CreateBooleanComponent(c, names[i], &handles_[i]));
        check(in->CreateScalarComponent(c, "/input/trigger/value", &handles_[6],
              vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedOneSided));
        check(in->CreateScalarComponent(c, "/input/trackpad/x", &handles_[7],
              vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided));
        check(in->CreateScalarComponent(c, "/input/trackpad/y", &handles_[8],
              vr::VRScalarType_Absolute, vr::VRScalarUnits_NormalizedTwoSided));
        vr::VRInputComponentHandle_t haptic{};
        check(in->CreateHapticComponent(c,"/output/haptic",&haptic));
        haptic_.store(haptic); container_.store(c);
        index_ = index;
        return vr::VRInitError_None;
    }, vr::VRInitError_Driver_Failed);
}
void VirtualController::Deactivate() { index_ = vr::k_unTrackedDeviceIndexInvalid; haptic_.store(0); container_.store(0); }
std::optional<HapticPulse> VirtualController::haptic(const vr::VREvent_HapticVibration_t &event) const {
    if (!haptic_.load()||event.componentHandle!=haptic_.load()||event.containerHandle!=container_.load()) return {};
    return bounded_haptic(left_?0:1,event.fDurationSeconds,event.fAmplitude,event.fFrequency);
}
vr::DriverPose_t VirtualController::GetPose() {
    std::lock_guard lock(pose_mutex_);
    return pose_;
}
void VirtualController::update(const vr::DriverPose_t &head, const DesktopInput &input,
                               const PoseSnapshot &external, bool externally_owned,
                               double prediction_ms, double recenter_yaw) {
    const auto next = externally_owned ? tracked_controller_pose(head, external, monotonic_ns(), prediction_ms, recenter_yaw) :
        controller_pose(head, left_, input.gamepad, input.active);
    const auto candidate = input.hands[left_ ? 0 : 1];
    const bool allow_buttons = input_armer_.allow(externally_owned, next.poseIsValid, candidate);
    {
        std::lock_guard lock(pose_mutex_);
        pose_ = next;
    }
    const auto index = index_.load();
    if (index == vr::k_unTrackedDeviceIndexInvalid) return;
    const auto h = input.active && allow_buttons ? candidate : HandInput{};
    const std::array buttons{h.trigger >= 0.5F, h.grip, h.menu, h.system, h.pad,
                            h.pad || h.x != 0 || h.y != 0};
    auto *in = vr::VRDriverInput();
    for (size_t i = 0; i < buttons.size(); ++i)
        check(in->UpdateBooleanComponent(handles_[i], buttons[i], 0));
    check(in->UpdateScalarComponent(handles_[6], h.trigger, 0));
    check(in->UpdateScalarComponent(handles_[7], h.x, 0));
    check(in->UpdateScalarComponent(handles_[8], h.y, 0));
    vr::VRServerDriverHost()->TrackedDevicePoseUpdated(index, next, sizeof(next));
}
} // namespace pocketvr::driver
