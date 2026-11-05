// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "encoder.hpp"
#include "compositor.hpp"
#include "realtime.hpp"
#include <pocketvr/tracking.hpp>
#include <pocketvr/adaptive.hpp>
#include <thread>

namespace pocketvr::driver {
enum class EncoderBackend { unavailable, amf, mf_hardware, mf_software, other };
inline const char *encoder_backend_name(EncoderBackend backend) {
    switch (backend) {
    case EncoderBackend::amf:
        return "AMD AMF";
    case EncoderBackend::mf_hardware:
        return "Media Foundation hardware";
    case EncoderBackend::mf_software:
        return "Media Foundation software H.264";
    case EncoderBackend::other:
        return "Other encoder";
    default:
        return "Unavailable";
    }
}
struct PipelineStats {
    uint64_t encoded{}, dropped{}, resets{};
    bool ready{};
    uint32_t requested_bitrate{}, accepted_bitrate{};
    bool bitrate_rejected{};
    EncoderBackend backend{};
    bool hardware{};
    unsigned input_fps_limit{};
    uint64_t guard_skipped{}, encode_samples{}, encode_sample_ns{};
    double encode_p95_ms{};
};
// The output sink enqueues a whole compressed access unit without blocking.
// Compositor and Gpu must outlive this object. One Present producer is supported.
class EncoderPipeline final : public std::enable_shared_from_this<EncoderPipeline> {
  public:
    using Factory = std::function<std::unique_ptr<IVideoEncoder>()>;
    using Output = std::function<bool(EncodedFrame &&, const wire::VideoHeader &)>;
    using Tap = std::function<void(const EncodedFrame &)>;
    static std::shared_ptr<EncoderPipeline> start(Compositor &compositor, Factory factory,
                                                  Output output, uint32_t initial_bitrate = 0,
                                                  Tap tap = {});
    ~EncoderPipeline();
    void stop();
    void request_idr() noexcept {
        idr_.store(true);
    }
    void bitrate(uint32_t value);
    void input_fps_limit(unsigned fps);
    PipelineStats stats() const noexcept;

  private:
    EncoderPipeline(Compositor &compositor, Factory factory, Output output,
                    uint32_t initial_bitrate, Tap tap);
    bool enqueue(const GpuFrame &frame) noexcept;
    void run(std::stop_token stop) noexcept;
    Compositor &compositor_;
    Factory factory_;
    Output output_;
    Tap tap_;
    SpscQueue<GpuFrame, 4> input_;
    std::atomic<bool> accepting_{true}, ready_{}, idr_{true};
    std::atomic<unsigned> enqueue_active_{};
    std::atomic<uint32_t> bitrate_{};
    uint32_t initial_bitrate_{};
    std::atomic<uint32_t> accepted_bitrate_{};
    std::atomic<bool> bitrate_rejected_{};
    std::atomic<EncoderBackend> backend_{};
    std::atomic<bool> hardware_{};
    std::atomic<uint64_t> encoded_{}, dropped_{}, resets_{};
    std::atomic<unsigned> input_fps_{UINT32_MAX};
    FrameCadence cadence_;
    std::atomic<uint64_t> guard_skipped_{}, encode_samples_{}, encode_sample_ns_{};
    std::atomic<double> encode_p95_ms_{};
    win::Handle wake_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::jthread worker_;
};
} // namespace pocketvr::driver
