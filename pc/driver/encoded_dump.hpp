// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pocketvr/video.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>

namespace pocketvr::driver {
// Optional diagnostic tap. All buffers are allocated by set_enabled, never by
// record. A recording overflow stops the capture, never drops a reference frame
// and then appends dependent pictures. The live stream is entirely independent.
class EncodedDump {
  public:
    struct Limits {
        uint64_t duration_ns = 5'000'000'000;
        uint64_t max_bytes = 64 * 1024 * 1024;
    };
    explicit EncodedDump(std::filesystem::path directory);
    EncodedDump(std::filesystem::path directory, Limits limits);
    ~EncodedDump();
    void set_enabled(bool enabled); // Serialized setup/settings thread only.
    bool waiting() const;
    void record(std::span<const uint8_t> bytes, bool idr, Codec codec) noexcept;
    nlohmann::json status() const;

  private:
    static constexpr size_t capacity = 8, max_frame = 8 * 1024 * 1024;
    struct Slot { wire::Bytes bytes; size_t size{}; };
    std::filesystem::path directory_, path_;
    Limits limits_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::array<Slot, capacity> slots_;
    size_t head_{}, count_{};
    uint64_t first_ns_{}, armed_ns_{}, queued_bytes_{}, bytes_{}, frames_{};
    Codec codec_{};
    bool enabled_{}, accepting_{}, finish_{};
    std::atomic<bool> missed_{};
    std::string state_{"disabled"}, error_;
    std::jthread worker_;
    void run(std::stop_token stop) noexcept;
};
} // namespace pocketvr::driver
