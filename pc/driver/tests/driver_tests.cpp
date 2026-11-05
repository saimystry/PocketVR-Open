// SPDX-License-Identifier: Apache-2.0
#include "hmd.hpp"
#include <gtest/gtest.h>
#include <bit>
#include <limits>

using namespace pocketvr;
using namespace pocketvr::driver;
namespace {
class DriverFixture : public testing::Test {
  protected:
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
                                      ("PocketVR-driver-test-" + std::to_string(monotonic_ns()));
    std::unique_ptr<State> state;
    std::unique_ptr<Gpu> gpu;
    std::unique_ptr<Hmd> hmd;
    void SetUp() override {
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        state = std::make_unique<State>(directory / "pocketvr.json");
        gpu = std::make_unique<Gpu>(true);
        hmd = std::make_unique<Hmd>(*state, *gpu);
    }
    void TearDown() override {
        hmd.reset();
        gpu.reset();
        state.reset();
        std::filesystem::remove(directory / "pocketvr.json");
        std::filesystem::remove(directory);
    }
};
TEST_F(DriverFixture, IdentityDistortionAndDisplayContract) {
    EXPECT_FALSE(hmd->IsDisplayOnDesktop());
    EXPECT_FALSE(hmd->IsDisplayRealDisplay());
    uint32_t width{}, height{}, x{}, y{};
    hmd->GetRecommendedRenderTargetSize(&width, &height);
    EXPECT_EQ(width, 1080);
    EXPECT_EQ(height, 1080);
    hmd->GetEyeOutputViewport(vr::Eye_Right, &x, &y, &width, &height);
    EXPECT_EQ(x, 1080);
    EXPECT_EQ(width, 1080);
    auto coordinates = hmd->ComputeDistortion(vr::Eye_Left, 0.2F, 0.7F);
    EXPECT_EQ(coordinates.rfRed[0], 0.2F);
    EXPECT_EQ(coordinates.rfBlue[1], 0.7F);
    vr::HmdVector2_t inverse{};
    EXPECT_TRUE(hmd->ComputeInverseDistortion(&inverse, vr::Eye_Left, 0, 0.2F, 0.7F));
    EXPECT_EQ(inverse.v[1], 0.7F);
    EXPECT_FALSE(hmd->ComputeInverseDistortion(nullptr, vr::Eye_Left, 0, 0, 0));
    float left{}, right{}, top{}, bottom{};
    hmd->GetProjectionRaw(vr::Eye_Left, &left, &right, &top, &bottom);
    const auto render = guarded_projection({1, 1, 1, 1});
    EXPECT_FLOAT_EQ(left, -static_cast<float>(render.left));
    EXPECT_FLOAT_EQ(right, static_cast<float>(render.right));
    EXPECT_FLOAT_EQ(top, -static_cast<float>(render.up));
    EXPECT_FLOAT_EQ(bottom, static_cast<float>(render.down));
    EXPECT_NE(hmd->GetComponent(vr::IVRDisplayComponent_Version), nullptr);
    EXPECT_EQ(hmd->GetComponent("unknown"), nullptr);
}
TEST_F(DriverFixture, PredictedPoseAndTrackingLossHoldLastOrientation) {
    EXPECT_FALSE(hmd->GetPose().deviceIsConnected);
    const auto now = monotonic_ns();
    state->head.publish({{{}, {}, {0, 1, 0}}, now, 1});
    const auto predicted = hmd->GetPose();
    EXPECT_TRUE(predicted.poseIsValid);
    EXPECT_TRUE(predicted.deviceIsConnected);
    EXPECT_GT(predicted.qRotation.y, 0.015);
    EXPECT_LT(predicted.qRotation.y, 0.05);
    EXPECT_DOUBLE_EQ(predicted.vecPosition[1], 1.65);
    EXPECT_NEAR(predicted.poseTimeOffset, 0.04, 1e-9);
    state->head.publish(
        {{{}, Quat::exponential({0, 0.5, 0}), {0, 1, 0}}, monotonic_ns() - 300'000'000, 1});
    const auto stale = hmd->GetPose();
    EXPECT_FALSE(stale.poseIsValid);
    EXPECT_EQ(stale.result, vr::TrackingResult_Running_OutOfRange);
    EXPECT_NEAR(stale.qRotation.y, std::sin(0.25), 1e-8);
    EXPECT_EQ(stale.vecAngularVelocity[1], 0);
}
TEST_F(DriverFixture, TrackerBridgeAndLiveSettings) {
    const auto response = state->request({{"method", "bridge.pose"},
                                          {"orientation", {0, 0, 0, 1}},
                                          {"angular_velocity", {0, 0, 0}}});
    EXPECT_TRUE(response.at("ok"));
    EXPECT_TRUE(hmd->GetPose().poseIsValid);
    state->request({{"method", "settings.set"}, {"key", "video.render_scale"}, {"value", 0.7}});
    uint32_t width{}, height{};
    hmd->GetRecommendedRenderTargetSize(&width, &height);
    EXPECT_EQ(width, 756);
    EXPECT_THROW(state->request({{"method", "bridge.pose"}, {"orientation", {0, 0, 0, 0}}}),
                 std::invalid_argument);
    EXPECT_THROW(
        state->request({{"method", "settings.set"}, {"key", "tracking.arcore"}, {"value", true}}),
        std::invalid_argument);
}
TEST_F(DriverFixture, ControllerBridgeHasIndependentOwnershipValidationAndExplicitRelease) {
    const auto head_before = state->head.read().timestamp_ns;
    const auto timestamp = monotonic_ns();
    auto left = nlohmann::json{{"method", "bridge.pose"}, {"device", 1}, {"timestamp_pc_ns", timestamp},
        {"orientation", {0, 0, 0, 1}}, {"position", {-0.2, 1.3, -0.4}}, {"linear_velocity", {0.1, 0, 0}}};
    ASSERT_TRUE(state->request(left).at("ok").get<bool>());
    auto right = left; right["device"] = 2; right["position"] = {0.2, 1.3, -0.4}; right["space"] = "standing";
    ASSERT_TRUE(state->request(right).at("ok").get<bool>());
    EXPECT_EQ(state->head.read().timestamp_ns, head_before);
    EXPECT_TRUE(state->controller_override[0].load()); EXPECT_TRUE(state->controller_override[1].load());
    EXPECT_EQ(state->controller_tracking[0].read().linear_velocity.x, 0.1);
    EXPECT_EQ(state->controller_tracking[1].read().space, 1);
    EXPECT_THROW(state->request(left), std::invalid_argument); // Replay/older timestamp.
    auto bad = left; bad["timestamp_pc_ns"] = monotonic_ns(); bad["orientation"] = {0, 0, 0, 0};
    EXPECT_THROW(state->request(bad), std::invalid_argument);
    bad = left; bad["timestamp_pc_ns"] = monotonic_ns(); bad["linear_velocity"] = {101, 0, 0};
    EXPECT_THROW(state->request(bad), std::invalid_argument);
    bad = left; bad["timestamp_pc_ns"] = monotonic_ns(); bad["space"] = "standing";
    EXPECT_THROW(state->request(bad), std::invalid_argument);
    bad = left; bad["device"] = 3;
    EXPECT_THROW(state->request(bad), std::invalid_argument);
    bad = left; bad["timestamp_pc_ns"] = monotonic_ns() - 2'000'000'000;
    EXPECT_THROW(state->request(bad), std::invalid_argument);
    const auto status = state->request({{"method", "bridge.status"}});
    EXPECT_TRUE(status.at("controllers")[0].at("owned").get<bool>());
    EXPECT_EQ(status.at("controllers")[1].at("space"), "standing");
    state->request({{"method", "bridge.release"}, {"device", 1}});
    EXPECT_FALSE(state->controller_override[0].load()); EXPECT_TRUE(state->controller_override[1].load());
    EXPECT_EQ(state->controller_tracking[0].read().timestamp_ns, 0);
}
TEST_F(DriverFixture, PhoneMappingPersistsAtomicallyAndChangesRequireNeutral) {
    auto mapping=state->settings_copy().values().at("input"); mapping.erase("recenter_hotkey");
    mapping["mapping"]="custom"; mapping["bind_touch_right"]="left_grip";
    RawInput input; input.active=true;
    state->phone_controls(input,1);
    input.buttons=1U<<13; state->phone_controls(input,2);
    ASSERT_EQ(state->phone_input.read(3).hands[1].trigger,1);
    state->request({{"method","input.mapping.set"},{"values",mapping}});
    EXPECT_FALSE(state->phone_input.read(4).active);
    state->phone_controls(input,5); EXPECT_FALSE(state->phone_input.read(6).active);
    input.buttons=0; state->phone_controls(input,7);
    input.buttons=1U<<13; state->phone_controls(input,8);
    EXPECT_TRUE(state->phone_input.read(9).hands[0].grip);
    const auto accepted=state->settings_copy().values();
    mapping["bind_a"]="unknown";
    EXPECT_THROW(state->request({{"method","input.mapping.set"},{"values",mapping}}),std::exception);
    EXPECT_EQ(state->settings_copy().values(),accepted);
    State restored(directory/"pocketvr.json");
    EXPECT_EQ(restored.settings_copy().values(),accepted);
    auto legacy=accepted;
    for (const auto name:input_buttons) legacy["input"].erase("bind_"+std::string(name));
    EXPECT_NO_THROW((void)Settings(legacy));
}
TEST_F(DriverFixture, VirtualEyeHeightStaysAboveFloorAndPreservesPositionedTracking) {
    state->head.publish({{{}, Quat::exponential({0, 0, 0.5}), {}}, monotonic_ns(), 1});
    EXPECT_DOUBLE_EQ(hmd->GetPose().vecPosition[1], 1.65);
    state->request({{"method", "settings.set"}, {"key", "tracking.eye_height_m"}, {"value", 1.2}});
    EXPECT_DOUBLE_EQ(hmd->GetPose().vecPosition[1], 1.2);
    EXPECT_THROW(state->request({{"method", "settings.set"}, {"key", "tracking.eye_height_m"}, {"value", 0}}), std::invalid_argument);
    state->head.publish({{{0, -0.25, 0}, {}, {}}, monotonic_ns(), 2});
    EXPECT_DOUBLE_EQ(hmd->GetPose().vecPosition[1], -0.25);
}
TEST_F(DriverFixture, ViewerCalibrationPersistsAtomicallyAndSeparatesLensSpacingFromIpd) {
    auto profile = state->settings_copy().viewer_values();
    profile["screen_width_m"] = 0.140;
    profile["screen_height_m"] = 0.060;
    profile["inter_lens_distance_m"] = 0.064;
    profile["screen_to_lens_m"] = 0.040;
    profile["ipd_mm"] = 70;
    profile["k1"] = profile["k2"] = 0;
    const auto original_id = state->viewer_geometry()->id;
    const auto applied = state->request({{"method", "viewer.set"}, {"values", profile}});
    EXPECT_TRUE(applied.at("ok"));
    EXPECT_GT(state->viewer_geometry()->id, original_id);
    float left{}, right{}, top{}, bottom{};
    hmd->GetProjectionRaw(vr::Eye_Left, &left, &right, &top, &bottom);
    const auto render = guarded_projection({0.95, 0.8, 0.75, 0.75});
    EXPECT_NEAR(left, -render.left, 1e-6);
    EXPECT_NEAR(right, render.right, 1e-6);
    EXPECT_NEAR(top, -render.up, 1e-6);
    EXPECT_NEAR(state->viewer_geometry()->ipd_m, 0.07, 1e-12);
    profile["ipd_mm"] = 75;
    state->request({{"method", "viewer.set"}, {"values", profile}});
    EXPECT_NEAR(state->viewer_geometry()->eyes[0].left, 0.95, 1e-12);
    EXPECT_NEAR(state->viewer_geometry()->ipd_m, 0.075, 1e-12);
    const auto before = state->settings_copy().values();
    const auto id = state->viewer_geometry()->id;
    auto bad = profile;
    bad["inter_lens_distance_m"] = 1;
    EXPECT_THROW(state->request({{"method", "viewer.set"}, {"values", bad}}),
                 std::invalid_argument);
    EXPECT_EQ(state->settings_copy().values(), before);
    EXPECT_EQ(state->viewer_geometry()->id, id);
    State reloaded(directory / "pocketvr.json");
    EXPECT_EQ(reloaded.settings_copy().values(), before);
    EXPECT_EQ(reloaded.settings_copy().get<std::string>("lens.profile"), "manual");
}
TEST(Settings, ViewerMigrationPreservesExistingSettingsAndRejectsPartialEdits) {
    Settings original;
    auto legacy = original.values();
    legacy["video"]["bitrate_mbps"] = 42;
    for (const auto *key : {"inter_lens_distance_m", "screen_width_m", "screen_height_m"})
        legacy["lens"].erase(key);
    Settings migrated(legacy);
    EXPECT_EQ(migrated.get<int>("video.bitrate_mbps"), 42);
    EXPECT_DOUBLE_EQ(migrated.get<double>("lens.screen_width_m"), 0.134);
    legacy["lens"].erase("k1");
    EXPECT_THROW(Settings{legacy}, std::invalid_argument);
    const auto before = migrated.values();
    auto partial = migrated.viewer_values();
    partial.erase("ipd_mm");
    EXPECT_THROW(migrated.set_viewer(partial), std::invalid_argument);
    EXPECT_EQ(migrated.values(), before);
    auto invalid = migrated.viewer_values();
    invalid["k1"] = std::numeric_limits<double>::infinity();
    EXPECT_THROW(migrated.set_viewer(invalid), std::invalid_argument);
    EXPECT_EQ(migrated.values(), before);
}
TEST(DriverPoseState, ConcurrentSnapshotNeverMixesSamples) {
    PoseState state;
    std::atomic<bool> done{};
    std::thread writer([&] {
        for (int i = 1; i < 20000; ++i) {
            const auto v = static_cast<double>(i);
            state.publish({{{v, -v, v * 2}, {}, {}}, static_cast<uint64_t>(i), 1});
        }
        done.store(true);
    });
    while (!done.load()) {
        const auto sample = state.read();
        if (sample.timestamp_ns) {
            EXPECT_EQ(sample.pose.position.x, static_cast<double>(sample.timestamp_ns));
            EXPECT_EQ(sample.pose.position.y, -sample.pose.position.x);
            EXPECT_EQ(sample.pose.position.z, 2 * sample.pose.position.x);
        }
    }
    writer.join();
}
TEST(DriverFactory, ExportAndInterfaceVersionNegotiation) {
    HMODULE module = LoadLibraryA(POCKETVR_TEST_DRIVER_DLL);
    ASSERT_NE(module, nullptr);
    const std::shared_ptr<void> guard(
        module, [](void *handle) { FreeLibrary(static_cast<HMODULE>(handle)); });
    using Factory = void *(*)(const char *, int *);
    const auto symbol = GetProcAddress(module, "HmdDriverFactory");
    ASSERT_NE(symbol, nullptr);
    const auto factory = std::bit_cast<Factory>(symbol);
    int error = -1;
    EXPECT_EQ(factory("invalid", &error), nullptr);
    EXPECT_EQ(error, vr::VRInitError_Init_InterfaceNotFound);
    auto *provider = static_cast<vr::IServerTrackedDeviceProvider *>(
        factory(vr::IServerTrackedDeviceProvider_Version, &error));
    ASSERT_NE(provider, nullptr);
    EXPECT_EQ(error, vr::VRInitError_None);
    EXPECT_NE(provider->GetInterfaceVersions(), nullptr);
    EXPECT_FALSE(provider->ShouldBlockStandbyMode());
}
TEST(Settings, SchemaValidationAndTransactionalChanges) {
    Settings settings;
    EXPECT_EQ(settings.get<unsigned>("video.fps"), 60U);
    settings.set("video.fps", 45);
    EXPECT_EQ(settings.get<unsigned>("video.fps"), 45U);
    EXPECT_THROW(settings.set("video.fps", 144), std::invalid_argument);
    EXPECT_EQ(settings.get<unsigned>("video.fps"), 45U);
    EXPECT_THROW(settings.set("security.encrypt_media", "false"), std::invalid_argument);
    EXPECT_THROW(settings.set("video.min_bitrate_mbps", 130), std::invalid_argument);
    EXPECT_THROW(settings.set("tracking.arcore", false), std::invalid_argument);
    EXPECT_THROW(settings.set("display.ipd_mm", 100), std::invalid_argument);
    EXPECT_THROW(settings.set("video.render_scale", std::numeric_limits<double>::infinity()),
                 std::invalid_argument);
}
TEST(Settings, ReservedKeysAreNeverRuntimeEditable) {
    // Reserved settings describe features that are not finished. They must never
    // be offered as runtime controls, and an edit must be refused outright
    // instead of being accepted and silently ignored.
    EXPECT_FALSE(Settings::runtime_supported("connection.mode"));
    EXPECT_TRUE(Settings().values().contains("video"));
    EXPECT_FALSE(Settings::runtime_supported("input.recenter_hotkey"));
    EXPECT_TRUE(Settings::runtime_supported("video.fps"));
    EXPECT_TRUE(Settings::runtime_supported("video.render_scale"));
}
TEST_F(DriverFixture, ReservedSettingsAreRejectedAtRuntime) {
    const auto reject = [](State &driver, const char *key, const nlohmann::json &value) {
        try {
            driver.request({{"method", "settings.set"}, {"key", key}, {"value", value}});
        } catch (const std::invalid_argument &error) {
            return std::string(error.what());
        }
        return std::string();
    };
    const auto before = state->settings_copy().values();
    const auto mode = reject(*state, "connection.mode", "usb_adb");
    EXPECT_FALSE(mode.empty()) << "driver accepted a runtime edit of a reserved setting";
    EXPECT_NE(mode.find("reserved"), std::string::npos) << mode;
    const auto hotkey = reject(*state, "input.recenter_hotkey", "Ctrl+Alt+R");
    EXPECT_FALSE(hotkey.empty()) << "driver accepted a runtime edit of a reserved setting";
    EXPECT_NE(hotkey.find("reserved"), std::string::npos) << hotkey;
    EXPECT_EQ(state->settings_copy().values(), before);
    // A supported key still applies.
    const auto applied = state->request(
        {{"method", "settings.set"}, {"key", "video.fps"}, {"value", 45}});
    EXPECT_TRUE(applied.at("ok").get<bool>());
    EXPECT_EQ(state->settings_copy().get<unsigned>("video.fps"), 45U);
}
} // namespace
