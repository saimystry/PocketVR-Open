// SPDX-License-Identifier: Apache-2.0
#include "input.hpp"
#include <pocketvr/math.hpp>
#include "../../control/gamepad.hpp"
#include "../../control/rumble.hpp"
#include "state.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <thread>

using namespace pocketvr;
using namespace pocketvr::driver;
using namespace pocketvr::desktop;
namespace {
TEST(TrackedControllerPose, IndependentPositionAndWorldVelocitiesRespectExplicitCoordinateSpace) {
    vr::DriverPose_t head{}; head.deviceIsConnected = head.poseIsValid = true;
    head.qRotation.w = head.qWorldFromDriverRotation.w = head.qDriverFromHeadRotation.w = 1;
    head.vecPosition[0] = 10; head.vecPosition[1] = 1.65;
    const uint64_t now = 1'000'000'000;
    const auto q = Quat::exponential({0, 0.5, 0});
    PoseSnapshot sample{{{1, 1.3, -2}, q, {0.2, 0, 0}}, now, 2, {1, 0, 0}, 0};
    const auto origin = Quat::exponential({0, 0.4, 0});
    const auto pose = tracked_controller_pose(head, sample, now, 0, 0.4);
    EXPECT_TRUE(pose.poseIsValid);
    const auto position = origin.rotate(sample.pose.position), velocity = origin.rotate(sample.linear_velocity);
    EXPECT_NEAR(pose.vecPosition[0], position.x, 1e-10);
    EXPECT_NEAR(pose.vecPosition[2], position.z, 1e-10);
    EXPECT_NEAR(pose.vecVelocity[0], velocity.x, 1e-10);
    const auto omega = (origin * q).rotate(sample.pose.angular_velocity);
    EXPECT_NEAR(pose.vecAngularVelocity[0], omega.x, 1e-10);
    EXPECT_NEAR(pose.vecAngularVelocity[2], omega.z, 1e-10);
    sample.space = 1;
    const auto aligned = tracked_controller_pose(head, sample, now, 0, 0.4);
    EXPECT_EQ(aligned.vecPosition[0], 1); EXPECT_EQ(aligned.vecPosition[2], -2);
    EXPECT_EQ(aligned.vecVelocity[0], 1);
    EXPECT_NEAR(aligned.qRotation.y, q.y, 1e-10);
    VirtualController controller(true);
    sample.timestamp_ns = monotonic_ns();
    controller.update(head, {}, sample, true, 0, 0);
    EXPECT_TRUE(controller.GetPose().poseIsValid); // Tracking does not require a held button/input lease.
}
TEST(TrackedControllerPose, PredictionIsTimestampedAndStaleTrackingNeverFallsBackToSyntheticHands) {
    vr::DriverPose_t head{}; head.deviceIsConnected = head.poseIsValid = true;
    const uint64_t now = 5'000'000'000;
    PoseSnapshot sample{{{0, 1.3, -0.4}, {}, {0, 1, 0}}, now - 200'000'000, 2, {1, 0, 0}, 1};
    auto pose = tracked_controller_pose(head, sample, now, 40, 0);
    EXPECT_TRUE(pose.poseIsValid);
    EXPECT_NEAR(pose.vecPosition[0], 0.08, 1e-10);
    EXPECT_NEAR(pose.poseTimeOffset, -0.12, 1e-10);
    sample.timestamp_ns = now - 300'000'000;
    pose = tracked_controller_pose(head, sample, now, 40, 0);
    EXPECT_FALSE(pose.poseIsValid); EXPECT_TRUE(pose.deviceIsConnected);
    EXPECT_EQ(pose.vecVelocity[0], 0); EXPECT_EQ(pose.vecAngularVelocity[1], 0);
    sample.timestamp_ns = now - 3'000'000'000;
    EXPECT_FALSE(tracked_controller_pose(head, sample, now, 40, 0).deviceIsConnected);
}
TEST(TrackedControllerPose, HeldActionsRequireNeutralAfterHandoffAndTrackingRecovery) {
    ControllerInputArmer armer;
    HandState held; held.trigger = 1;
    EXPECT_FALSE(armer.allow(true, true, held));
    EXPECT_TRUE(armer.allow(true, true, {}));
    EXPECT_TRUE(armer.allow(true, true, held));
    EXPECT_FALSE(armer.allow(true, false, held));
    EXPECT_FALSE(armer.allow(true, true, held));
    EXPECT_TRUE(armer.allow(true, true, {}));
    EXPECT_TRUE(armer.allow(true, true, held));
    EXPECT_FALSE(armer.allow(false, true, held));
    EXPECT_TRUE(armer.allow(false, true, held));
}
TEST(DesktopRumble, EnvelopeRejectsStaleFutureInvalidAndKeepsIndependentMotorDeadlines) {
    RumbleEnvelope envelope;
    constexpr uint64_t now = 1'000'000'000;
    envelope.submit({{100, 0, 0.5F, 100}, now}, now);
    envelope.submit({{200, 1, 1, 200}, now}, now);
    EXPECT_EQ(envelope.sample(now).left, 32768);
    EXPECT_EQ(envelope.sample(now).right, 65535);
    EXPECT_EQ(envelope.sample(now + 100'000'000).left, 0);
    EXPECT_EQ(envelope.sample(now + 100'000'000).right, 65535);
    envelope.submit({{0, 1, 0, 0}, now + 100'000'000}, now + 100'000'000);
    EXPECT_EQ(envelope.sample(now + 100'000'000).right, 0);
    envelope.clear();
    envelope.submit({{500, 0, 1, 100}, now - 100'000'000}, now);
    envelope.submit({{500, 0, 1, 100}, now + 1}, now);
    envelope.submit({{501, 0, 1, 100}, now}, now);
    envelope.submit({{10, 0, std::numeric_limits<float>::quiet_NaN(), 100}, now}, now);
    EXPECT_EQ(envelope.sample(now), Motors{});
}
TEST(DesktopRumble, IndependentWatchdogStopsDuringRpcStallAndCannotReplayOnRecovery) {
    std::atomic<uint16_t> value{};
    const auto wait = [&](auto predicate) {
        const auto end = monotonic_ns() + 1'000'000'000;
        while (!predicate() && monotonic_ns() < end) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return predicate();
    };
    {
        RumbleOutput output([&](int, Motors m) { value.store(m.left); });
        output.update(0, true, {});
        const std::array pulse{TimedHaptic{{500, 0, 1, 100}, monotonic_ns()}};
        output.update(0, true, pulse);
        ASSERT_TRUE(wait([&] { return value.load() != 0; }));
        ASSERT_TRUE(wait([&] { return value.load() == 0; }));
        output.update(0, true, {}); // The old half-second pulse must not revive.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_EQ(value.load(), 0);
    }
    EXPECT_EQ(value.load(), 0);
}
TEST(DesktopRumble, DeviceChangesPauseAndDestructionStopThePreviousMotors) {
    std::array<std::atomic<uint16_t>, 4> values{};
    const auto wait = [&](auto predicate) {
        const auto end = monotonic_ns() + 1'000'000'000;
        while (!predicate() && monotonic_ns() < end) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return predicate();
    };
    {
        RumbleOutput output([&](int i, Motors m) { values[static_cast<size_t>(i)].store(m.left); });
        output.update(0, true, {});
        std::array pulse{TimedHaptic{{500, 0, 1, 100}, monotonic_ns()}};
        output.update(0, true, pulse);
        ASSERT_TRUE(wait([&] { return values[0].load() != 0; }));
        output.update(1, true, pulse); // No replay on the new device.
        ASSERT_TRUE(wait([&] { return values[0].load() == 0; }));
        EXPECT_EQ(values[1].load(), 0);
        pulse[0].timestamp_ns = monotonic_ns();
        output.update(1, true, pulse);
        ASSERT_TRUE(wait([&] { return values[1].load() != 0; }));
        output.stop();
        ASSERT_TRUE(wait([&] { return values[1].load() == 0; }));
        output.update(1, true, {}); // New ownership handshake after pause.
        pulse[0].timestamp_ns = monotonic_ns();
        output.update(1, true, pulse);
        ASSERT_TRUE(wait([&] { return values[1].load() != 0; }));
    }
    EXPECT_EQ(values[1].load(), 0);
}
TEST(DesktopRumble, MailboxIsBoundedAndDisabledReadersConsumeWithoutReplaying) {
    HapticMailbox mailbox;
    constexpr uint64_t now = 1'000'000'000;
    for (unsigned i = 0; i < 32; ++i) EXPECT_TRUE(mailbox.push({{100, 0, 1, 100}, now}));
    EXPECT_FALSE(mailbox.push({{100, 0, 1, 100}, now}));
    EXPECT_TRUE(mailbox.take(now, false).empty());
    EXPECT_TRUE(mailbox.take(now, true).empty());
    EXPECT_TRUE(mailbox.push({{100, 0, 1, 100}, now - 100'000'000}));
    EXPECT_TRUE(mailbox.push({{100, 0, 1, 100}, now + 1}));
    EXPECT_TRUE(mailbox.push({{100, 1, 1, 100}, now}));
    const auto events = mailbox.take(now, true);
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].pulse.hand, 1);
}
nlohmann::json request(bool active = true, bool gamepad = false) {
    return {{"active", active}, {"gamepad", gamepad},
            {"hands", nlohmann::json::array({hand(), hand()})}};
}
TEST(DesktopInput, LeaseExpiryFocusLossAndSourceSwitchReleaseAllButtons) {
    InputMailbox box;
    auto j = request();
    EXPECT_TRUE(box.submit(j, 100));
    j["hands"][1]["trigger"] = 1;
    EXPECT_TRUE(box.submit(j, 200));
    EXPECT_EQ(box.read(201).hands[1].trigger, 1);
    EXPECT_FALSE(box.read(250'000'201).active);
    EXPECT_FALSE(box.submit(j, 250'000'202)); // held button cannot resume stale lease
    EXPECT_FALSE(box.read(250'000'203).active);
    j["hands"][1]["trigger"] = 0;
    EXPECT_TRUE(box.submit(j, 250'000'204));
    j["active"] = false;
    EXPECT_FALSE(box.submit(j, 250'000'205));
    EXPECT_FALSE(box.read(250'000'206).active);
    j["active"] = true; j["gamepad"] = true; j["hands"][1]["grip"] = true;
    EXPECT_FALSE(box.submit(j, 250'000'207));
    EXPECT_FALSE(box.read(250'000'208).hands[1].grip);
    j["hands"][1]["grip"] = false;
    EXPECT_TRUE(box.submit(j, 250'000'209));
    EXPECT_FALSE(box.read(100).active); // clock rollback never revives stale input
    box.release(); // tracking/session loss also invalidates the lease
    j["hands"][1]["trigger"] = 1;
    EXPECT_FALSE(box.submit(j, 250'000'210));
    EXPECT_FALSE(box.read(250'000'211).active);
}
TEST(DesktopRumble, DriverRoutesOnlyToActiveOwnerAndPauseDiscardsPendingCommands) {
    const auto directory = std::filesystem::temp_directory_path() /
        ("PocketVR-rumble-state-" + std::to_string(monotonic_ns()));
    std::filesystem::create_directory(directory);
    {
        State state(directory / "settings.json");
        state.session_connected.store(true);
        state.haptic_owner.store(1);
        auto input = request(true, true);
        input["method"] = "input.desktop"; input["haptics"] = true;
        ASSERT_TRUE(state.request(input).at("armed").get<bool>());
        state.route_haptic({{100, 0, 0.5F, 100}, monotonic_ns()});
        auto response = state.request(input);
        ASSERT_EQ(response.at("rumble").size(), 1);
        EXPECT_EQ(response.at("rumble")[0].at("hand"), 0);
        EXPECT_TRUE(state.request(input).at("rumble").empty());
        TimedHaptic event;
        EXPECT_FALSE(state.haptics.pop(event));
        state.haptic_owner.store(2);
        state.route_haptic({{100, 1, 1, 100}, monotonic_ns()});
        ASSERT_TRUE(state.haptics.pop(event));
        EXPECT_EQ(event.pulse.hand, 1);
        EXPECT_TRUE(state.request(input).at("rumble").empty());
        state.haptic_owner.store(1);
        state.route_haptic({{100, 0, 1, 100}, monotonic_ns()});
        input["active"] = false;
        EXPECT_TRUE(state.request(input).at("rumble").empty());
        input["active"] = true;
        EXPECT_TRUE(state.request(input).at("rumble").empty());
        state.route_haptic({{100, 0, 1, 100}, monotonic_ns()});
        state.session_connected.store(false);
        EXPECT_TRUE(state.request(input).at("rumble").empty());
    }
    std::filesystem::remove_all(directory);
}
TEST(DesktopInput, RejectsMalformedAndNonfiniteSamplesWithoutRefreshingLease) {
    InputMailbox box;
    auto j = request();
    EXPECT_TRUE(box.submit(j, 100));
    j["hands"][1]["x"] = 1.1;
    EXPECT_THROW(box.submit(j, 200), std::invalid_argument);
    j["hands"][1]["x"] = std::numeric_limits<double>::infinity();
    EXPECT_THROW(box.submit(j, 200), std::invalid_argument);
    j["hands"] = nlohmann::json::array();
    EXPECT_THROW(box.submit(j, 200), std::invalid_argument);
    EXPECT_FALSE(box.read(250'000'101).active);
}
TEST(DesktopInput, XboxMappingsDeadzoneTriggerEndpointsAndUnplugNeutral) {
    auto hands = xbox_hands(0, 30, 0, 1200, -1200, 0, 0);
    EXPECT_EQ(hands[0], hand());
    EXPECT_EQ(hands[1], hand());
    hands = xbox_hands(0x1000 | 0x0200 | 0x8000, 255, 255, -32768, 32767, 32767, -32768);
    EXPECT_EQ(hands[0]["trigger"], 1);
    EXPECT_EQ(hands[0]["x"], -1);
    EXPECT_EQ(hands[0]["y"], 1);
    EXPECT_EQ(hands[1]["trigger"], 1);
    EXPECT_EQ(hands[1]["system"], true);
    EXPECT_EQ(hands[1]["grip"], true);
    EXPECT_EQ(hands[1]["y"], -1);
    EXPECT_EQ(joystick_axis(10, 10, 10), 0);
    EXPECT_EQ(joystick_axis(0, 0, 65535), -1);
    EXPECT_EQ(joystick_axis(65535, 0, 65535), 1);
    EXPECT_EQ(axis_deadzone(std::numeric_limits<float>::quiet_NaN()), 0);
    InputMailbox box;
    auto j = request(true, true);
    box.submit(j, 1);
    j["hands"] = hands;
    box.submit(j, 2);
    EXPECT_TRUE(box.read(3).hands[1].grip);
    j = request(false, true); // disconnected gamepad explicitly releases everything
    box.submit(j, 4);
    EXPECT_FALSE(box.read(5).active);
    EXPECT_EQ(box.read(5).hands[1].trigger, 0);
}
TEST(DesktopInput, GazeRayFollowsHeadAndInvalidTrackingDisablesControllers) {
    vr::DriverPose_t h{};
    h.qRotation.w = 1;
    h.deviceIsConnected = h.poseIsValid = true;
    h.vecPosition[1] = 1.65;
    auto p = controller_pose(h, false, false, true);
    EXPECT_DOUBLE_EQ(p.vecPosition[0], 0);
    EXPECT_DOUBLE_EQ(p.vecPosition[1], 1.65);
    EXPECT_DOUBLE_EQ(p.vecPosition[2], -0.1);
    h.qRotation = {std::cos(pi / 4), 0, std::sin(pi / 4), 0};
    p = controller_pose(h, false, false, true);
    EXPECT_NEAR(p.vecPosition[0], -0.1, 1e-8);
    EXPECT_NEAR(p.vecPosition[2], 0, 1e-8);
    EXPECT_FALSE(controller_pose(h, false, false, false).deviceIsConnected);
    h.poseIsValid = false;
    EXPECT_FALSE(controller_pose(h, false, true, true).poseIsValid);
}
TEST(DesktopInput, SwitchingBetweenDesktopAndPhoneRequiresNeutralAfterHandoff) {
    InputMailbox desktop,phone; InputArbiter arbiter;
    auto neutral=request(); ASSERT_TRUE(phone.submit(neutral,1));
    EXPECT_TRUE(arbiter.read(desktop,phone,2).active);
    auto held=neutral; held["hands"][1]["trigger"]=1;
    ASSERT_TRUE(phone.submit(held,3));
    ASSERT_TRUE(desktop.submit(neutral,4));
    EXPECT_FALSE(arbiter.read(desktop,phone,5).active);
    ASSERT_FALSE(desktop.submit(held,6)); // New owner cannot inherit a held control.
    ASSERT_TRUE(desktop.submit(neutral,7));
    EXPECT_TRUE(arbiter.read(desktop,phone,8).active);
    desktop.release();
    EXPECT_FALSE(arbiter.read(desktop,phone,9).active);
    EXPECT_FALSE(phone.submit(held,10));
    EXPECT_TRUE(phone.submit(neutral,11));
    EXPECT_TRUE(arbiter.read(desktop,phone,12).active);
}
TEST(DesktopInput, MouseAndXboxMergeWithoutLosingAnalogOrHeldControllerButtons) {
    const auto pad = xbox_hands(0x0200, 160, 100, 22000, -20000, 16000, 0);
    auto snapshot = desktop_snapshot(true, false, true, false, 1U | 4U | 8U, pad);
    EXPECT_TRUE(snapshot.active);
    EXPECT_TRUE(snapshot.gamepad);
    EXPECT_EQ(snapshot.hands[0], pad[0]);
    EXPECT_EQ(snapshot.hands[1]["x"], pad[1]["x"]);
    EXPECT_EQ(snapshot.hands[1]["trigger"], 1);
    EXPECT_TRUE(snapshot.hands[1]["grip"].get<bool>());
    EXPECT_TRUE(snapshot.hands[1]["menu"].get<bool>());
    EXPECT_TRUE(snapshot.hands[1]["system"].get<bool>());
    snapshot = desktop_snapshot(true, false, true, false, 0, pad);
    EXPECT_EQ(snapshot.hands, pad); // Releasing the mouse cannot release RB/RT.
}
TEST(DesktopInput, BackgroundPermissionNeverCapturesMouseOrEnablesDisconnectedPad) {
    auto pad = xbox_hands(0x1000, 0, 0, 0, 0, 0, 0);
    auto snapshot = desktop_snapshot(false, false, true, false, 15, pad);
    EXPECT_FALSE(snapshot.active);
    EXPECT_EQ(snapshot.hands, request()["hands"]);
    snapshot = desktop_snapshot(false, false, true, true, 15, pad);
    EXPECT_TRUE(snapshot.active);
    EXPECT_EQ(snapshot.hands, pad); // No background mouse/menu/dashboard bits.
    snapshot = desktop_snapshot(false, false, false, true, 15, pad);
    EXPECT_FALSE(snapshot.active);
    EXPECT_FALSE(snapshot.gamepad);
    EXPECT_EQ(snapshot.hands, request()["hands"]);
    snapshot = desktop_snapshot(true, false, false, false, 2, pad);
    EXPECT_TRUE(snapshot.active); // An unplugged controller leaves local mouse usable.
    EXPECT_FALSE(snapshot.gamepad);
    EXPECT_EQ(snapshot.hands[1], hand(0, 0, 0, true));
}
TEST(DesktopInput, PauseAndReconnectRequireNeutralBeforeHeldButtonsCanResume) {
    InputMailbox box;
    const auto neutral = xbox_hands(0, 0, 0, 0, 0, 0, 0);
    const auto held = xbox_hands(0x1000, 0, 0, 0, 0, 0, 0);
    const auto submit = [&](DesktopSnapshot s, uint64_t now) {
        return box.submit({{"active", s.active}, {"gamepad", s.gamepad}, {"hands", s.hands}}, now);
    };
    EXPECT_TRUE(submit(desktop_snapshot(true, false, true, false, 0, neutral), 1));
    EXPECT_TRUE(submit(desktop_snapshot(true, false, true, false, 0, held), 2));
    EXPECT_FALSE(submit(desktop_snapshot(true, true, true, true, 15, held), 3));
    EXPECT_FALSE(box.read(4).active);
    EXPECT_FALSE(submit(desktop_snapshot(true, false, true, true, 0, held), 5));
    EXPECT_TRUE(submit(desktop_snapshot(true, false, true, true, 0, neutral), 6));
    EXPECT_TRUE(submit(desktop_snapshot(true, false, true, true, 0, held), 7));
    EXPECT_FALSE(submit(desktop_snapshot(true, false, false, true, 1, neutral), 8));
    EXPECT_FALSE(box.read(9).active); // Held mouse cannot inherit the controller lease.
    EXPECT_TRUE(submit(desktop_snapshot(true, false, false, true, 0, neutral), 10));
    EXPECT_FALSE(submit(desktop_snapshot(true, false, true, true, 0, held), 11));
    EXPECT_FALSE(box.read(12).active); // A reconnected held pad must also go neutral.
    EXPECT_TRUE(submit(desktop_snapshot(true, false, true, true, 0, neutral), 13));
}
}
