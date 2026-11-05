// SPDX-License-Identifier: Apache-2.0
#include "benchmark.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <limits>

using namespace pocketvr::win;
namespace {
using Json = nlohmann::json;
Json fixture(const BenchmarkOptions &o = {}) {
    return {{"report_version", 1}, {"preset", "fast"}, {"codec", o.codec},
        {"width", o.width}, {"height", o.height}, {"fps", o.fps}, {"frames", o.frames},
        {"hardware", true}, {"warp", false}, {"encoder", "Synthetic hardware fixture"},
        {"gpu_description", "Test fixture, not a measured GPU"},
        {"target_bitrate_mbps", o.bitrate / 1e6}, {"actual_bitrate_mbps", 58.2},
        {"encode_ms_p50", 5.0}, {"encode_ms_p95", 7.0},
        {"wall_fps_including_synthetic_generation_and_quality", 30.0},
        {"phone_decode_measured", false}, {"headset_latency_measured", false}};
}
std::string output(const Json &value) { return "backend initialization log\n" + std::string(benchmark_marker) + value.dump() + "\r\n"; }
TEST(EncoderComparison, ArgumentsRequireHardwareAndPreserveTheSameTestSettings) {
    BenchmarkOptions o; o.codec = "hevc"; o.quality = true; o.frames = 300;
    for (const auto *preset : {"fast", "balanced", "quality"}) {
        const auto args = benchmark_arguments(o, preset);
        EXPECT_NE(std::find(args.begin(), args.end(), L"--require-hardware"), args.end());
        EXPECT_NE(std::find(args.begin(), args.end(), L"--quality"), args.end());
        EXPECT_NE(std::find(args.begin(), args.end(), L"300"), args.end());
        EXPECT_EQ(args[0], L"--machine-readable");
    }
    EXPECT_THROW(benchmark_arguments(o, "unknown"), std::invalid_argument);
    o.frames = 0; EXPECT_THROW(o.validate(), std::invalid_argument);
    o.frames = 120; o.width = 2159; EXPECT_THROW(o.validate(), std::invalid_argument);
    o.width = 2160; o.bitrate = 9'000'000; EXPECT_THROW(o.validate(), std::invalid_argument);
}
TEST(EncoderComparison, ReportRejectsSoftwareFallbackMismatchesAndFalsePhoneClaims) {
    const BenchmarkOptions o;
    EXPECT_EQ(parse_benchmark_report(output(fixture()), o, "fast")["encode_ms_p95"], 7.0);
    for (const auto &[key, value] : std::vector<std::pair<std::string, Json>>{
             {"hardware", false}, {"warp", true}, {"preset", "balanced"}, {"width", 1280},
             {"frames", 30}, {"target_bitrate_mbps", 80}, {"phone_decode_measured", true},
             {"headset_latency_measured", true}, {"report_version", 99}}) {
        auto bad = fixture(); bad[key] = value;
        EXPECT_THROW(parse_benchmark_report(output(bad), o, "fast"), std::runtime_error);
    }
    EXPECT_THROW(parse_benchmark_report("no result", o, "fast"), std::runtime_error);
    EXPECT_THROW(parse_benchmark_report(output(fixture()) + output(fixture()), o, "fast"), std::runtime_error);
    EXPECT_THROW(parse_benchmark_report(std::string(256 * 1024 + 1, 'x'), o, "fast"), std::runtime_error);
}
TEST(EncoderComparison, MetricsMustBeFiniteConsistentAndQualityIsExplicitlyPcOnly) {
    BenchmarkOptions o;
    for (const auto &[key, value] : std::vector<std::pair<std::string, Json>>{
             {"encode_ms_p95", 4.0}, {"encode_ms_p50", -1}, {"actual_bitrate_mbps", 0},
             {"encode_ms_p95", std::numeric_limits<double>::infinity()}, {"gpu_description", ""}}) {
        auto bad = fixture(); bad[key] = value;
        EXPECT_THROW(parse_benchmark_report(output(bad), o, "fast"), std::exception);
    }
    o.quality = true;
    EXPECT_THROW(parse_benchmark_report(output(fixture()), o, "fast"), std::exception);
    auto quality = fixture(); quality["software_decode_ms_p95"] = 3.0;
    quality["sampled_luma_psnr_db_p50_capped_100"] = 42.5;
    EXPECT_EQ(parse_benchmark_report(output(quality), o, "fast")["sampled_luma_psnr_db_p50_capped_100"], 42.5);
    quality["sampled_luma_psnr_db_p50_capped_100"] = 101;
    EXPECT_THROW(parse_benchmark_report(output(quality), o, "fast"), std::runtime_error);
}
}
