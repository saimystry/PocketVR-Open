// SPDX-License-Identifier: Apache-2.0
#include "benchmark.hpp"
#include <cmath>
#include <sstream>

namespace pocketvr::win {
namespace {
bool preset_valid(const std::string &preset) {
    return preset == "fast" || preset == "balanced" || preset == "quality";
}
double metric(const nlohmann::json &report, const char *name, double maximum) {
    const auto &field = report.at(name);
    if (!field.is_number()) throw std::runtime_error("Benchmark returned a nonnumeric metric");
    const auto value = field.get<double>();
    if (!std::isfinite(value) || value < 0 || value > maximum)
        throw std::runtime_error("Benchmark returned an invalid metric");
    return value;
}
}
void BenchmarkOptions::validate() const {
    if ((codec != "h264" && codec != "hevc") || width < 128 || height < 128 ||
        width > 4096 || height > 4096 || (width & 1) || (height & 1) ||
        (fps != 30 && fps != 45 && fps != 60) || bitrate < 10'000'000 ||
        bitrate > 150'000'000 || frames < 30 || frames > 600)
        throw std::invalid_argument("Invalid benchmark settings: use even dimensions, 30/45/60 FPS, 10-150 Mbps and 30-600 frames");
}
std::vector<std::wstring> benchmark_arguments(const BenchmarkOptions &o, const std::string &preset) {
    o.validate();
    if (!preset_valid(preset)) throw std::invalid_argument("Unknown benchmark preset");
    std::vector<std::wstring> args{L"--machine-readable", L"--require-hardware",
        L"--width", std::to_wstring(o.width), L"--height", std::to_wstring(o.height),
        L"--fps", std::to_wstring(o.fps), L"--bitrate", std::to_wstring(o.bitrate),
        L"--frames", std::to_wstring(o.frames), L"--codec", o.codec == "hevc" ? L"hevc" : L"h264",
        L"--preset", std::wstring(preset.begin(), preset.end())};
    if (o.quality) args.push_back(L"--quality");
    return args;
}
nlohmann::json parse_benchmark_report(std::string_view output, const BenchmarkOptions &o,
                                     const std::string &preset) {
    o.validate();
    if (!preset_valid(preset)) throw std::invalid_argument("Unknown benchmark preset");
    if (output.size() > 256 * 1024) throw std::runtime_error("Benchmark output exceeds limit");
    std::istringstream input{std::string(output)}; std::string line;
    nlohmann::json report;
    unsigned found{};
    while (std::getline(input, line)) {
        if (!line.starts_with(benchmark_marker)) continue;
        if (++found != 1 || line.size() > 65536) throw std::runtime_error("Ambiguous or oversized benchmark result");
        report = nlohmann::json::parse(line.substr(benchmark_marker.size()));
    }
    if (found != 1 || !report.is_object()) throw std::runtime_error("Benchmark returned no compatible result; rebuild the benchmark tool and UI together");
    if (report.at("report_version") != 1 || report.at("preset") != preset ||
        report.at("codec") != o.codec || report.at("width") != o.width ||
        report.at("height") != o.height || report.at("fps") != o.fps || report.at("frames") != o.frames ||
        report.at("hardware") != true || report.at("warp") != false ||
        report.at("phone_decode_measured") != false || report.at("headset_latency_measured") != false ||
        std::abs(metric(report, "target_bitrate_mbps", 150) - static_cast<double>(o.bitrate) / 1e6) > 0.001)
        throw std::runtime_error("Benchmark result does not match the requested hardware test");
    const auto p50 = metric(report, "encode_ms_p50", 60'000);
    if (metric(report, "encode_ms_p95", 60'000) < p50 || metric(report, "actual_bitrate_mbps", 10000) <= 0)
        throw std::runtime_error("Benchmark timing/bitrate result is inconsistent");
    (void)metric(report, "wall_fps_including_synthetic_generation_and_quality", 1'000'000);
    for (const auto *key : {"encoder", "gpu_description"})
        if (!report.at(key).is_string() || report.at(key).get_ref<const std::string &>().empty() ||
            report.at(key).get_ref<const std::string &>().size() > 256)
            throw std::runtime_error("Benchmark backend/GPU identification is invalid");
    if (o.quality || report.contains("software_decode_ms_p95"))
        (void)metric(report, "software_decode_ms_p95", 60'000);
    if (o.quality || report.contains("sampled_luma_psnr_db_p50_capped_100"))
        (void)metric(report, "sampled_luma_psnr_db_p50_capped_100", 100);
    return report;
}
nlohmann::json run_benchmark(const std::filesystem::path &tool, const BenchmarkOptions &o,
    const std::string &preset, std::stop_token stop) {
    const auto arguments = benchmark_arguments(o, preset);
    const auto result = run_process(tool, arguments, stop, std::chrono::minutes(5), 256 * 1024);
    if (result.exit_code) {
        auto error = result.output;
        if (error.size() > 2048) error.erase(0, error.size() - 2048);
        throw std::runtime_error("Encoder benchmark failed: " + error);
    }
    return parse_benchmark_report(result.output, o, preset);
}
} // namespace pocketvr::win
