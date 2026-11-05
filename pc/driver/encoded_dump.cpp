// SPDX-License-Identifier: Apache-2.0
#include "encoded_dump.hpp"
#include "windows.hpp"
#include <pocketvr/transport.hpp>
#include <algorithm>

namespace pocketvr::driver {
EncodedDump::EncodedDump(std::filesystem::path directory)
    : EncodedDump(std::move(directory), Limits{}) {}
EncodedDump::EncodedDump(std::filesystem::path directory, Limits limits)
    : directory_(std::move(directory)), limits_(limits) {
    if (!limits.duration_ns || limits.duration_ns > 30'000'000'000 ||
        !limits.max_bytes || limits.max_bytes > 256 * 1024 * 1024)
        throw std::invalid_argument("Invalid encoded capture limits");
}
EncodedDump::~EncodedDump() { set_enabled(false); }
void EncodedDump::set_enabled(bool enabled) {
    {
        std::lock_guard lock(mutex_);
        if (enabled == enabled_) return;
        enabled_ = enabled;
        accepting_ = false;
    }
    worker_.request_stop();
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    if (!enabled) {
        for (auto &slot : slots_) wire::Bytes{}.swap(slot.bytes);
        if (state_ == "waiting for keyframe" || state_ == "recording") state_ = "cancelled";
        return;
    }
    try {
        for (auto &slot : slots_) slot.bytes.resize(max_frame);
        head_ = count_ = 0;
        first_ns_ = queued_bytes_ = bytes_ = frames_ = 0;
        armed_ns_ = monotonic_ns();
        path_.clear(); error_.clear(); finish_ = false;
        error_.reserve(256);
        missed_.store(false);
        state_ = "waiting for keyframe";
        accepting_ = true;
        worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
    } catch (const std::exception &e) {
        accepting_ = false; state_ = "failed"; error_ = e.what();
    }
}
bool EncodedDump::waiting() const {
    std::lock_guard lock(mutex_);
    return accepting_ && !first_ns_;
}
void EncodedDump::record(std::span<const uint8_t> bytes, bool idr, Codec codec) noexcept {
    try {
        std::unique_lock lock(mutex_, std::try_to_lock);
        // Mark gaps even when the diagnostic lock cannot be acquired. Never
        // append a dependent picture after skipping an encoded access unit.
        if (!lock.owns_lock()) { missed_.store(true); return; }
        if (!accepting_ || bytes.empty()) return;
        const auto now = monotonic_ns();
        if (!first_ns_) {
            if (!idr || !contains_parameter_sets(bytes, codec)) return;
            missed_.store(false); // A gap before the initial keyframe is harmless.
            first_ns_ = now; codec_ = codec; state_ = "recording";
        }
        const auto stop = [&](const char *error) {
            accepting_ = false; finish_ = true;
            if (*error) error_ = error;
            wake_.notify_one();
        };
        if (missed_.exchange(false)) { stop("Capture tap contention; valid prefix retained"); return; }
        if (now - first_ns_ >= limits_.duration_ns) { stop(""); return; }
        if (codec != codec_) { stop("Codec changed during capture"); return; }
        if (bytes.size() > max_frame) { stop("Encoded frame exceeds capture buffer"); return; }
        if (queued_bytes_ + bytes.size() > limits_.max_bytes) { stop(""); return; }
        if (count_ == capacity) { stop("Capture disk queue overflow; valid prefix retained"); return; }
        auto &slot = slots_[(head_ + count_) % capacity];
        std::copy(bytes.begin(), bytes.end(), slot.bytes.begin());
        slot.size = bytes.size(); ++count_; queued_bytes_ += bytes.size();
        wake_.notify_one();
    } catch (...) { missed_.store(true); /* Diagnostics must never break the stream. */ }
}
nlohmann::json EncodedDump::status() const {
    std::lock_guard lock(mutex_);
    return {{"ok", true}, {"state", state_}, {"error", error_}, {"path", path_.string()},
            {"frames", frames_}, {"bytes", bytes_}, {"enabled", enabled_},
            {"duration_limit_ms", limits_.duration_ns / 1'000'000},
            {"byte_limit", limits_.max_bytes}};
}
void EncodedDump::run(std::stop_token stop) noexcept {
    win::Handle file;
    try {
        while (true) {
            size_t index{}, size{};
            {
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, stop, std::chrono::milliseconds(50), [&] { return count_ || finish_; });
                const auto now = monotonic_ns();
                if (first_ns_ && missed_.exchange(false)) {
                    error_ = "Capture tap contention; valid prefix retained";
                    accepting_ = false; finish_ = true;
                }
                if (!first_ns_ && now - armed_ns_ > 10'000'000'000) {
                    error_ = "No keyframe with parameter sets within ten seconds";
                    accepting_ = false; finish_ = true;
                }
                if (first_ns_ && now - first_ns_ >= limits_.duration_ns) {
                    accepting_ = false; finish_ = true;
                }
                if (!count_) {
                    if (stop.stop_requested() || finish_) break;
                    continue;
                }
                index = head_; size = slots_[index].size;
            }
            if (!file) {
                std::filesystem::create_directories(directory_);
                const auto path = directory_ / ("capture-" + std::to_string(monotonic_ns()) +
                    (codec_ == Codec::h264 ? ".h264" : ".hevc"));
                file.reset(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                       CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
                if (!file) throw std::runtime_error("Cannot create encoded capture file");
                std::lock_guard lock(mutex_); path_ = path;
            }
            DWORD written{};
            if (!WriteFile(file.get(), slots_[index].bytes.data(), static_cast<DWORD>(size), &written, nullptr) || written != size)
                throw std::runtime_error("Encoded capture write failed");
            {
                std::lock_guard lock(mutex_);
                bytes_ += size; ++frames_;
                head_ = (head_ + 1) % capacity; --count_;
            }
        }
        file.reset();
        std::lock_guard lock(mutex_);
        accepting_ = false;
        state_ = !error_.empty() ? "failed" : stop.stop_requested() ? "cancelled" : "complete";
    } catch (const std::exception &e) {
        file.reset();
        std::lock_guard lock(mutex_);
        accepting_ = false; state_ = "failed"; error_ = e.what();
    } catch (...) {
        std::lock_guard lock(mutex_);
        accepting_ = false; state_ = "failed"; error_ = "Unexpected capture failure";
    }
}
} // namespace pocketvr::driver
