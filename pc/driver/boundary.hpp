// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <spdlog/spdlog.h>
#include <exception>
#include <utility>
namespace pocketvr::driver {
// Called from a catch block, so the reason for the failure reaches the log.
inline void log_boundary(const char *name) noexcept {
    try {
        try {
            throw;
        } catch (const std::exception &error) {
            spdlog::error("Exception contained at OpenVR entry point: {}: {}", name, error.what());
        } catch (...) {
            spdlog::error("Exception contained at OpenVR entry point: {}", name);
        }
    } catch (...) {
    }
}
template <class F, class R> R boundary(const char *name, F &&function, R fallback) noexcept {
    try {
        return function();
    } catch (...) {
        log_boundary(name);
        return fallback;
    }
}
template <class F> void boundary(const char *name, F &&function) noexcept {
    try {
        function();
    } catch (...) {
        log_boundary(name);
    }
}
} // namespace pocketvr::driver
