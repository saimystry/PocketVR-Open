// SPDX-License-Identifier: Apache-2.0
#include "hmd.hpp"
#include "boundary.hpp"
#include "compositor.hpp"
#include <algorithm>
#include <cstring>

namespace pocketvr::driver {
Hmd::Hmd(State &state, Gpu &gpu)
    : state_(state), gpu_(gpu), compositor_(std::make_unique<Compositor>(gpu, state)) {}
Hmd::~Hmd() = default;
vr::EVRInitError Hmd::Activate(uint32_t index) {
    return boundary(
        "Activate",
        [&] {
            index_.store(index);
            auto *properties = vr::VRProperties();
            const auto container = properties->TrackedDeviceToPropertyContainer(index);
            properties->SetStringProperty(container, vr::Prop_ModelNumber_String,
                                          "PocketVR Android HMD");
            properties->SetStringProperty(container, vr::Prop_SerialNumber_String, "PocketVR-HMD");
            properties->SetStringProperty(container, vr::Prop_ManufacturerName_String, "PocketVR");
            properties->SetStringProperty(container, vr::Prop_TrackingSystemName_String,
                                          "pocketvr");
            // Without a universe SteamVR has no room data for this headset, and OpenXR games
            // that use it can receive an invalid reference space.
            properties->SetUint64Property(container, vr::Prop_CurrentUniverseId_Uint64, 2);
            properties->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, 60);
            properties->SetFloatProperty(container, vr::Prop_SecondsFromVsyncToPhotons_Float,
                                         0.011F);
            properties->SetBoolProperty(container, vr::Prop_DisplayDebugMode_Bool, false);
            properties->SetBoolProperty(container, vr::Prop_IsOnDesktop_Bool, false);
            properties->SetBoolProperty(container, vr::Prop_ContainsProximitySensor_Bool, false);
            properties->SetBoolProperty(container, vr::Prop_HasDisplayComponent_Bool, true);
            properties->SetBoolProperty(container, vr::Prop_HasDriverDirectModeComponent_Bool,
                                        true);
            properties->SetBoolProperty(container, vr::Prop_DriverDirectModeSendsVsyncEvents_Bool,
                                        true);
            properties->SetUint64Property(container, vr::Prop_GraphicsAdapterLuid_Uint64,
                                          gpu_.luid());
            update_properties();
            return vr::VRInitError_None;
        },
        vr::VRInitError_Driver_Failed);
}
void Hmd::Deactivate() {
    index_.store(vr::k_unTrackedDeviceIndexInvalid);
}
void Hmd::EnterStandby() {
    state_.standby.store(true);
}
void *Hmd::GetComponent(const char *version) {
    return boundary(
        "GetComponent",
        [&]() -> void * {
            if (version && std::strcmp(version, vr::IVRDisplayComponent_Version) == 0)
                return static_cast<vr::IVRDisplayComponent *>(this);
            if (version && std::strcmp(version, vr::IVRDriverDirectModeComponent_Version) == 0)
                return static_cast<vr::IVRDriverDirectModeComponent *>(compositor_.get());
            return nullptr;
        },
        static_cast<void *>(nullptr));
}
void Hmd::DebugRequest(const char *, char *response, uint32_t capacity) {
    boundary("DebugRequest", [&] {
        if (!response || capacity == 0)
            return;
        const auto text = state_.request({{"method", "status"}}).dump();
        const auto count = std::min<size_t>(text.size(), capacity - 1);
        std::memcpy(response, text.data(), count);
        response[count] = '\0';
    });
}
vr::DriverPose_t Hmd::GetPose() {
    return boundary(
        "GetPose",
        [&] {
            vr::DriverPose_t result{};
            result.qWorldFromDriverRotation.w = 1;
            result.qDriverFromHeadRotation.w = 1;
            result.qRotation.w = 1;
            result.willDriftInYaw = true;
            result.shouldApplyHeadModel = false;
            const auto sample = state_.head.read();
            const auto now = monotonic_ns();
            if (!sample.timestamp_ns) {
                result.result = vr::TrackingResult_Uninitialized;
                return result;
            }
            const auto age = now >= sample.timestamp_ns ? now - sample.timestamp_ns : 0;
            const bool fresh = age <= 250'000'000 && sample.tracking != 0;
            const auto prediction = std::clamp(state_.prediction_ms.load(), 0.0, 80.0) * 0.001;
            const auto horizon =
                std::clamp(prediction + static_cast<double>(age) * 1e-9, 0.0, 0.08);
            auto orientation =
                fresh ? predict(sample.pose.orientation, sample.pose.angular_velocity, horizon)
                      : sample.pose.orientation;
            orientation =
                (Quat::exponential({0, state_.recenter_yaw.load(), 0}) * orientation).unit();
            result.qRotation = {orientation.w, orientation.x, orientation.y, orientation.z};
            result.vecPosition[0] = sample.tracking == 2 ? sample.pose.position.x : 0;
            // A 3DoF headset has no measured translation. Keep both eyes above
            // the standing-origin floor: y=0 made the reflective SteamVR floor
            // bisect the headset and appear inverted between the two eyes.
            result.vecPosition[1] = sample.tracking == 2 ? sample.pose.position.y : state_.eye_height_m.load();
            result.vecPosition[2] = sample.tracking == 2 ? sample.pose.position.z : 0;
            if (fresh) {
                const auto world_omega = orientation.rotate(sample.pose.angular_velocity);
                result.vecAngularVelocity[0] = world_omega.x;
                result.vecAngularVelocity[1] = world_omega.y;
                result.vecAngularVelocity[2] = world_omega.z;
                const auto sample_offset = sample.timestamp_ns <= now
                    ? -static_cast<double>(now - sample.timestamp_ns) * 1e-9
                    : static_cast<double>(sample.timestamp_ns - now) * 1e-9;
                // Timestamp the pose actually returned, including a capped
                // horizon; SteamVR must not predict it from the wrong time.
                result.poseTimeOffset = sample_offset + horizon;
            }
            result.poseIsValid = fresh;
            result.deviceIsConnected = age < 2'000'000'000;
            result.result =
                fresh ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
            return result;
        },
        vr::DriverPose_t{});
}
void Hmd::GetWindowBounds(int32_t *x, int32_t *y, uint32_t *width, uint32_t *height) {
    if (x)
        *x = 0;
    if (y)
        *y = 0;
    if (width)
        *width = 2160;
    if (height)
        *height = 1080;
}
void Hmd::GetRecommendedRenderTargetSize(uint32_t *width, uint32_t *height) {
    const auto size = static_cast<uint32_t>(1080 * state_.render_scale.load());
    if (width)
        *width = size;
    if (height)
        *height = size;
}
void Hmd::GetEyeOutputViewport(vr::EVREye eye, uint32_t *x, uint32_t *y, uint32_t *width,
                               uint32_t *height) {
    if (x)
        *x = eye == vr::Eye_Left ? 0U : 1080U;
    if (y)
        *y = 0;
    if (width)
        *width = 1080;
    if (height)
        *height = 1080;
}
void Hmd::GetProjectionRaw(vr::EVREye eye, float *left, float *right, float *top, float *bottom) {
    const auto geometry = state_.viewer_geometry();
    const auto p = guarded_projection(geometry->eyes[eye == vr::Eye_Right ? 1 : 0]);
    if (left)
        *left = -static_cast<float>(p.left);
    if (right)
        *right = static_cast<float>(p.right);
    if (top)
        *top = -static_cast<float>(p.down);
    if (bottom)
        *bottom = static_cast<float>(p.up);
}
vr::DistortionCoordinates_t Hmd::ComputeDistortion(vr::EVREye, float u, float v) {
    return {{u, v}, {u, v}, {u, v}};
}
bool Hmd::ComputeInverseDistortion(vr::HmdVector2_t *result, vr::EVREye, uint32_t channel, float u,
                                   float v) {
    if (!result || channel > 2 || !std::isfinite(u) || !std::isfinite(v))
        return false;
    result->v[0] = u;
    result->v[1] = v;
    return true;
}
void Hmd::update_properties() {
    boundary("update_properties", [&] {
        const auto id = index();
        if (id == vr::k_unTrackedDeviceIndexInvalid)
            return;
        const auto geometry = state_.viewer_geometry();
        const auto ipd = static_cast<float>(geometry->ipd_m);
        vr::VRProperties()->SetFloatProperty(
            vr::VRProperties()->TrackedDeviceToPropertyContainer(id), vr::Prop_UserIpdMeters_Float,
            ipd);
        vr::HmdMatrix34_t left{{{1, 0, 0, -ipd / 2}, {0, 1, 0, 0}, {0, 0, 1, 0}}},
            right{{{1, 0, 0, ipd / 2}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
        vr::VRServerDriverHost()->SetDisplayEyeToHead(id, left, right);
        const auto rectangle = [](const Projection &p) {
            const auto render = guarded_projection(p);
            return vr::HmdRect2_t{{{-static_cast<float>(render.left), -static_cast<float>(render.down)}},
                                  {{static_cast<float>(render.right), static_cast<float>(render.up)}}};
        };
        vr::VRServerDriverHost()->SetDisplayProjectionRaw(id, rectangle(geometry->eyes[0]),
                                                          rectangle(geometry->eyes[1]));
    });
}
} // namespace pocketvr::driver
