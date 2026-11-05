// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <span>
#include <stop_token>
#include <string>

namespace pocketvr::win {
struct ProcessResult { uint32_t exit_code{}; std::string output; };
std::wstring quote_argument(std::wstring_view value);
// Executes one explicit executable without a shell, with bounded combined output.
ProcessResult run_process(const std::filesystem::path &executable,
    std::span<const std::wstring> arguments, std::stop_token stop = {},
    std::chrono::milliseconds timeout = std::chrono::seconds(10), size_t output_limit = 65536);
} // namespace pocketvr::win
