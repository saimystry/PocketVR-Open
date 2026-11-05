// SPDX-License-Identifier: Apache-2.0
#include "pipeline.hpp"
#include <pocketvr/transport.hpp>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <pocketvr/telemetry.hpp>

namespace pocketvr::driver {
EncoderPipeline::EncoderPipeline(Compositor &compositor, Factory factory, Output output,
                                 uint32_t initial_bitrate, Tap tap)
    : compositor_(compositor), factory_(std::move(factory)), output_(std::move(output)),
      tap_(std::move(tap)), bitrate_(initial_bitrate), initial_bitrate_(initial_bitrate) {
    if (!factory_ || !output_)
        throw std::invalid_argument("Encoder pipeline requires a factory and output sink");
    if (!wake_)
        throw std::runtime_error("Encoder wake event allocation failed");
}
std::shared_ptr<EncoderPipeline> EncoderPipeline::start(Compositor &compositor, Factory factory,
                                                        Output output, uint32_t initial_bitrate, Tap tap) {
    auto pipeline = std::shared_ptr<EncoderPipeline>(
        new EncoderPipeline(compositor, std::move(factory), std::move(output), initial_bitrate, std::move(tap)));
    std::weak_ptr<EncoderPipeline> weak = pipeline;
    compositor.sink([weak](const GpuFrame &frame) {
        const auto locked = weak.lock();
        return locked && locked->enqueue(frame);
    });
    pipeline->worker_ =
        std::jthread([raw = pipeline.get()](std::stop_token stop) { raw->run(stop); });
    return pipeline;
}
EncoderPipeline::~EncoderPipeline() {
    stop();
}
void EncoderPipeline::stop() {
    if (accepting_.exchange(false))
        compositor_.sink(nullptr);
    // Present can already hold a copied sink. Let its bounded enqueue finish
    // before the worker's final lease drain; Present never waits for this join.
    while (enqueue_active_.load()) std::this_thread::yield();
    worker_.request_stop();
    SetEvent(wake_.get());
    if (worker_.joinable())
        worker_.join();
}
bool EncoderPipeline::enqueue(const GpuFrame &frame) noexcept {
    enqueue_active_.fetch_add(1);
    struct Finished { std::atomic<unsigned> &count; ~Finished() { count.fetch_sub(1); } } finished{enqueue_active_};
    if (!accepting_.load() || !ready_.load()) {
        dropped_.fetch_add(1);
        return false; // Present retains ownership on rejection.
    }
    if (!cadence_.accept(frame.submit_ns, input_fps_.load())) {
        guard_skipped_.fetch_add(1); return false;
    }
    if (!input_.push(frame)) { dropped_.fetch_add(1); return false; }
    SetEvent(wake_.get());
    return true;
}
void EncoderPipeline::bitrate(uint32_t value) {
    if (value < 1'000'000 || value > 150'000'000)
        throw std::invalid_argument("Pipeline bitrate must be 1-150 Mbps");
    bitrate_.store(value);
    SetEvent(wake_.get());
}
void EncoderPipeline::input_fps_limit(unsigned fps) {
    if (fps != 0 && fps != 30 && fps != 45 && fps != 60) throw std::invalid_argument("Input FPS cap");
    if (input_fps_.exchange(fps) == 0 && fps) request_idr();
}
PipelineStats EncoderPipeline::stats() const noexcept {
    return {encoded_.load(), dropped_.load(),          resets_.load(),           ready_.load(),
            bitrate_.load(), accepted_bitrate_.load(), bitrate_rejected_.load(), backend_.load(),
            hardware_.load(), input_fps_.load(), guard_skipped_.load(), encode_samples_.load(),
            encode_sample_ns_.load(), encode_p95_ms_.load()};
}
void EncoderPipeline::run(std::stop_token stop) noexcept {
    raise_thread_priority();
    const PreciseTimer timer;
    struct Pending {
        bool active{};
        GpuFrame frame{};
        uint64_t accepted_ns{};
    };
    std::array<Pending, 3> pending{};
    std::unique_ptr<IVideoEncoder> encoder;
    uint64_t retry_ns{}, last_output{};
    RollingLatency<> encode_times;
    uint64_t last_encode_summary{};
    auto release_all = [&] {
        // Destroy the encoder before releasing textures it may still reference.
        encoder.reset();
        encode_times = {}; encode_samples_.store(0); encode_sample_ns_.store(0); encode_p95_ms_.store(0);
        accepted_bitrate_.store(0);
        backend_.store(EncoderBackend::unavailable);
        hardware_.store(false);
        for (auto &slot : pending) {
            if (slot.active) {
                compositor_.release(slot.frame.slot);
                slot.active = false;
                dropped_.fetch_add(1);
            }
        }
        GpuFrame queued;
        while (input_.pop(queued)) {
            compositor_.release(queued.slot);
            dropped_.fetch_add(1);
        }
    };
    while (!stop.stop_requested()) {
        try {
            auto now = monotonic_ns();
            if (!encoder && now >= retry_ns) {
                encoder = factory_();
                if (!encoder)
                    throw std::runtime_error("Encoder factory returned no encoder");
                const auto name = encoder->name();
                backend_.store(name == "AMD AMF"                     ? EncoderBackend::amf
                               : name == "Media Foundation hardware" ? EncoderBackend::mf_hardware
                               : name == "Media Foundation software H.264"
                                   ? EncoderBackend::mf_software
                                   : EncoderBackend::other);
                hardware_.store(encoder->hardware());
                idr_.store(true);
                accepted_bitrate_.store(initial_bitrate_);
                ready_.store(true);
            }
            if (encoder) {
                const auto value = std::min(bitrate_.load(), encoder->max_bitrate());
                if (value && value != accepted_bitrate_.load() && !bitrate_rejected_.load()) {
                    try {
                        encoder->bitrate(value);
                        accepted_bitrate_.store(value);
                    } catch (const std::exception &error) {
                        // Unsupported live tuning must not reset a working stream.
                        bitrate_rejected_.store(true);
                        spdlog::warn("Live bitrate rejected; holding encoder setting: {}",
                                     error.what());
                    }
                }
                // Poll first so backpressure cannot prevent draining a full encoder.
                for (size_t i = 0; i < pending.size(); ++i) {
                    auto frame = encoder->poll();
                    if (!frame)
                        break;
                    auto slot = std::find_if(pending.begin(), pending.end(), [&](const auto &p) {
                        return p.active && p.frame.metadata.frame_id == frame->frame_id;
                    });
                    if (slot == pending.end()) {
                        if (frame->frame_id <= last_output) {
                            dropped_.fetch_add(1); // Duplicate/stale output owns no current lease.
                            continue;
                        }
                        throw std::runtime_error("Encoder output has unknown future frame identity");
                    }
                    auto metadata = slot->frame.metadata;
                    compositor_.release(slot->frame.slot);
                    slot->active = false;
                    metadata.flags = frame->idr ? 1 : 0;
                    encode_times.add(frame->encode_ms);
                    const auto measured = monotonic_ns();
                    if (!last_encode_summary || measured - last_encode_summary >= 500'000'000) {
                        const auto summary = encode_times.summary();
                        encode_p95_ms_.store(summary.p95_ms); encode_samples_.store(summary.samples);
                        encode_sample_ns_.store(measured); last_encode_summary = measured;
                    }
                    // Capture the encoder's reference chain before freshness or
                    // network delivery can omit an access unit from streaming.
                    if (tap_) {
                        try { tap_(*frame); } catch (...) { /* Optional diagnostics are isolated. */ }
                    }
                    if (frame->frame_id <= last_output ||
                        monotonic_ns() - metadata.render_ns > 120'000'000 ||
                        !output_(std::move(*frame), metadata)) {
                        dropped_.fetch_add(1);
                        idr_.store(true);
                    } else {
                        last_output = metadata.frame_id;
                        encoded_.fetch_add(1);
                    }
                }
                now = monotonic_ns();
                for (const auto &slot : pending)
                    if (slot.active && slot.accepted_ns && now - slot.accepted_ns > 250'000'000)
                        throw std::runtime_error("Encoder stalled for 250 ms");
                // An input that the encoder has not accepted remains in this slot and is retried.
                // accepted_ns == 0 distinguishes it from a frame already handed to the encoder.
                auto slot = std::find_if(pending.begin(), pending.end(),
                                         [](const auto &p) { return p.active && !p.accepted_ns; });
                if (slot == pending.end()) {
                    slot = std::find_if(pending.begin(), pending.end(),
                                        [](const auto &p) { return !p.active; });
                    if (slot != pending.end()) {
                        GpuFrame frame;
                        if (input_.pop(frame))
                            *slot = {true, frame, 0};
                        else
                            slot = pending.end();
                    }
                }
                if (slot != pending.end()) {
                    // Replace work the encoder has not accepted with the freshest
                    // compositor output. Dropping here cannot break codec references.
                    GpuFrame fresher;
                    while (input_.pop(fresher)) {
                        compositor_.release(slot->frame.slot);
                        dropped_.fetch_add(1);
                        slot->frame = fresher;
                    }
                    now = monotonic_ns();
                    if (now - slot->frame.submit_ns > 100'000'000) {
                        compositor_.release(slot->frame.slot);
                        slot->active = false;
                        dropped_.fetch_add(1);
                    } else {
                        const auto force = idr_.exchange(false);
                        if (encoder->submit(slot->frame.texture, slot->frame.metadata.frame_id,
                                            force))
                            slot->accepted_ns = now;
                        else if (force)
                            idr_.store(true);
                    }
                }
            }
        } catch (const std::exception &e) {
            ready_.store(false);
            try {
                spdlog::error("Encoder pipeline reset: {}", e.what());
            } catch (...) {
            }
            release_all();
            resets_.fetch_add(1);
            retry_ns = monotonic_ns() + 1'000'000'000;
        } catch (...) {
            ready_.store(false);
            try {
                spdlog::error("Encoder pipeline reset after unknown exception");
            } catch (...) {
            }
            release_all();
            resets_.fetch_add(1);
            retry_ns = monotonic_ns() + 1'000'000'000;
        }
        // Encoders do not signal finished output, so poll quickly while one is working.
        // Otherwise sleep until a new frame arrives; the timeout only bounds retries.
        if (std::any_of(pending.begin(), pending.end(), [](const auto &p) { return p.active; }))
            timer.sleep(250'000);
        else
            WaitForSingleObject(wake_.get(), 2);
    }
    ready_.store(false);
    release_all();
}
} // namespace pocketvr::driver
