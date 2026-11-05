// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pocketvr/media_queue.hpp>
#include <pocketvr/worker.hpp>
#include <pocketvr/telemetry.hpp>
#include <android/native_window.h>
#include <nlohmann/json.hpp>
#include <functional>
#include <memory>

namespace pocketvr::android {
struct CodecEvents;
struct DecoderConfig {
    std::string name, mime;
    uint32_t width{}, height{}, fps{60};
    std::function<bool(const wire::VideoHeader &, uint64_t, uint64_t, uint64_t)> present;
    std::shared_ptr<std::atomic<bool>> keyframe_wanted; // Sent at once by the media link.
};
class Decoder final {
  public:
    Decoder(std::shared_ptr<ANativeWindow> window, DecoderConfig config);
    ~Decoder();
    void stop();
    bool enqueue(FrameView frame);
    bool take_idr_request() {
        return needs_idr_.exchange(false);
    }
    nlohmann::json status() const;

  private:
    std::shared_ptr<ANativeWindow> window_;
    DecoderConfig config_;
    std::unique_ptr<CodecEvents> events_;
    MediaQueue queue_;
    std::atomic<bool> running_{}, awaiting_idr_{true}, needs_idr_{true};
    std::atomic<uint64_t> submitted_{}, decoded_{}, dropped_{}, last_frame_{};
    std::atomic<double> last_decode_ms_{};
    mutable std::mutex error_mutex_;
    std::string error_;
    mutable std::mutex latency_mutex_;
    RollingLatency<> latencies_;
    Worker worker_;
    void run(StopToken stop) noexcept;
    void want_keyframe() noexcept {
        needs_idr_.store(true);
        if (config_.keyframe_wanted)
            config_.keyframe_wanted->store(true);
    }
};
} // namespace pocketvr::android
