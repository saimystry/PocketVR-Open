// SPDX-License-Identifier: Apache-2.0
#include "model.hpp"
#include "input.hpp"
#include <pocketvr/settings.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <limits>

using namespace pocketvr::dashboard;
namespace {
bool until(const std::function<bool()> &predicate) {
    const auto end = Clock::now() + std::chrono::seconds(8);
    while (Clock::now() < end) { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return false;
}
TEST(DashboardInput, CoordinatesAndUntrustedEventsCannotClickTheWrongControl) {
    Pointer pointer; std::vector<InputAction> actions;
    const auto sink = [&](InputAction a) { actions.push_back(a); };
    vr::VREvent_t event{}; event.eventType = vr::VREvent_MouseMove;
    event.data.mouse = {100, 200, 0, 0}; pointer.event(event, sink);
    ASSERT_EQ(actions.size(), 1); EXPECT_EQ(actions[0].x, 100); EXPECT_EQ(actions[0].y, 700);
    actions.clear(); event.eventType = vr::VREvent_MouseButtonDown;
    event.data.mouse = {100, std::numeric_limits<float>::quiet_NaN(), vr::VRMouseButton_Left, 0};
    pointer.event(event, sink); EXPECT_TRUE(actions.empty());
    event.data.mouse = {100, 200, vr::VRMouseButton_Left, 0}; pointer.event(event, sink);
    ASSERT_EQ(actions.size(), 2); EXPECT_TRUE(actions.back().down);
    actions.clear(); event.eventType = vr::VREvent_MouseButtonUp; event.data.mouse.x = -100;
    pointer.event(event, sink);
    ASSERT_EQ(actions.size(), 2); EXPECT_LT(actions.front().x, -1e20f); EXPECT_FALSE(actions.back().down);
    actions.clear(); event.eventType = vr::VREvent_ScrollDiscrete; event.data.scroll.xdelta = 1000; event.data.scroll.ydelta = -1000;
    pointer.event(event, sink); ASSERT_EQ(actions.size(), 1); EXPECT_EQ(actions[0].x, 10); EXPECT_EQ(actions[0].y, -10);
}
TEST(DashboardInput, FocusLossAndShutdownReleaseEveryHeldButton) {
    Pointer pointer; std::vector<InputAction> actions;
    const auto sink = [&](InputAction a) { actions.push_back(a); };
    vr::VREvent_t event{}; event.eventType = vr::VREvent_MouseButtonDown;
    for (const auto button : {vr::VRMouseButton_Left, vr::VRMouseButton_Right, vr::VRMouseButton_Middle}) {
        event.data.mouse = {100, 200, static_cast<uint32_t>(button), 0}; pointer.event(event, sink);
    }
    actions.clear(); event.eventType = vr::VREvent_OverlayHidden; pointer.event(event, sink);
    ASSERT_EQ(actions.size(), 5);
    for (int i = 0; i < 3; ++i) { EXPECT_EQ(actions[static_cast<size_t>(i)].index, i); EXPECT_FALSE(actions[static_cast<size_t>(i)].down); }
    actions.clear(); event.eventType = vr::VREvent_Quit; pointer.event(event, sink);
    EXPECT_EQ(actions.back().kind, InputAction::quit);
}
TEST(DashboardHistory, FreshCountersAndPhoneSamplesOnlyAndResetOnLoss) {
    Snapshot s; s.received = Clock::now() - std::chrono::seconds(1); s.revision = 1;
    s.data = {{"network", {{"session", {{"sent_bytes", 1000}, {"encoded_frames", 10}, {"dropped_frames", 0},
        {"phone_stats", {{"available", false}}}}}}}};
    History history; history.update(s); EXPECT_TRUE(history.bitrate.empty()); EXPECT_TRUE(history.decode.empty());
    s.received += std::chrono::seconds(1); ++s.revision;
    s.data["network"]["session"]["sent_bytes"] = 7501000;
    s.data["network"]["session"]["encoded_frames"] = 70;
    history.update(s); ASSERT_EQ(history.bitrate.size(), 1); EXPECT_FLOAT_EQ(history.bitrate.back(), 60);
    EXPECT_TRUE(history.decode.empty()); EXPECT_FLOAT_EQ(history.drops.back(), 0);
    for (int i = 0; i < 150; ++i) { s.received = Clock::now(); ++s.revision; s.data["network"]["session"]["sent_bytes"] = 7501001 + i; history.update(s); }
    EXPECT_LE(history.bitrate.size(), 120);
    history.previous = Clock::now() - std::chrono::seconds(10);
    s.received = Clock::now(); ++s.revision; history.update(s);
    EXPECT_TRUE(history.bitrate.empty()); // Reopening a long-hidden panel starts a fresh chart.
    s.received = Clock::now() - std::chrono::seconds(4); history.update(s);
    EXPECT_TRUE(history.bitrate.empty()); EXPECT_TRUE(history.fps.empty());
}
TEST(DashboardClient, AsyncSettingsAndRejectedMutationsAreNeverRetried) {
    const auto name = std::wstring(LR"(\\.\pipe\PocketVR.dashboard.test.)") + std::to_wstring(GetCurrentProcessId());
    pocketvr::Settings settings;
    std::atomic<int> mutations{}, rejected{};
    pocketvr::win::RpcServer server([&](const Json &request) -> Json {
        const auto method = request.at("method").get<std::string>();
        if (method == "settings.set") {
            ++mutations; std::this_thread::sleep_for(std::chrono::milliseconds(300));
            settings.set(request.at("key"), request.at("value")); return {{"ok", true}};
        }
        if (method == "recenter") { ++rejected; return {{"ok", false}, {"error", "No tracking"}}; }
        if (method == "settings.get") return {{"ok", true}, {"values", settings.values()}};
        if (method == "pairing.list") return {{"ok", true}, {"peers", Json::array()}};
        return {{"ok", true}};
    }, name);
    Client client(name);
    ASSERT_TRUE(until([&] { return client.snapshot().online(); }));
    EXPECT_FALSE(client.command({{"method", "bridge.pose"}}));
    const auto started = Clock::now();
    EXPECT_TRUE(client.command({{"method", "settings.set"}, {"key", "video.bitrate_mbps"}, {"value", 75}}));
    EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(200));
    ASSERT_TRUE(until([&] { const auto s = client.snapshot(); return s.online() && s.data.at("settings").at("values").at("video").at("bitrate_mbps") == 75; }));
    EXPECT_EQ(mutations.load(), 1);
    EXPECT_TRUE(client.command({{"method", "recenter"}}));
    ASSERT_TRUE(until([&] { return client.snapshot().result.find("uncertain") != std::string::npos; }));
    const auto revision = client.snapshot().revision;
    ASSERT_TRUE(until([&] { return client.snapshot().revision > revision + 1; }));
    EXPECT_EQ(rejected.load(), 1);
}
TEST(DashboardClient, OfflineCommandsFailAndCancellationDoesNotWaitForFullRpcTimeout) {
    const auto started = Clock::now();
    {
        Client client(std::wstring(LR"(\\.\pipe\PocketVR.dashboard.absent.)") + std::to_wstring(GetCurrentProcessId()));
        EXPECT_FALSE(client.snapshot().online()); EXPECT_FALSE(client.command({{"method", "recenter"}}));
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    EXPECT_LT(Clock::now() - started, std::chrono::milliseconds(600));
}
}
