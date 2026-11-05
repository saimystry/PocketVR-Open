// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/adaptive.hpp>
#include <pocketvr/crypto.hpp>
#include <pocketvr/fec.hpp>
#include <pocketvr/reassembler.hpp>
#include <pocketvr/timing.hpp>
#include <pocketvr/transport.hpp>
#include <pocketvr/tracking.hpp>
#include <pocketvr/video.hpp>
#include <pocketvr/discovery.hpp>
#include <pocketvr/media_queue.hpp>
#include <pocketvr/viewer.hpp>
#include <pocketvr/surface_frames.hpp>
#include <pocketvr/telemetry.hpp>
#include <pocketvr/latency_log.hpp>
#include <pocketvr/profiles.hpp>
#include <pocketvr/settings.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <limits>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <random>
#include <thread>

using namespace pocketvr;
namespace {
TEST(VideoGuard, ThermalLevelsPauseVideoAndRequireFreshSustainedCoolingBeforeRecovery) {
    StreamLoadGuard guard(60);
    EXPECT_EQ(guard.update(1, true, {2, true}, 10, true).fps, 45);
    EXPECT_EQ(guard.update(2, true, {2, false}, 10, true).fps, 45);
    EXPECT_EQ(guard.update(3, true, {3, true}, 10, true).fps, 30);
    EXPECT_EQ(guard.update(4, true, {4, true}, 10, true).fps, 0);
    EXPECT_EQ(guard.update(5000, true, {0, false}, 0, false).fps, 0);
    EXPECT_EQ(guard.update(6000, true, {0, true}, 0, false).fps, 0);
    EXPECT_EQ(guard.update(15999, true, {0, true}, 0, false).fps, 0);
    EXPECT_EQ(guard.update(16000, true, {0, true}, 0, false).fps, 30);
    EXPECT_EQ(guard.update(17000, true, {0, true}, 10, true).fps, 30);
    EXPECT_EQ(guard.update(27000, true, {0, true}, 10, true).fps, 45);
    EXPECT_EQ(guard.update(28000, true, {0, true}, 10, true).fps, 45);
    EXPECT_EQ(guard.update(38000, true, {0, true}, 10, true).fps, 60);
}
TEST(VideoGuard, EncoderOverloadIsSustainedAndMissingMeasurementsCannotInventHeadroom) {
    StreamLoadGuard guard(60);
    EXPECT_EQ(guard.update(1000, true, {-1, false}, 20, true).fps, 60);
    EXPECT_EQ(guard.update(2499, true, {-1, false}, 20, true).fps, 60);
    EXPECT_EQ(guard.update(2500, true, {-1, false}, 20, true).fps, 45);
    EXPECT_EQ(guard.update(2600, true, {-1, false}, 25, true).fps, 45);
    EXPECT_EQ(guard.update(4100, true, {-1, false}, 25, true).fps, 30);
    EXPECT_EQ(guard.update(20'000, true, {-1, false}, 1, false).fps, 30);
    EXPECT_EQ(guard.update(30'000, true, {-1, false}, std::numeric_limits<double>::quiet_NaN(), true).fps, 30);
    EXPECT_EQ(guard.update(31'000, true, {-1, false}, 10, true).fps, 30);
    EXPECT_EQ(guard.update(41'000, true, {-1, false}, 10, true).fps, 45);
    EXPECT_EQ(guard.update(42'000, false, {6, true}, 50, true).fps, 60);
    StreamLoadGuard low(30);
    EXPECT_EQ(low.update(1, true, {2, true}, 1, true).fps, 30);
    EXPECT_THROW(StreamLoadGuard(120), std::invalid_argument);
}
TEST(VideoGuard, CadencePreserves45On60AndPauseDoesNotBuildATimingBacklog) {
    for (const auto limit : {30U, 45U, 60U}) {
        FrameCadence cadence;
        unsigned accepted = 0;
        for (uint64_t i = 0; i < 60; ++i) accepted += cadence.accept(1 + i * 16'666'667, limit) ? 1U : 0U;
        EXPECT_GE(accepted, limit - 1); EXPECT_LE(accepted, limit + 1);
        EXPECT_FALSE(cadence.accept(2'000'000'000, 0));
        EXPECT_FALSE(cadence.accept(20'000'000'000, 0));
        EXPECT_TRUE(cadence.accept(20'000'000'001, 60));
        EXPECT_FALSE(cadence.accept(20'000'000'002, 60));
    }
}
TEST(VideoGuard, ThermalReportsRejectReplayBadLevelsAndStaleSamplesWithoutAdvancingSequence) {
    ThermalInbox inbox;
    EXPECT_FALSE(inbox.fresh(100));
    auto report = nlohmann::json{{"type", "phone.thermal"}, {"seq", 1}, {"status", 2}, {"sample_age_ms", 50}};
    ASSERT_TRUE(inbox.accept(report, 1'000'000'000));
    EXPECT_EQ(inbox.level(), 2); EXPECT_TRUE(inbox.fresh(2'000'000'000));
    EXPECT_FALSE(inbox.accept(report, 2'000'000'000));
    report["seq"] = 2; report["status"] = 7;
    EXPECT_FALSE(inbox.accept(report, 2'000'000'000));
    report["status"] = 0; report["sample_age_ms"] = 1501;
    EXPECT_FALSE(inbox.accept(report, 2'000'000'000));
    EXPECT_EQ(inbox.level(), 2);
    report["sample_age_ms"] = 0;
    EXPECT_TRUE(inbox.accept(report, 2'000'000'000));
    EXPECT_EQ(inbox.level(), 0);
    EXPECT_FALSE(inbox.fresh(6'000'000'000));
    EXPECT_FALSE(inbox.fresh(1));
    EXPECT_EQ(inbox.rejected(), 3);
}
TEST(StreamFec, FreshReassemblyPressureIsBoundedAndUsefulRecoveryPreventsPrematureRetreat) {
    StreamFecController control(10, true);
    FecFeedback f{1, 60, 0, 0, true};
    uint64_t now = 1000;
    EXPECT_FALSE(control.update(now, &f).changed);
    const auto step = [&](uint64_t elapsed, bool loss, bool recovered) {
        now += elapsed; ++f.sequence; f.accepted += 60;
        f.reassembly_dropped += loss ? 1 : 0; f.recovered_groups += recovered ? 1 : 0;
        return control.update(now, &f);
    };
    EXPECT_EQ(step(1000, true, false).ratio, 17);
    EXPECT_EQ(step(500, true, false).ratio, 17); // Bounded change cadence.
    EXPECT_EQ(step(1500, true, false).ratio, 24);
    EXPECT_EQ(step(2000, true, false).ratio, 25);
    EXPECT_FALSE(step(2000, true, false).changed);
    EXPECT_EQ(step(1000, false, true).reason, FecReason::useful);
    EXPECT_EQ(control.ratio(), 25);
    for (unsigned i = 0; i < 10; ++i) EXPECT_EQ(step(1000, false, false).ratio, 25);
    EXPECT_EQ(step(1000, false, false).ratio, 18);
    for (unsigned i = 0; i < 10; ++i) step(1000, false, false);
    EXPECT_EQ(control.ratio(), 10); // User's protection floor survives recovery.
}
TEST(StreamFec, MissingRepeatedIdleResetAndUnsynchronizedFeedbackCannotInventRecoveryOrLoss) {
    StreamFecController control(0, true);
    FecFeedback f{1, 60, 0, 0, true};
    control.update(1000, &f);
    ++f.sequence; f.accepted += 60; ++f.reassembly_dropped;
    EXPECT_EQ(control.update(2000, &f).ratio, 7);
    EXPECT_FALSE(control.update(2500, &f).changed);
    ++f.sequence; ++f.reassembly_dropped; // No accepted video; no useful evidence.
    EXPECT_EQ(control.update(3000, &f).reason, FecReason::idle);
    EXPECT_FALSE(control.update(4000, nullptr).changed);
    ++f.sequence; f.accepted += 60; f.clock_ready = false;
    EXPECT_FALSE(control.update(5000, &f).changed);
    f.clock_ready = true;
    EXPECT_EQ(control.update(6000, &f).reason, FecReason::baseline);
    ++f.sequence; f.accepted = f.reassembly_dropped = f.recovered_groups = 0;
    EXPECT_EQ(control.update(7000, &f).reason, FecReason::baseline);
    EXPECT_EQ(control.ratio(), 7);
    ++f.sequence; f.accepted += 60;
    EXPECT_EQ(control.update(20000, &f).reason, FecReason::baseline); // Stale gap.
    ++f.sequence;
    EXPECT_EQ(control.update(100, &f).reason, FecReason::baseline); // Clock rollback.
}
TEST(StreamFec, FixedAndTcpBypassModesNeverChangeRatio) {
    for (const auto ratio : {0U, 13U, 25U}) {
        StreamFecController fixed(ratio, false);
        FecFeedback f{1, 60, 100, 1, true};
        for (uint64_t t = 1000; t < 10000; t += 1000) {
            ++f.sequence; f.accepted += 60; ++f.reassembly_dropped;
            const auto decision = fixed.update(t, &f);
            EXPECT_EQ(decision.ratio, ratio); EXPECT_FALSE(decision.changed);
            EXPECT_EQ(decision.reason, FecReason::fixed);
        }
    }
    EXPECT_THROW(StreamFecController(26, true), std::invalid_argument);
}
TEST(SensorCalibration, RuntimeBiasSurvivesAllDisplayRotationsAndExplicitInvalidation) {
    GyroBiasCache cache;
    EXPECT_FALSE(cache.get());
    const Vec3 raw{0.004, -0.003, 0.002};
    cache.set(raw);
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        SensorFusion fusion;
        const auto display_bias = sensor_to_display(*cache.get(), rotation);
        fusion.restore_bias(display_bias);
        EXPECT_FALSE(fusion.calibrating());
        const auto recovered = sensor_to_display(fusion.gyro_bias(), (4 - rotation) % 4);
        EXPECT_NEAR((recovered - raw).norm(), 0, 1e-12);
        ASSERT_TRUE(fusion.update({1'000'000'000, display_bias, {0, 9.80665, 0}, {}}));
        ASSERT_TRUE(
            fusion.update({1'005'000'000, display_bias + Vec3{0, 0.01, 0}, {0, 9.80665, 0}, {}}));
        EXPECT_NEAR(fusion.angular_velocity().y, 0.01, 1e-12);
    }
    cache.clear();
    EXPECT_FALSE(cache.get());
    EXPECT_THROW(cache.set({0.2, 0, 0}), std::invalid_argument);
    SensorFusion fusion;
    EXPECT_THROW(fusion.restore_bias({std::numeric_limits<double>::quiet_NaN(), 0, 0}),
                 std::invalid_argument);
    fusion.begin_bias_calibration();
    EXPECT_TRUE(fusion.calibrating());
}
TEST(Tracking, QuietPeriodsFollowGyroDriftButMovementIsNotLearned) {
    SensorFusion fusion;
    uint64_t time = 1'000'000'000;
    ASSERT_TRUE(fusion.update({time, {}, {0, 9.80665, 0}, {}}));
    const auto run = [&](Vec3 gyro, double seconds) {
        for (int i = 0; i < static_cast<int>(seconds * 200); ++i) {
            time += 5'000'000;
            ASSERT_TRUE(fusion.update({time, gyro, {0, 9.80665, 0}, {}}));
        }
    };
    run({0, 0.003, 0}, 120);
    EXPECT_NEAR(fusion.gyro_bias().y, 0.003, 0.0003);
    const auto learned = fusion.gyro_bias().y;
    run({0, 0.2, 0}, 20);  // A deliberate turn.
    run({0, 0.012, 0}, 60); // A very slow deliberate turn.
    EXPECT_NEAR(fusion.gyro_bias().y, learned, 1e-9);
}
TEST(Latency, JoinsBothHalvesOfAFrameAndRejectsImpossibleTimings) {
    LatencyJoin join;
    join.add_pc({7, 1'000'000'000, 1'010'000'000, 1'012'000'000, 125'000, 8.5});
    EXPECT_FALSE(join.add_phone({8, 1'015'000'000, 1'016'000'000, 1'026'000'000, 1'030'000'000}));
    EXPECT_FALSE(join.add_phone({7, 1'016'000'000, 1'015'000'000, 1'026'000'000, 1'030'000'000}));
    const auto frame = join.add_phone({7, 1'015'000'000, 1'016'000'000, 1'026'000'000, 1'030'000'000});
    ASSERT_TRUE(frame);
    EXPECT_DOUBLE_EQ(frame->before_send_ms, 10);
    EXPECT_DOUBLE_EQ(frame->send_ms, 2);
    EXPECT_DOUBLE_EQ(frame->network_ms, 3);
    EXPECT_DOUBLE_EQ(frame->decode_ms, 10);
    EXPECT_DOUBLE_EQ(frame->total_ms, 30);
    const auto summary = join.summary();
    EXPECT_EQ(summary.at("samples"), 1);
    EXPECT_DOUBLE_EQ(summary.at("total").at("p50_ms").get<double>(), 30);
    EXPECT_DOUBLE_EQ(summary.at("average_frame_kilobits").get<double>(), 1000);
    join.add_pc({9, 1'000'000'000, 1'010'000'000, 1'012'000'000, 1, 1});
    const auto skewed = join.add_phone({9, 1'011'000'000, 1'016'000'000, 1'026'000'000, 1'030'000'000});
    ASSERT_TRUE(skewed);
    EXPECT_DOUBLE_EQ(skewed->network_ms, 0);
    EXPECT_NE(latency_csv_row(*frame).find("7,125000,"), std::string::npos);
}
TEST(Profiles, BuiltInProfilesAreValidSettings) {
    for (const auto &[name, values] : built_in_profiles().items()) {
        Settings settings;
        for (const auto &[key, value] : values.items())
            EXPECT_NO_THROW(settings.set(key, value)) << name << " " << key;
    }
    EXPECT_FALSE(valid_profile_name("War Thunder"));
    EXPECT_FALSE(valid_profile_name(""));
    EXPECT_FALSE(valid_profile_name(std::string(41, 'a')));
    EXPECT_TRUE(valid_profile_name("My Shinecon"));
}
TEST(Profiles, TestPictureHasASmoothSkyAndFineDetailInBothEyes) {
    const auto pixels = test_pattern_pixels(400, 100);
    ASSERT_EQ(pixels.size(), 400U * 100 * 4);
    const auto red = [&](uint32_t x, uint32_t y) { return pixels[(y * 400 + x) * 4 + 2]; };
    for (uint32_t y = 1; y < 60; ++y)
        EXPECT_GE(red(10, y), red(10, y - 1));
    EXPECT_EQ(red(10, 30), red(210, 30));
    EXPECT_NE(red(199, 70), red(198, 70)); // One-pixel lines at the fine end.
}
TEST(Menu, ChangesStayInRangeAndClosedMenuDoesNothing) {
    Settings settings;
    auto values = settings.values();
    const auto &schema = Settings::schema();
    VrMenu menu;
    EXPECT_FALSE(menu.input(MenuInput::right, values, schema));
    EXPECT_TRUE(menu.lines(values).empty());
    menu.toggle();
    const auto lines = menu.lines(values);
    ASSERT_EQ(lines.size(), 4U);
    EXPECT_EQ(lines[1], "> EYE SIZE 135%");
    for (const auto &line : lines)
        EXPECT_LE(line.size(), 42U);
    const auto bigger = menu.input(MenuInput::right, values, schema);
    ASSERT_TRUE(bigger);
    EXPECT_EQ(bigger->key, "display.eye_size");
    EXPECT_NEAR(bigger->value, 1.4, 1e-9);
    values["display"]["eye_size"] = 1.6;
    EXPECT_NEAR(menu.input(MenuInput::right, values, schema)->value, 1.6, 1e-9);
    EXPECT_FALSE(menu.input(MenuInput::up, values, schema)); // Wraps to Close.
    EXPECT_EQ(menu.lines(values)[3], "> CLOSE MENU");
    EXPECT_FALSE(menu.input(MenuInput::left, values, schema));
    const auto closed = menu.input(MenuInput::right, values, schema);
    ASSERT_TRUE(closed);
    EXPECT_TRUE(closed->close);
    EXPECT_FALSE(menu.open());
}
TEST(Latency, PacingSpreadsAFrameWithinItsShareOfTheInterval) {
    const uint64_t interval = 16'000'000;
    EXPECT_EQ(paced_send_ns(100, 5, 10, interval, 0), 100);
    EXPECT_EQ(paced_send_ns(100, 0, 10, interval, 50), 100);
    EXPECT_EQ(paced_send_ns(100, 5, 10, interval, 50), 100 + 4'000'000);
    EXPECT_LE(paced_send_ns(100, 99, 100, interval, 60), 100 + interval * 60 / 100);
    EXPECT_EQ(paced_send_ns(100, 50, 0, interval, 60), 100);
    EXPECT_LE(paced_send_ns(100, 500, 10, interval, 60), 100 + interval * 60 / 100);
}
#ifndef __ANDROID__ // The log is PC-only, and Android apps have no shared temp folder.
TEST(Latency, LogSizeLimitHoldsAcrossSessions) {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("pocketvr-latency-" + std::to_string(monotonic_ns()));
    const auto path = directory / "latency.csv";
    EXPECT_EQ(prepare_latency_file(path, 10), 0U); // First run: nothing there yet.
    EXPECT_TRUE(std::filesystem::is_directory(directory));
    std::ofstream(path) << "12345";
    EXPECT_EQ(prepare_latency_file(path, 10), 5U); // A later session carries on counting.
    std::ofstream(path, std::ios::app) << "123456";
    EXPECT_EQ(prepare_latency_file(path, 10), 0U);
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(std::filesystem::file_size(directory / "latency.previous.csv"), 11U);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}
#endif
TEST(Latency, PacingKeepsSmallFramesShortAndLargeFramesInsideTheWindow) {
    const uint64_t interval = 16'000'000;
    const auto window = interval / 2;
    // A tenth of a typical frame takes a tenth of the window.
    EXPECT_EQ(paced_send_ns(0, 10, 10, interval, 50, 100), window / 10);
    EXPECT_EQ(paced_send_ns(0, 100, 100, interval, 50, 100), window);
    // A keyframe five times the usual size must not overrun the window.
    EXPECT_LE(paced_send_ns(0, 500, 500, interval, 50, 100), window);
    EXPECT_EQ(paced_send_ns(100, 10, 10, interval, 0, 100), 100);
}
TEST(KeyframeRequests, BothPathsProduceOneKeyframeButLaterLossStillGetsOne) {
    KeyframeRequestGate gate;
    const uint64_t start = 1'000'000'000;
    EXPECT_TRUE(gate.accept(start));
    EXPECT_FALSE(gate.accept(start + 100'000'000)); // The same request on the control path.
    EXPECT_TRUE(gate.accept(start + 500'000'000)); // The phone's next allowed request.
}
TEST(Protocol, KeyframeRequestIsAValidPacketType) {
    wire::Header header;
    header.type = wire::Type::keyframe;
    header.session = 1;
    header.counter = 1;
    std::array<uint8_t, wire::header_size> bytes{};
    ASSERT_TRUE(wire::encode_header(header, bytes));
    const auto decoded = wire::decode_header(bytes);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->type, wire::Type::keyframe);
}
TEST(UdpWait, WakesWhenAnySocketHasDataAndTimesOutOtherwise) {
    UdpTransport first(Endpoint::parse("127.0.0.1", 0)), second(Endpoint::parse("127.0.0.1", 0));
    UdpTransport sender(Endpoint::parse("127.0.0.1", 0));
    const std::array<UdpTransport *, 2> sockets{&first, &second};
    EXPECT_FALSE(UdpTransport::wait_any(sockets, std::chrono::milliseconds(5)));
    const std::array<uint8_t, 4> message{1, 2, 3, 4};
    ASSERT_TRUE(sender.send(message, second.local_endpoint()));
    EXPECT_TRUE(UdpTransport::wait_any(sockets, std::chrono::milliseconds(1000)));
}
TEST(PoseState, AlternatingInstancesRetainTheirOwnValidFallbackUnderContention) {
    PoseState a, b;
    const PoseSnapshot first{{{}, Quat::exponential({0, 0.2, 0}), {}}, 1, 1, {1, 2, 3}, 1};
    const PoseSnapshot second{{{}, Quat::exponential({0, -0.4, 0}), {}}, 2, 1, {-1, -2, -3}, 0};
    a.publish(first); b.publish(second);
    ASSERT_EQ(a.read().timestamp_ns, 1);
    ASSERT_EQ(b.read().timestamp_ns, 2);
    std::atomic<bool> stop{};
    std::thread writer([&] { while (!stop.load()) { a.publish(first); b.publish(second); } });
    unsigned invalid{};
    for (unsigned i = 0; i < 20000; ++i) {
        const auto x = a.read(), y = b.read();
        if (x.timestamp_ns != 1 || y.timestamp_ns != 2 ||
            std::abs(x.pose.orientation.y - first.pose.orientation.y) > 1e-10 ||
            std::abs(y.pose.orientation.y - second.pose.orientation.y) > 1e-10 ||
            x.linear_velocity.x != 1 || y.linear_velocity.x != -1 || x.space != 1 || y.space != 0) ++invalid;
    }
    stop.store(true); writer.join();
    EXPECT_EQ(invalid, 0);
}
TEST(StreamBitrate, FreshPressureCutsAndActiveCleanRecoveryStaysBounded) {
    StreamBitrateController control(60'000'000, 15'000'000, 120'000'000, true);
    BitrateFeedback f;
    f.clock_ready = true;
    f.sequence = 1;
    EXPECT_FALSE(control.update(1000, &f).changed);
    f.sequence++;
    f.accepted = 55;
    f.decoded = 55;
    f.reassembly_dropped = 5;
    EXPECT_EQ(control.update(2000, &f).bits_per_second, 48'000'000U);
    EXPECT_FALSE(control.update(2100, &f).changed); // repeated report
    for (uint64_t i = 3; i <= 5; ++i) {
        f.sequence++;
        f.accepted += 60;
        f.decoded += 60;
        control.update(i * 1000, &f);
    }
    EXPECT_EQ(control.bitrate(), 50'400'000U);
    for (uint64_t i = 6; i <= 60; ++i) {
        f.sequence++;
        f.accepted += 60;
        f.decoded += 60;
        control.update(i * 1000, &f);
    }
    EXPECT_EQ(control.bitrate(), 120'000'000U);
    for (uint64_t i = 61; i < 90; ++i) {
        f.sequence++;
        f.accepted += 60;
        f.decoded += 60;
        f.missed_vsync += 6;
        control.update(i * 1000, &f);
    }
    EXPECT_EQ(control.bitrate(), 15'000'000U);
}
TEST(StreamBitrate, IsolatedDisplayHiccupsDoNotCutBitrate) {
    StreamBitrateController control(60'000'000, 15'000'000, 120'000'000, true);
    BitrateFeedback f;
    f.sequence = 1;
    f.clock_ready = true;
    control.update(1000, &f);
    for (uint64_t i = 2; i <= 300; ++i) {
        f.sequence++;
        f.accepted += 60;
        f.decoded += 60;
        if (i % 2 == 0)
            f.missed_vsync++;
        if (i % 3 == 0)
            f.decoder_dropped++;
        const auto decision = control.update(i * 1000, &f);
        EXPECT_NE(decision.reason, BitrateReason::pressure);
    }
    EXPECT_EQ(control.bitrate(), 120'000'000U);
}
TEST(StreamBitrate, MissedVsyncWithoutVideoIsNotLinkPressure) {
    StreamBitrateController control(60'000'000, 15'000'000, 120'000'000, true);
    BitrateFeedback f;
    f.sequence = 1;
    f.clock_ready = true;
    control.update(1000, &f);
    f.sequence++;
    f.missed_vsync += 100;
    EXPECT_EQ(control.update(2000, &f).reason, BitrateReason::idle);
    EXPECT_EQ(control.bitrate(), 60'000'000U);
}
TEST(StreamBitrate, FixedIdleMissingResetAndCooldownDoNotInventPressureOrRecovery) {
    StreamBitrateController fixed(10'500'000, 15'000'000, 120'000'000, false);
    BitrateFeedback f;
    f.sequence = 1;
    f.clock_ready = true;
    fixed.update(1000, &f);
    f.sequence++;
    f.missed_vsync = 100;
    EXPECT_EQ(fixed.update(2000, &f).bits_per_second, 10'500'000U);
    StreamBitrateController control(60'000'000, 15'000'000, 120'000'000, true);
    control.update(1000, &f);
    f.sequence++;
    f.accepted += 60;
    f.decoded += 60;
    f.missed_vsync += 6;
    EXPECT_EQ(control.update(2000, &f).bits_per_second, 48'000'000U);
    f.sequence++;
    f.accepted += 60;
    f.decoded += 60;
    f.missed_vsync += 6;
    EXPECT_FALSE(control.update(2200, &f).changed);
    for (uint64_t i = 3; i < 8; ++i) {
        f.sequence++;
        EXPECT_FALSE(control.update(i * 1000, &f).changed); // no video progress
    }
    control.update(8000, nullptr);
    f.sequence++;
    f.missed_vsync += 100;
    EXPECT_FALSE(control.update(9000, &f).changed); // stale gap establishes baseline
    f.sequence++;
    f.missed_vsync = 0;
    EXPECT_FALSE(control.update(10'000, &f).changed); // counter reset
    f.sequence++;
    f.clock_ready = false;
    f.missed_vsync = 100;
    EXPECT_FALSE(control.update(11'000, &f).changed);
    EXPECT_EQ(control.bitrate(), 48'000'000U);
    EXPECT_THROW(StreamBitrateController(60'000'000, 120'000'000, 15'000'000, true),
                 std::invalid_argument);
}
TEST(Telemetry, RollingPercentilesBoundedWindowAndInvalidSamples) {
    RollingLatency<4> latency;
    EXPECT_EQ(latency.summary().samples, 0);
    for (const double sample : {1.0, 2.0, 3.0, 4.0, 100.0})
        latency.add(sample);
    latency.add(-1);
    latency.add(std::numeric_limits<double>::infinity());
    latency.add(std::numeric_limits<double>::quiet_NaN());
    latency.add(60'001);
    const auto result = latency.summary();
    EXPECT_EQ(result.samples, 4);
    EXPECT_EQ(result.p50_ms, 3);
    EXPECT_EQ(result.p95_ms, 100);
    RollingLatency<1> single;
    single.add(7);
    EXPECT_EQ(single.summary().p50_ms, 7);
    EXPECT_EQ(single.summary().p95_ms, 7);
}
TEST(Telemetry, RejectMalformedAndReplayWithoutRefreshingValidSnapshot) {
    PhoneStatsInbox inbox;
    EXPECT_FALSE(inbox.status(1).at("available").get<bool>());
    nlohmann::json report{{"seq", 1}, {"values", empty_phone_stats()}};
    EXPECT_TRUE(inbox.accept(report, 1'000));
    EXPECT_FALSE(inbox.accept(report, 2'000));
    for (const auto &invalid : {nlohmann::json(-1), nlohmann::json(1.5), nlohmann::json("2"),
                                nlohmann::json(UINT64_MAX)}) {
        auto bad = report;
        bad["seq"] = invalid;
        EXPECT_FALSE(inbox.accept(bad, 3'000));
    }
    for (const auto *key : {"display_fps", "decoder_decoded", "clock_ready", "decode_samples"}) {
        auto bad = report;
        bad["seq"] = 2;
        bad["values"][key] = "invalid";
        EXPECT_FALSE(inbox.accept(bad, 3'000));
    }
    auto bad = report;
    bad["seq"] = 2;
    bad["values"]["decode_p50_ms"] = 10;
    EXPECT_FALSE(inbox.accept(bad, 3'000));
    bad["values"] = empty_phone_stats();
    bad["values"]["display_fps"] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(inbox.accept(bad, 3'000));
    bad["values"] = empty_phone_stats();
    bad["values"]["extra"] = 1;
    EXPECT_FALSE(inbox.accept(bad, 3'000));
    bad["values"].erase("extra");
    bad["values"].erase("media_bytes");
    EXPECT_FALSE(inbox.accept(bad, 3'000));
    const auto current = inbox.status(4'000);
    EXPECT_EQ(current.at("seq"), 1);
    EXPECT_EQ(current.at("values"), report.at("values"));
    EXPECT_EQ(current.at("age_ms"), 0.003);
    const auto expired = inbox.status(3'000'001'001);
    EXPECT_FALSE(expired.at("available").get<bool>());
    EXPECT_TRUE(expired.at("values").is_null());
    report["seq"] = 2;
    EXPECT_TRUE(inbox.accept(report, 3'000'002'000));
    EXPECT_TRUE(inbox.status(3'000'002'001).at("available").get<bool>());
    EXPECT_FALSE(inbox.status(1).at("available").get<bool>());
}
TEST(SurfaceFrames, ExactTimestampAndSessionMatchSurvivesTextureSkipping) {
    SurfaceFrames frames;
    wire::VideoHeader header;
    header.frame_id = 10;
    header.render_pose.orientation = Quat::exponential({0, 0.1, 0});
    ASSERT_TRUE(frames.publish({header, 100, 1}));
    header.frame_id = 11;
    header.render_pose.orientation = Quat::exponential({0, 0.2, 0});
    ASSERT_TRUE(frames.publish({header, 200, 1}));
    const auto actual = frames.match(100, 1);
    ASSERT_TRUE(actual);
    EXPECT_EQ(actual->header.frame_id, 10);
    EXPECT_NEAR(actual->header.render_pose.orientation.y, std::sin(0.05), 1e-12);
    EXPECT_EQ(frames.match(200, 1)->header.frame_id, 11);
    EXPECT_FALSE(frames.match(201, 1));
    EXPECT_FALSE(frames.match(200, 2));
    EXPECT_FALSE(frames.match(0, 1));
    for (uint64_t i = 0; i < 64; ++i)
        ASSERT_TRUE(frames.publish({header, 1000 + i, 2}));
    EXPECT_FALSE(frames.publish({header, 5000, 2}));
    EXPECT_TRUE(frames.match(1063, 2));
    EXPECT_FALSE(frames.match(100, 1));
}
TEST(SurfaceFrames, LatePredictionUsesTheDisplayedPoseAndRejectsStaleTracking) {
    wire::VideoHeader header;
    header.render_pose.orientation = Quat::exponential({0, 0.4, 0});
    PoseSnapshot current{{{}, Quat::exponential({0, 0.5, 0}), {0, 1, 0}}, 1'000'000'000, 1};
    const auto rotation = late_sampling_rotation(header, current, 1'010'000'000, true);
    const auto ray = rotation.rotate({0, 0, -1});
    EXPECT_NEAR(ray.x, -std::sin(0.11), 1e-10);
    EXPECT_DOUBLE_EQ(late_sampling_rotation(header, current, 1'010'000'000, false).w, 1);
    EXPECT_DOUBLE_EQ(late_sampling_rotation(header, current, 1'300'000'000, true).w, 1);
    EXPECT_DOUBLE_EQ(late_sampling_rotation(header, current, 999'999'999, true).w, 1);
}
TEST(SurfaceFrames, OriginDiscontinuityAndStaleVideoCannotCollapseTheView) {
    SurfaceFrame frame;
    frame.timestamp_ns = 1'000'000'000;
    frame.header.render_pose.orientation = Quat{};
    PoseSnapshot pose{{{}, Quat::exponential({0, 0.1, 0}), {}}, 1'000'000'000, 1};
    auto rotation = late_surface_rotation(frame, pose, 1'010'000'000, true);
    EXPECT_NEAR(rotation.rotate({0, 0, -1}).x, -std::sin(0.1), 1e-10);
    pose.pose.orientation = Quat::exponential({0, 2.0, 0});
    EXPECT_DOUBLE_EQ(late_surface_rotation(frame, pose, 1'010'000'000, true).w, 1);
    pose.pose.orientation = Quat::exponential({0, 0.1, 0});
    pose.timestamp_ns = 1'290'000'000; // Tracking is fresh, but this texture is old.
    EXPECT_DOUBLE_EQ(late_surface_rotation(frame, pose, 1'300'000'000, true).w, 1);
    EXPECT_DOUBLE_EQ(late_surface_rotation(frame, pose, 999'999'999, true).w, 1);
    frame.timestamp_ns = 1'290'000'000;
    EXPECT_LT(late_surface_rotation(frame, pose, 1'300'000'000, true).w, 1);
}
TEST(Viewer, PhysicalLensCentersProduceMirroredOffAxisProjections) {
    const auto geometry = calibrated_viewer(0.140, 0.060, 0.064, 0.040, {0, 0}, 1);
    const auto &left = geometry.eyes[0];
    const auto &right = geometry.eyes[1];
    EXPECT_NEAR(left.left, 0.95, 1e-12);
    EXPECT_NEAR(left.right, 0.8, 1e-12);
    EXPECT_NEAR(left.up, 0.75, 1e-12);
    EXPECT_DOUBLE_EQ(left.left, right.right);
    EXPECT_DOUBLE_EQ(left.right, right.left);
    EXPECT_NEAR(left.uv({0, 0, -1})[0], 0.95 / 1.75, 1e-12);
    const auto farther = calibrated_viewer(0.140, 0.060, 0.064, 0.080, {0, 0}, 1);
    EXPECT_NEAR(farther.eyes[0].left, left.left / 2, 1e-12);
    const auto wider = calibrated_viewer(0.140, 0.060, 0.068, 0.040, {0, 0}, 1);
    EXPECT_LT(wider.eyes[0].left, left.left);
    EXPECT_GT(wider.eyes[0].right, left.right);
}
TEST(Viewer, GuardPixelsPreserveOpticsAndCoverSmallLateRotations) {
    const Projection visible{1, 1, 1, 1};
    const auto rendered = guarded_projection(visible);
    EXPECT_NEAR(std::atan(rendered.left) - std::atan(visible.left), 8 * pi / 180, 1e-12);
    for (const auto yaw : {-0.10, 0.10})
        for (const auto u : {0.0, 1.0}) {
            const auto ray = Quat::exponential({0, yaw, 0}).rotate(visible.ray(u, 0.5));
            const auto uv = rendered.uv(ray);
            EXPECT_GT(uv[0], 0);
            EXPECT_LT(uv[0], 1);
        }
    const Projection off_axis{0.8, 1.2, 0.9, 1.1};
    const auto guarded = guarded_projection(off_axis);
    EXPECT_LT(guarded.left, guarded.right);
    EXPECT_LT(guarded.up, guarded.down);
    EXPECT_TRUE(guarded_projection({19.9, 19.9, 19.9, 19.9}).valid());
    EXPECT_THROW(guarded_projection({0, 0, 1, 1}), std::invalid_argument);
}
TEST(Viewer, DistortionAndFovRemainFiniteAndRejectImpossibleGeometry) {
    const auto plain = calibrated_viewer(0.134, 0.062, 0.064, 0.039, {0, 0}, 1);
    const auto distorted = calibrated_viewer(0.134, 0.062, 0.064, 0.039, {}, 1);
    EXPECT_GT(distorted.eyes[0].left, plain.eyes[0].left);
    EXPECT_TRUE(distorted.eyes[0].valid());
    EXPECT_THROW(calibrated_viewer(0.06, 0.06, 0.064, 0.039, {}, 1), std::invalid_argument);
    EXPECT_THROW(calibrated_viewer(0.134, 0.062, 0.064, 0, {}, 1), std::invalid_argument);
    EXPECT_THROW(calibrated_viewer(0.2, 0.11, 0.04, 0.02, {2, 2}, 1.3), std::invalid_argument);
    EXPECT_THROW(
        calibrated_viewer(std::numeric_limits<double>::quiet_NaN(), 0.06, 0.064, 0.039, {}, 1),
        std::invalid_argument);
}
TEST(Discovery, NonceBoundedNamesVersionAndMalformedReplies) {
    DiscoveryNonce nonce{};
    nonce[0] = 42;
    const auto query = discovery_query(nonce);
    EXPECT_EQ(parse_discovery_query(query), nonce);
    auto invalid_query = query;
    invalid_query[6] = 1;
    EXPECT_FALSE(parse_discovery_query(invalid_query));
    DiscoveredServer server{9943, 9944, "PC \xf0\x9f\x98\x80", {}};
    const auto bytes = discovery_reply(nonce, server);
    const auto parsed = parse_discovery_reply(bytes, nonce);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->name, server.name);
    EXPECT_EQ(parsed->control_port, 9943);
    for (size_t size = 0; size < bytes.size(); ++size)
        EXPECT_FALSE(parse_discovery_reply(std::span(bytes).first(size), nonce));
    auto other = nonce;
    other[0] ^= 1;
    EXPECT_FALSE(parse_discovery_reply(bytes, other));
    auto invalid_utf8 = bytes;
    invalid_utf8[28] = 0xff;
    EXPECT_FALSE(parse_discovery_reply(invalid_utf8, nonce));
    server.name = std::string(97, 'x');
    EXPECT_THROW(discovery_reply(nonce, server), std::invalid_argument);
}
TEST(Protocol, HeaderVectorAndVersionRejection) {
    std::array<uint8_t, wire::header_size> bytes{};
    wire::Header h{wire::Type::pose, 1, 0x01020304, 7, 42, 99};
    ASSERT_TRUE(wire::encode_header(h, bytes));
    EXPECT_EQ(bytes[0], 0x50);
    EXPECT_EQ(bytes[1], 0x56);
    EXPECT_EQ(bytes[2], 0x52);
    EXPECT_EQ(bytes[3], 0x31);
    EXPECT_EQ(bytes[8], 4);
    EXPECT_EQ(bytes[16], 42);
    auto decoded = wire::decode_header(bytes);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->session, h.session);
    EXPECT_EQ(decoded->device, 7);
    EXPECT_EQ(decoded->send_ns, 99);
    bytes[4] = 2;
    EXPECT_FALSE(wire::decode_header(bytes));
    bytes[4] = 1;
    bytes[14] = 1;
    EXPECT_FALSE(wire::decode_header(bytes));
    bytes[14] = 0;
    bytes[5] = 1;
    EXPECT_FALSE(wire::decode_header(bytes)); // v1.0 has no newer minor layout negotiated.
}
TEST(Protocol, PoseRoundtripAndMalformedFloats) {
    wire::PosePacket p{
        12, 1234, {{1, 2, 3}, Quat::exponential({0, 0.3, 0}), {0, 1, 0}}, {0, 9.8, 0}, 1};
    std::array<uint8_t, wire::pose_size> b{};
    ASSERT_TRUE(wire::encode_pose(p, b));
    auto d = wire::decode_pose(b);
    ASSERT_TRUE(d);
    EXPECT_NEAR(d->pose.position.z, 3, 1e-6);
    EXPECT_EQ(d->sequence, 12);
    EXPECT_NEAR(d->pose.orientation.y, p.pose.orientation.y, 1e-6);
    for (size_t i = 0; i < b.size(); ++i)
        EXPECT_FALSE(wire::decode_pose(std::span(b).first(i)));
    b[12] = 0;
    b[13] = 0;
    b[14] = 0xc0;
    b[15] = 0x7f;
    EXPECT_FALSE(wire::decode_pose(b));
}
TEST(Protocol, ControlFragmentationCoalescingAndOversize) {
    const wire::Bytes message{'{', '}'};
    const auto framed = wire::ControlFramer::frame(message);
    wire::ControlFramer parser;
    std::vector<wire::Bytes> output;
    for (auto byte : framed)
        ASSERT_TRUE(parser.feed(std::span(&byte, 1), output));
    ASSERT_EQ(output.size(), 1);
    EXPECT_EQ(output[0], message);
    wire::Bytes combined = framed;
    combined.insert(combined.end(), framed.begin(), framed.end());
    ASSERT_TRUE(parser.feed(combined, output));
    EXPECT_EQ(output.size(), 3);
    std::array<uint8_t, 4> invalid{0xff, 0xff, 0xff, 0x7f};
    EXPECT_FALSE(parser.feed(invalid, output));
    EXPECT_TRUE(parser.failed());
    EXPECT_FALSE(parser.feed(framed, output));
}
TEST(Protocol, DeterministicMalformedFuzz) {
    std::mt19937 random(7331);
    wire::Bytes data(1300);
    for (int i = 0; i < 20000; ++i) {
        const auto n = random() % data.size();
        for (size_t j = 0; j < n; ++j)
            data[j] = static_cast<uint8_t>(random());
        const auto span = std::span<const uint8_t>(data).first(n);
        (void)wire::decode_header(span);
        (void)wire::decode_pose(span);
        (void)wire::decode_video_header(span);
    }
}
TEST(Crypto, X25519DirectionalKeysAndTranscript) {
    KeyPair client, server;
    std::array<uint8_t, 64> transcript{};
    transcript[0] = 9;
    const auto a = exchange_keys(client, server.public_key, true, transcript),
               b = exchange_keys(server, client.public_key, false, transcript);
    EXPECT_EQ(a.transmit, b.receive);
    EXPECT_EQ(a.receive, b.transmit);
    EXPECT_NE(a.transmit, a.receive);
    transcript[1] = 1;
    const auto changed = exchange_keys(client, server.public_key, true, transcript);
    EXPECT_NE(a.transmit, changed.transmit);
    Key zero{};
    EXPECT_THROW(exchange_keys(client, zero, true, transcript), std::invalid_argument);
}
TEST(Crypto, TamperReplayNonceAndHeaderAuthentication) {
    KeyPair client, server;
    std::array<uint8_t, 32> transcript{};
    PacketCipher a(123, exchange_keys(client, server.public_key, true, transcript)),
        b(123, exchange_keys(server, client.public_key, false, transcript));
    wire::Bytes message{1, 2, 3};
    auto first = a.seal(wire::Type::pose, 0, 55, message),
         second = a.seal(wire::Type::pose, 0, 55, message);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(*first, *second);
    wire::Header h;
    auto tampered = *first;
    tampered.back() ^= 1;
    EXPECT_FALSE(b.open(tampered, h));
    tampered = *first;
    tampered[12] ^= 1;
    EXPECT_FALSE(b.open(tampered, h));
    auto plain = b.open(*second, h);
    ASSERT_TRUE(plain);
    EXPECT_EQ(*plain, message);
    EXPECT_EQ(h.counter, 2);
    plain = b.open(*first, h);
    ASSERT_TRUE(plain);
    EXPECT_EQ(h.counter, 1);
    EXPECT_FALSE(b.open(*first, h));
    EXPECT_FALSE(a.seal(wire::Type::pose, 0, 0, wire::Bytes(wire::max_payload + 1)));
}
TEST(Crypto, ReplayWindowEdges) {
    ReplayWindow w;
    EXPECT_FALSE(w.accept(0));
    EXPECT_TRUE(w.accept(5000));
    EXPECT_TRUE(w.accept(4999));
    EXPECT_FALSE(w.accept(4999));
    EXPECT_FALSE(w.accept(1));
    EXPECT_TRUE(w.accept(6024));
    EXPECT_FALSE(w.accept(5000));
    EXPECT_TRUE(w.accept(6023));
}
TEST(Crypto, PreallocatedPacketBuffersAndAuthenticationBeforeReplayAdvance) {
    KeyPair a, b;
    std::array<uint8_t, 32> transcript{};
    PacketCipher tx(3, exchange_keys(a, b.public_key, true, transcript)),
        rx(3, exchange_keys(b, a.public_key, false, transcript));
    std::array<uint8_t, wire::max_datagram> packet{};
    std::array<uint8_t, wire::max_payload> plain{};
    std::array<uint8_t, 3> message{4, 5, 6};
    wire::Header h;
    EXPECT_FALSE(tx.seal_into(wire::Type::pose, 0, 1, message, std::span(packet).first(1)));
    auto size = tx.seal_into(wire::Type::pose, 0, 1, message, packet);
    ASSERT_TRUE(size);
    auto corrupted = packet;
    corrupted[16] = 255;
    EXPECT_FALSE(rx.open_into(std::span(corrupted).first(*size), h, plain));
    auto length = rx.open_into(std::span(packet).first(*size), h, plain);
    ASSERT_TRUE(length);
    EXPECT_EQ(*length, 3);
    EXPECT_EQ(h.counter, 1);
    EXPECT_TRUE(std::equal(message.begin(), message.end(), plain.begin()));
}
TEST(Fec, ReedSolomonAllThreeErasureCombinations) {
    constexpr uint8_t k = 5, m = 3;
    Fec code(k, m);
    std::array<std::array<uint8_t, 257>, k + m> data{};
    std::array<std::span<uint8_t>, k + m> spans{};
    std::mt19937 random(18);
    for (size_t i = 0; i < k; ++i)
        for (auto &b : data[i])
            b = static_cast<uint8_t>(random());
    for (size_t i = 0; i < spans.size(); ++i)
        spans[i] = data[i];
    ASSERT_TRUE(code.encode(spans));
    const auto original = data;
    for (size_t a = 0; a < k + m; ++a)
        for (size_t b = a + 1; b < k + m; ++b)
            for (size_t c = b + 1; c < k + m; ++c) {
                data = original;
                std::array<uint8_t, k + m> present{};
                present.fill(1);
                for (auto i : {a, b, c}) {
                    present[i] = 0;
                    data[i].fill(0);
                }
                ASSERT_TRUE(code.recover(spans, present));
                EXPECT_EQ(data, original);
            }
}
TEST(Fec, XorRecoveryAndInsufficientParity) {
    Fec code(3, 1, FecKind::xor_parity);
    std::array<std::array<uint8_t, 8>, 4> data{};
    data[0].fill(1);
    data[1].fill(2);
    data[2].fill(4);
    std::array<std::span<uint8_t>, 4> spans{};
    for (size_t i = 0; i < 4; ++i)
        spans[i] = data[i];
    ASSERT_TRUE(code.encode(spans));
    EXPECT_EQ(data[3][0], 7);
    data[1].fill(0);
    std::array<uint8_t, 4> present{1, 0, 1, 1};
    ASSERT_TRUE(code.recover(spans, present));
    EXPECT_EQ(data[1][0], 2);
    present[0] = 0;
    EXPECT_FALSE(code.recover(spans, present));
    EXPECT_THROW(Fec(0, 1), std::invalid_argument);
}
TEST(Fec, RandomizedDimensionsAndBursts) {
    std::mt19937 random(42);
    for (int trial = 0; trial < 100; ++trial) {
        const auto k = static_cast<uint8_t>(1 + random() % 32),
                   m = static_cast<uint8_t>(1 + random() % 8);
        Fec code(k, m);
        std::vector<std::vector<uint8_t>> data(k + m, std::vector<uint8_t>(127));
        std::vector<std::span<uint8_t>> spans;
        for (auto &shard : data) {
            for (auto &v : shard)
                v = static_cast<uint8_t>(random());
            spans.push_back(shard);
        }
        ASSERT_TRUE(code.encode(spans));
        const auto original = data;
        std::vector<uint8_t> present(k + m, 1);
        const size_t start = random() % (k + m);
        for (size_t j = 0; j < m; ++j) {
            const auto i = (start + j) % present.size();
            present[i] = 0;
            std::fill(data[i].begin(), data[i].end(), 0);
        }
        ASSERT_TRUE(code.recover(spans, present));
        EXPECT_EQ(data, original);
    }
}
struct Shard {
    wire::VideoHeader header;
    wire::Bytes bytes;
};
static std::vector<Shard> packets(const wire::Bytes &frame, uint64_t id = 1,
                                  uint64_t render_ns = 1) {
    std::vector<Shard> result;
    wire::VideoHeader h;
    h.frame_id = id;
    h.render_ns = render_ns;
    if (!packetize_frame(frame, h, 4, [&](const auto &header, auto bytes) {
            result.push_back({header, {bytes.begin(), bytes.end()}});
            return true;
        }))
        throw std::runtime_error("packetization");
    return result;
}
TEST(Reassembler, InterleavedShardsSurviveABurstWithOneParityPerGroup) {
    wire::Bytes frame(static_cast<size_t>(stream_shard_bytes) * 64 - 100);
    std::iota(frame.begin(), frame.end(), static_cast<uint8_t>(0));
    wire::VideoHeader metadata;
    metadata.frame_id = 1;
    metadata.render_ns = 1;
    std::vector<Shard> sent;
    ASSERT_TRUE(packetize_frame(frame, metadata, 1, [&](const auto &header, auto bytes) {
        sent.push_back({header, {bytes.begin(), bytes.end()}});
        return true;
    }));
    ASSERT_EQ(sent.size(), 68U); // Four groups of 16, plus one parity shard each.
    const auto first_parity = std::find_if(sent.begin(), sent.end(), [](const auto &p) {
        return p.header.shard >= p.header.k;
    });
    EXPECT_EQ(first_parity - sent.begin(), 64); // Data never waits behind parity.
    sent.erase(sent.begin() + 10, sent.begin() + 14); // Four packets lost back to back.
    Reassembler reassembler(1024 * 1024);
    std::optional<FrameView> done;
    for (const auto &p : sent)
        if (const auto out = reassembler.push(p.header, p.bytes, 100))
            done = out;
    ASSERT_TRUE(done);
    EXPECT_EQ(wire::Bytes(done->bytes.begin(), done->bytes.end()), frame);
}
TEST(Reassembler, ParityRoundsToNearestShardAndProtectsSmallFrames) {
    EXPECT_EQ(stream_parity_shards(122, 10), 2); // The default is not silently halved.
    EXPECT_EQ(stream_parity_shards(5, 10), 1);
    EXPECT_EQ(stream_parity_shards(17, 10), 1);
    EXPECT_EQ(stream_parity_shards(122, 25), 4);
    EXPECT_EQ(stream_parity_shards(122, 0), 0); // A cable with no loss spends nothing.
    EXPECT_EQ(stream_parity_shards(0, 10), 0);
}
TEST(Fec, SharedCoderIsReusedAndMatchesAFreshOne) {
    EXPECT_EQ(&shared_fec(16, 2), &shared_fec(16, 2));
    EXPECT_NE(&shared_fec(16, 2), &shared_fec(16, 3));
    EXPECT_THROW((void)shared_fec(0, 1), std::invalid_argument);
    std::array<std::array<uint8_t, 64>, 6> left{}, right{};
    std::array<std::span<uint8_t>, 6> left_spans{}, right_spans{};
    for (size_t i = 0; i < 6; ++i) {
        for (size_t b = 0; b < 64; ++b)
            left[i][b] = right[i][b] = static_cast<uint8_t>(i * 31 + b * 7);
        left_spans[i] = left[i];
        right_spans[i] = right[i];
    }
    ASSERT_TRUE(shared_fec(4, 2).encode(left_spans));
    ASSERT_TRUE(Fec(4, 2).encode(right_spans));
    EXPECT_EQ(left, right);
}
TEST(Reassembler, OutOfOrderBurstRecoveryDuplicatesAndPadding) {
    wire::Bytes frame(27017);
    std::iota(frame.begin(), frame.end(), static_cast<uint8_t>(0));
    auto data = packets(frame);
    std::erase_if(data, [](auto &p) { return p.header.shard >= 2 && p.header.shard < 5; });
    std::reverse(data.begin(), data.end());
    Reassembler reassembler(65536);
    std::optional<FrameView> done;
    for (const auto &p : data) {
        const auto out = reassembler.push(p.header, p.bytes, 100);
        if (out)
            done = out;
    }
    ASSERT_TRUE(done);
    EXPECT_EQ(wire::Bytes(done->bytes.begin(), done->bytes.end()), frame);
    EXPECT_EQ(reassembler.stats().completed, 1);
    EXPECT_GT(reassembler.stats().recovered, 0);
    for (const auto &p : data)
        EXPECT_FALSE(reassembler.push(p.header, p.bytes, 101));
    EXPECT_GT(reassembler.stats().duplicates, 0);
}
TEST(Reassembler, MalformedLimitsTimeoutAndStaleFrames) {
    const wire::Bytes frame(17000, 7);
    auto data = packets(frame);
    Reassembler r(65536);
    auto bad = data[0];
    bad.header.group = 65535;
    EXPECT_FALSE(r.push(bad.header, bad.bytes, 1));
    EXPECT_EQ(r.stats().rejected, 1);
    EXPECT_FALSE(r.push(data[0].header, data[0].bytes, 1));
    r.expire(100'000'000);
    EXPECT_EQ(r.stats().dropped, 1);
    EXPECT_TRUE(r.stats().needs_idr);
    EXPECT_FALSE(r.push(data[1].header, data[1].bytes, 100'000'001));
    EXPECT_EQ(r.stats().completed, 0);
    auto newer = packets(wire::Bytes(200, 9), 2, 100'000'000);
    std::optional<FrameView> complete;
    for (auto &p : newer) {
        auto out = r.push(p.header, p.bytes, 200'000'000);
        if (out)
            complete = out;
    }
    ASSERT_TRUE(complete);
    EXPECT_FALSE(r.push(data[1].header, data[1].bytes, 200'000'001));
    EXPECT_GE(r.stats().rejected, 2);
}
TEST(Reassembler, VideoMetadataWireRoundtripAndConflicts) {
    auto data = packets(wire::Bytes(100, 4));
    std::array<uint8_t, wire::video_header_size> encoded{};
    ASSERT_TRUE(wire::encode_video_header(data[0].header, encoded));
    auto h = wire::decode_video_header(encoded);
    ASSERT_TRUE(h);
    EXPECT_EQ(h->frame_bytes, 100);
    EXPECT_EQ(h->shard_bytes, stream_shard_bytes);
    Reassembler r(65536);
    auto large = packets(wire::Bytes(17000, 1));
    EXPECT_FALSE(r.push(large[0].header, large[0].bytes, 1));
    large[1].header.render_ns++;
    EXPECT_FALSE(r.push(large[1].header, large[1].bytes, 2));
    EXPECT_EQ(r.stats().rejected, 1);
}
TEST(Reassembler, OneFrameReorderUsesNegotiatedFramePeriod) {
    for (const auto fps : {30U, 45U, 60U}) {
        const auto period = (1'000'000'000ULL + fps - 1) / fps;
        Reassembler reassembler(65536, 4, 100'000'000, FecKind::reed_solomon, fps);
        auto newer = packets(wire::Bytes(100, 2), 2, 100'000'000);
        auto older = packets(wire::Bytes(100, 1), 1, 100'000'000 - period);
        ASSERT_TRUE(reassembler.push(newer[0].header, newer[0].bytes, 200));
        ASSERT_TRUE(reassembler.push(older[0].header, older[0].bytes, 201));
        EXPECT_FALSE(reassembler.stats().needs_idr);
    }
}
TEST(ClockSync, SkewConvergenceJitterAndRoundtrip) {
    ClockSync clock;
    EXPECT_FALSE(clock.phone_to_pc_ns(1));
    const double scale = 1.000120;
    const int64_t offset = 37'000'000;
    const int64_t base = 1'000'000'000'000;
    std::mt19937 random(77);
    for (int64_t i = 0; i < 1000; ++i) {
        const int64_t p0 = base + i * 500'000'000;
        const int64_t delay = 500'000 + static_cast<int64_t>(random() % 2'000'000);
        const auto c1 = static_cast<int64_t>(static_cast<double>(p0 + delay) * scale) + offset,
                   c2 = c1 + 100'000;
        const auto p3 = p0 + 2 * delay + 100'000;
        ASSERT_TRUE(clock.observe(p0, c1, c2, p3));
    }
    EXPECT_NEAR(clock.skew_ppm(), 120, 1);
    const auto phone = base + 500'000'000'000;
    const auto converted = clock.phone_to_pc_ns(phone);
    ASSERT_TRUE(converted);
    EXPECT_NEAR(static_cast<double>(*converted),
                static_cast<double>(phone) * scale + static_cast<double>(offset), 1000);
    auto back = clock.pc_to_phone_ns(*converted);
    ASSERT_TRUE(back);
    EXPECT_NEAR(static_cast<double>(*back), static_cast<double>(phone), 2);
    EXPECT_FALSE(clock.observe(10, 1, 2, 0));
}
TEST(Math, PredictionTimewarpAndProjectionConventions) {
    const auto q = predict({}, {0, pi, 0}, 0.5, 1);
    const auto ray = q.rotate({0, 0, -1});
    EXPECT_NEAR(ray.x, -1, 1e-9);
    EXPECT_NEAR(ray.z, 0, 1e-9);
    const auto render = Quat::exponential({0.1, 0.2, 0}),
               current = Quat::exponential({-0.1, 0.5, 0.1});
    const Vec3 local{0.2, 0.1, -1};
    const auto sampled = sampling_rotation(render, current).rotate(local);
    const auto world = render.rotate(sampled), expected = current.rotate(local);
    EXPECT_NEAR((world - expected).norm(), 0, 1e-9);
    const auto roundtrip = geometry_rotation(render, current).rotate(sampled);
    EXPECT_NEAR((roundtrip - local).norm(), 0, 1e-9);
    Projection p{0.8, 1.2, 1.1, 0.9};
    const auto uv = p.uv(p.ray(0.3, 0.7));
    EXPECT_NEAR(uv[0], 0.3, 1e-9);
    EXPECT_NEAR(uv[1], 0.7, 1e-9);
    EXPECT_EQ(p.uv({0, 0, 1})[0], -1);
    const auto clamped = predict({}, {0, 1, 0}, 100);
    EXPECT_NEAR(clamped.y, std::sin(0.04), 1e-12);
    EXPECT_THROW((Quat{0, 0, 0, 0}.unit()), std::invalid_argument);
}
TEST(Math, LensMeshAndRadialReferenceValues) {
    Lens lens{0.34, 0.55};
    EXPECT_NEAR(lens.distort(1), 1.89, 1e-12);
    for (double r = 0; r < 2; r += 0.01)
        EXPECT_NEAR(lens.inverse(lens.distort(r)), r, 1e-10);
    auto mesh = lens_mesh(lens, {});
    EXPECT_EQ(mesh.size(), 1681);
    EXPECT_NEAR(mesh[840].ray_x, 0, 1e-6);
    EXPECT_NEAR(mesh[840].ray_y, 0, 1e-6);
    // On a fixed screen plane x=0.5,y=0, Cardboard's forward mapping
    // samples texture tan-angle 0.5596875, not its inverse.
    EXPECT_NEAR(mesh[850].ray_x, 0.5596875, 1e-6);
    EXPECT_THROW(lens_mesh(lens, {}, 1, 40), std::invalid_argument);
    // Eye size moves the picture away from the lens axis and nothing else.
    const Projection offset{0.9, 0.5, 0.8, 0.8};
    auto plain = lens_mesh(lens, offset, 40, 40), larger = plain;
    scale_mesh_about_lens(larger, offset, 1.25);
    const double axis_x = 2 * 0.9 / 1.4 - 1;
    for (size_t i = 0; i < plain.size(); ++i) {
        EXPECT_FLOAT_EQ(larger[i].ray_x, plain[i].ray_x);
        EXPECT_FLOAT_EQ(larger[i].ray_y, plain[i].ray_y);
        EXPECT_NEAR(larger[i].x - axis_x, (plain[i].x - axis_x) * 1.25, 1e-5);
        EXPECT_NEAR(larger[i].y, plain[i].y * 1.25, 1e-5);
    }
    auto same = plain;
    scale_mesh_about_lens(same, offset, 1.0);
    EXPECT_NEAR(same.front().x, plain.front().x, 1e-6);
    EXPECT_THROW(scale_mesh_about_lens(same, offset, 0.5), std::invalid_argument);
    EXPECT_THROW(scale_mesh_about_lens(same, offset, 1.7), std::invalid_argument);
    EXPECT_THROW(scale_mesh_about_lens(same, offset, std::nan("")), std::invalid_argument);
}
TEST(MediaQueue, BoundedOwnershipPreservesAccessUnitsAndRecyclesAfterConsumption) {
    MediaQueue queue(128);
    wire::Bytes bytes(100, 7);
    wire::VideoHeader header;
    header.frame_bytes = 100;
    for (uint64_t i = 1; i <= 3; ++i) {
        header.frame_id = i;
        ASSERT_TRUE(queue.push({header, bytes}, i));
    }
    EXPECT_FALSE(queue.push({header, bytes}, 4));
    for (uint64_t i = 1; i <= 3; ++i) {
        auto lease = queue.pop();
        ASSERT_TRUE(lease);
        EXPECT_EQ(lease->header.frame_id, i);
        EXPECT_EQ(lease->received_ns, i);
        EXPECT_TRUE(
            std::equal(lease->bytes.begin(), lease->bytes.end(), bytes.begin(), bytes.end()));
        queue.release(lease->slot);
        EXPECT_THROW(queue.release(lease->slot), std::logic_error);
    }
    EXPECT_FALSE(queue.pop());
    header.frame_id = 4;
    EXPECT_TRUE(queue.push({header, bytes}, 4));
    bytes.resize(129);
    header.frame_bytes = 129;
    EXPECT_FALSE(queue.push({header, bytes}, 5));
}
TEST(MediaQueue, ConcurrentProducerConsumerPreserveLeasedFrameBytes) {
    MediaQueue queue(128);
    std::atomic<bool> finished{};
    std::atomic<unsigned> accepted{};
    std::thread producer([&] {
        std::array<uint8_t, 128> bytes{};
        for (uint64_t id = 1; id <= 5000; ++id) {
            bytes.fill(static_cast<uint8_t>(id));
            wire::VideoHeader header;
            header.frame_id = id; header.frame_bytes = static_cast<uint32_t>(bytes.size());
            if (queue.push({header, bytes}, id)) ++accepted;
            else std::this_thread::yield();
        }
        finished.store(true);
    });
    unsigned consumed{}, invalid{};
    uint64_t previous{};
    for (;;) {
        const auto lease = queue.pop();
        if (!lease) {
            if (finished.load() && consumed == accepted.load()) break;
            std::this_thread::yield(); continue;
        }
        if (lease->header.frame_id <= previous || lease->received_ns != lease->header.frame_id ||
            !std::all_of(lease->bytes.begin(), lease->bytes.end(), [&](auto b) {
                return b == static_cast<uint8_t>(lease->header.frame_id);
            })) ++invalid;
        previous = lease->header.frame_id;
        queue.release(lease->slot);
        ++consumed;
    }
    producer.join();
    EXPECT_EQ(consumed, accepted.load());
    EXPECT_GT(consumed, 0);
    EXPECT_EQ(invalid, 0);
}
TEST(Fusion, SyntheticYawAndRecentering) {
    SensorFusion f;
    for (uint64_t i = 1; i <= 4001; ++i)
        ASSERT_TRUE(f.update({i * 2'500'000, {0, pi / 20, 0}, {0, 9.80665, 0}, {}}));
    auto forward = f.orientation().rotate({0, 0, -1});
    EXPECT_NEAR(forward.x, -1, 0.001);
    f.recenter();
    forward = f.orientation().rotate({0, 0, -1});
    EXPECT_NEAR(forward.x, 0, 1e-6);
    EXPECT_NEAR(forward.z, -1, 1e-6);
}
TEST(Fusion, StationaryBiasAndMagneticDisturbance) {
    SensorFusion f;
    f.begin_bias_calibration();
    f.magnetometer_enabled(true);
    for (uint64_t i = 1; i <= 16000; ++i)
        ASSERT_TRUE(f.update({i * 2'500'000, {0, 0.02, 0}, {0, 9.80665, 0}, Vec3{0, 0, -45}}));
    EXPECT_NEAR(f.gyro_bias().y, 0.02, 0.00002);
    EXPECT_LT(f.angular_velocity().norm(), 0.00002);
    ASSERT_TRUE(f.update({40'002'500'000, {0, 0.02, 0}, {0, 9.80665, 0}, Vec3{900, 0, 0}}));
    EXPECT_TRUE(f.magnetic_disturbance());
    EXPECT_FALSE(f.update({40'002'500'000, {}, {}, {}}));
}
TEST(Fusion, CalibrationFreezesBiasAndPreservesSlowIntentionalYaw) {
    PoseState initial;
    EXPECT_EQ(initial.read().pose.orientation.w, 1);
    SensorFusion fusion;
    fusion.begin_bias_calibration();
    uint64_t timestamp = 1;
    for (int i = 0; i < 1200; ++i) {
        timestamp += 2'500'000;
        ASSERT_TRUE(fusion.update({timestamp, {0, 0.02, 0}, {0, 9.80665, 0}, {}}));
    }
    ASSERT_FALSE(fusion.calibrating());
    fusion.recenter();
    for (int i = 0; i < 12000; ++i) {
        timestamp += 2'500'000;
        ASSERT_TRUE(fusion.update({timestamp, {0, 0.03, 0}, {0, 9.80665, 0}, {}}));
    }
    EXPECT_NEAR(fusion.gyro_bias().y, 0.02, 1e-8);
    EXPECT_NEAR(fusion.orientation().rotate({0, 0, -1}).x, -std::sin(0.3), 1e-6);
    fusion.slow_autocenter(0.01);
    EXPECT_NEAR(fusion.orientation().rotate({0, 0, -1}).x, -std::sin(0.3 - 0.00002), 1e-6);
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        const auto x = sensor_to_display({1, 0, 0}, rotation);
        const auto y = sensor_to_display({0, 1, 0}, rotation);
        const auto z = sensor_to_display({0, 0, 1}, rotation);
        EXPECT_NEAR((x.cross(y) - z).norm(), 0, 1e-12);
    }
    EXPECT_EQ(sensor_to_display({0, 9.80665, 0}, 1).x, -9.80665);
    EXPECT_THROW(sensor_to_display({}, 4), std::invalid_argument);
}
TEST(Fusion, QuietGyroCalibratesDespiteHighFrequencyAccelerometerSampleNoise) {
    SensorFusion fusion;
    fusion.begin_bias_calibration();
    const Vec3 bias{0.003, -0.002, 0.001};
    for (uint64_t i = 1; i <= 2000; ++i) {
        const double noise = (i % 2) ? 0.23 : -0.23;
        ASSERT_TRUE(fusion.update({i * 2'500'000, bias,
            {0.04 + noise, noise, 9.80665 + noise}, {}}));
    }
    EXPECT_FALSE(fusion.calibrating());
    EXPECT_NEAR((fusion.gyro_bias() - bias).norm(), 0, 1e-10);
}
TEST(Fusion, CalibrationStillRejectsRotationGyroVarianceAndSustainedAcceleration) {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        SensorFusion fusion;
        fusion.begin_bias_calibration();
        for (uint64_t i = 1; i <= 2000; ++i) {
            const Vec3 gyro = scenario == 0 ? Vec3{0, 0.2, 0} :
                scenario == 1 ? Vec3{0, (i % 2) ? 0.02 : -0.02, 0} : Vec3{};
            ASSERT_TRUE(fusion.update({i * 2'500'000, gyro,
                {0, scenario == 2 ? 10.80665 : 9.80665, 0}, {}}));
        }
        EXPECT_TRUE(fusion.calibrating()) << scenario;
        EXPECT_EQ(fusion.gyro_bias().norm(), 0);
    }
}
TEST(Fusion, PhysicalHeadYawDirectionsAgreeInBothLandscapeOrientations) {
    const std::array<Vec3, 4> device_up{{{0, 1, 0}, {1, 0, 0}, {0, -1, 0}, {-1, 0, 0}}};
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        const auto up = sensor_to_display(device_up[rotation], rotation);
        EXPECT_NEAR((up - Vec3{0, 1, 0}).norm(), 0, 1e-12);
        for (const double direction : {-1.0, 1.0}) {
            SensorFusion fusion;
            for (uint64_t i = 1; i <= 401; ++i)
                ASSERT_TRUE(fusion.update({i * 2'500'000,
                    sensor_to_display(device_up[rotation] * (direction * 0.4), rotation),
                    up * 9.80665, {}}));
            const auto forward = fusion.orientation().rotate({0, 0, -1});
            // Positive yaw turns the camera left, never right.
            EXPECT_NEAR(forward.x, -std::sin(direction * 0.4), 1e-6);
            // A stationary world target moves oppositely on screen, as in VR.
            const auto target = fusion.orientation().inverse().rotate({0, 0, -1});
            EXPECT_NEAR(target.x, std::sin(direction * 0.4), 1e-6);
        }
    }
}
TEST(Fusion, GameRotationBodyMotionAgreesWithGyroDisplayMapping) {
    const Vec3 raw_omega{0.2, -0.3, 0.4};
    const auto initial = Quat::exponential({0.1, 0.2, 0.3});
    for (unsigned rotation = 0; rotation < 4; ++rotation) {
        const auto q0 = game_rotation_to_display(initial, rotation);
        const auto q1 = game_rotation_to_display(initial * Quat::exponential(raw_omega * 0.01), rotation);
        const auto expected = q0 * Quat::exponential(sensor_to_display(raw_omega, rotation) * 0.01);
        EXPECT_NEAR((q1.rotate({0, 0, -1}) - expected.rotate({0, 0, -1})).norm(), 0, 1e-10);
    }
    EXPECT_THROW(game_rotation_to_display({}, 4), std::invalid_argument);
}
TEST(Adaptive, FixedModeAndBandwidthCliffRecovery) {
    AdaptiveController fixed;
    fixed.enabled(false);
    QualityMetrics m;
    m.loss_percent = 20;
    m.thermal_status = 4;
    for (uint64_t t = 0; t < 60000; t += 250)
        fixed.update(t, m);
    EXPECT_EQ(fixed.quality().bitrate_mbps, 60);
    EXPECT_EQ(fixed.quality().scale, 1);
    AdaptiveController c;
    m.thermal_status = 0;
    m.bursty = true;
    for (uint64_t t = 0; t <= 20000; t += 250)
        c.update(t, m);
    EXPECT_EQ(c.quality().bitrate_mbps, 15);
    EXPECT_EQ(c.quality().fec_percent, 25);
    EXPECT_LT(c.quality().scale, 1);
    const auto before = c.quality().scale;
    m = {};
    for (uint64_t t = 20250; t < 30000; t += 250)
        c.update(t, m);
    EXPECT_EQ(c.quality().scale, before);
    for (uint64_t t = 30000; t < 60000; t += 250)
        c.update(t, m);
    EXPECT_GE(c.quality().scale, before);
}
TEST(Adaptive, ThermalTraceAndCleanIncrease) {
    AdaptiveController c;
    QualityMetrics m;
    m.thermal_status = 3;
    for (uint64_t t = 0; t < 15000; t += 250)
        c.update(t, m);
    EXPECT_EQ(c.quality().scale, 0.55);
    EXPECT_EQ(c.quality().fps, 30);
    AdaptiveController clean;
    for (uint64_t t = 0; t <= 1000; t += 250)
        clean.update(t, {});
    EXPECT_NEAR(clean.quality().bitrate_mbps, 63, 1e-8);
}
TEST(Pacing, BoundedSlewAndConvergence) {
    PacingPll pll;
    double lead = 0, previous_error = 4;
    for (int i = 0; i < 2000; ++i) {
        const auto step = pll.update(lead);
        EXPECT_LE(std::abs(step), 0.10000001);
        lead -= step;
        const auto error = 4 - lead;
        if (i > 100)
            EXPECT_LE(std::abs(error), previous_error + 0.001);
        previous_error = std::abs(error);
    }
    EXPECT_NEAR(lead, 4, 0.001);
    EXPECT_EQ(pll.update(std::numeric_limits<double>::quiet_NaN()), 0);
}
TEST(Pacing, ReportedLeadLocksOnDespiteClockDrift) {
    // A 59.85 Hz phone against a 60 Hz PC timer slips about 2.5 ms every second.
    const double period = 1000.0 / 60;
    for (const double drift : {2.5, -2.5, 0.0}) {
        PacingPll pll;
        double lead = 12;
        for (int second = 0; second < 60; ++second) {
            const auto step = pll.report(lead, 4, period);
            EXPECT_LE(std::abs(step), 5.0);
            lead = std::fmod(lead - step + drift + 2 * period, period);
        }
        EXPECT_NEAR(lead, 4, 0.5);
    }
}
TEST(Pacing, ReportTakesTheShortWayAndIgnoresBadInput) {
    PacingPll pll;
    EXPECT_LT(pll.report(16, 1, 1000.0 / 60), 0);
    EXPECT_EQ(pll.report(std::numeric_limits<double>::quiet_NaN(), 4), 0);
    EXPECT_EQ(pll.report(-1, 4), 0);
    EXPECT_EQ(pll.report(4, 4, 0), 0);
}
TEST(Queues, ConcurrentSpscOrderingAndBoundedCapacity) {
    SpscQueue<uint64_t, 64> q;
    constexpr uint64_t count = 100000;
    std::thread producer([&] {
        for (uint64_t i = 0; i < count; ++i)
            while (!q.push(i))
                std::this_thread::yield();
    });
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t value{};
        while (!q.pop(value))
            std::this_thread::yield();
        EXPECT_EQ(value, i);
    }
    producer.join();
    uint64_t value{};
    EXPECT_FALSE(q.pop(value));
}
TEST(PoseHistory, BoundedWrapAndFrameLookup) {
    PoseHistory h;
    for (uint64_t i = 0; i < 1000; ++i)
        h.put({i, i * 100, {}});
    EXPECT_FALSE(h.frame(0));
    auto record = h.frame(999);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->timestamp_ns, 99900);
}
TEST(Video, LimitedRangeBt709AndStrideBounds) {
    std::array<uint8_t, 80> bgra{};
    for (size_t y = 0; y < 4; ++y)
        for (size_t x = 0; x < 4; ++x)
            for (size_t c = 0; c < 4; ++c)
                bgra[y * 20 + x * 4 + c] = 255;
    std::array<uint8_t, 24> nv12{};
    ASSERT_TRUE(bgra_to_nv12(bgra, 20, 4, 4, nv12));
    for (size_t i = 0; i < 16; ++i)
        EXPECT_EQ(nv12[i], 235);
    for (size_t i = 16; i < 24; ++i)
        EXPECT_EQ(nv12[i], 128);
    bgra.fill(0);
    ASSERT_TRUE(bgra_to_nv12(bgra, 20, 4, 4, nv12));
    EXPECT_EQ(nv12[0], 16);
    EXPECT_EQ(nv12[16], 128);
    EXPECT_FALSE(bgra_to_nv12(bgra, 20, 3, 4, nv12));
    EXPECT_FALSE(bgra_to_nv12(std::span(bgra).first(10), 20, 4, 4, nv12));
    EXPECT_TRUE(std::isinf(psnr(nv12, nv12)));
}
TEST(Video, LengthPrefixAmbiguityAndCodecParameterSets) {
    wire::Bytes avcc(260, 0xff);
    avcc[0] = avcc[1] = 0;
    avcc[2] = 1;
    avcc[3] = 0;
    avcc[4] = 0x65;
    const auto normalized = to_annex_b(avcc);
    ASSERT_TRUE(normalized);
    EXPECT_EQ((*normalized)[3], 1);
    EXPECT_EQ((*normalized)[4], 0x65);
    EXPECT_TRUE(contains_idr(*normalized, Codec::h264));
    const wire::Bytes config{1, 100, 0, 51, 0xff, 0xe1, 0, 2, 0x67, 0x11, 1, 0, 2, 0x68, 0x22};
    const auto headers = codec_headers(config, Codec::h264);
    ASSERT_TRUE(headers);
    EXPECT_TRUE(contains_parameter_sets(*headers, Codec::h264));
    EXPECT_FALSE(contains_idr(*headers, Codec::h264));
    const wire::Bytes bad{0, 0, 0, 99, 1};
    EXPECT_FALSE(to_annex_b(bad));
}
} // namespace
