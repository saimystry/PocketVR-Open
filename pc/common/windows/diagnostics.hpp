// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace pocketvr::win {
struct DiagnosticFile {
    std::string name, contents;
};
inline constexpr size_t diagnostic_log_limit = 512 * 1024;
inline constexpr size_t diagnostic_zip_limit = 8 * 1024 * 1024;
// These APIs are for a desktop worker, never streaming/VR callbacks.
nlohmann::json diagnostic_snapshot(const nlohmann::json &snapshot);
std::string diagnostic_log(std::string_view text, size_t &redacted_lines);
std::vector<DiagnosticFile> collect_diagnostics(const nlohmann::json &snapshot,
    const std::filesystem::path &logs, std::stop_token stop = {});
std::vector<uint8_t> diagnostic_zip(std::span<const DiagnosticFile> files);
void export_diagnostics(const std::filesystem::path &destination,
    const nlohmann::json &snapshot, const std::filesystem::path &logs,
    std::stop_token stop = {});
} // namespace pocketvr::win
