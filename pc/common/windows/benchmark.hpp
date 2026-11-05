// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "process.hpp"
#include <nlohmann/json.hpp>
#include <vector>

namespace pocketvr::win {
struct BenchmarkOptions {
    std::string codec{"h264"};
    uint32_t width{2160}, height{1080}, fps{60}, bitrate{60'000'000}, frames{120};
    bool quality{};
    void validate() const;
};
inline constexpr std::string_view benchmark_marker = "POCKETVR_BENCHMARK_JSON:";
std::vector<std::wstring> benchmark_arguments(const BenchmarkOptions &options, const std::string &preset);
nlohmann::json parse_benchmark_report(std::string_view output, const BenchmarkOptions &options,
                                     const std::string &preset);
nlohmann::json run_benchmark(const std::filesystem::path &tool, const BenchmarkOptions &options,
    const std::string &preset, std::stop_token stop = {});
} // namespace pocketvr::win
