// SPDX-License-Identifier: Apache-2.0
#include "decoder.hpp"
#include <pocketvr/transport.hpp>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <sys/resource.h>
#include <unistd.h>
#include <array>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <utility>

namespace pocketvr::android {
// MediaCodec reports ready buffers from its own thread. Only the worker touches the
// codec, so these callbacks just record the event and wake it.
struct CodecOutput {
    size_t index{};
    int64_t presentation_us{};
    uint32_t flags{};
};
template <class T> class EventRing {
    std::array<T, 64> items_{};
    size_t head_{}, count_{};

  public:
    bool push(T value) {
        if (count_ == items_.size())
            return false;
        items_[(head_ + count_++) % items_.size()] = value;
        return true;
    }
    bool pop(T &value) {
        if (!count_)
            return false;
        value = items_[head_];
        head_ = (head_ + 1) % items_.size();
        --count_;
        return true;
    }
};
struct CodecEvents {
    std::mutex mutex;
    std::condition_variable changed;
    EventRing<size_t> inputs;
    EventRing<CodecOutput> outputs;
    bool signalled{}, format_changed{}, failed{};
    void wake() {
        {
            std::lock_guard lock(mutex);
            signalled = true;
        }
        changed.notify_one();
    }
    template <class F> void record(F change) {
        {
            std::lock_guard lock(mutex);
            if (!change())
                failed = true; // More buffers than any codec owns.
            signalled = true;
        }
        changed.notify_one();
    }
    template <class T> bool take(EventRing<T> &ring, T &value) {
        std::lock_guard lock(mutex);
        return ring.pop(value);
    }
    bool take_format_change() {
        std::lock_guard lock(mutex);
        return std::exchange(format_changed, false);
    }
    bool has_failed() {
        std::lock_guard lock(mutex);
        return failed;
    }
    void wait(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, timeout, [&] { return signalled; });
        signalled = false;
    }
};
namespace {
void on_input(AMediaCodec *, void *context, int32_t index) {
    auto &events = *static_cast<CodecEvents *>(context);
    events.record([&] { return events.inputs.push(static_cast<size_t>(index)); });
}
void on_output(AMediaCodec *, void *context, int32_t index, AMediaCodecBufferInfo *info) {
    auto &events = *static_cast<CodecEvents *>(context);
    events.record([&] {
        return events.outputs.push({static_cast<size_t>(index), info->presentationTimeUs, info->flags});
    });
}
void on_format(AMediaCodec *, void *context, AMediaFormat *) {
    auto &events = *static_cast<CodecEvents *>(context);
    events.record([&] { return events.format_changed = true; });
}
void on_error(AMediaCodec *, void *context, media_status_t, int32_t, const char *) {
    auto &events = *static_cast<CodecEvents *>(context);
    events.record([] { return false; });
}
} // namespace
Decoder::Decoder(std::shared_ptr<ANativeWindow> window, DecoderConfig config)
    : window_(std::move(window)), config_(std::move(config)),
      events_(std::make_unique<CodecEvents>()) {
    if (!window_ || config_.name.empty() || config_.name.size() > 256 ||
        (config_.mime != "video/avc" && config_.mime != "video/hevc") || !config_.width ||
        !config_.height || config_.width > 4096 || config_.height > 4096 || (config_.width & 1) ||
        (config_.height & 1) || config_.fps < 30 || config_.fps > 60)
        throw std::invalid_argument("Invalid decoder configuration");
    worker_ = Worker([this](StopToken stop) { run(stop); });
}
Decoder::~Decoder() {
    stop();
}
void Decoder::stop() {
    worker_.request_stop();
    if (worker_.joinable())
        worker_.join();
    running_.store(false);
}
bool Decoder::enqueue(FrameView frame) {
    const bool idr = (frame.header.flags & 1) != 0;
    if (!running_.load() || (awaiting_idr_.load() && !idr) || frame.header.frame_id > INT64_MAX)
        return false;
    if (!queue_.push(frame, monotonic_ns())) {
        dropped_.fetch_add(1);
        awaiting_idr_.store(true);
        want_keyframe();
        return false;
    }
    if (idr)
        awaiting_idr_.store(false);
    events_->wake();
    return true;
}
nlohmann::json Decoder::status() const {
    LatencySummary latency;
    {
        std::lock_guard lock(latency_mutex_);
        latency = latencies_.summary();
    }
    std::lock_guard lock(error_mutex_);
    return {{"running", running_.load()},           {"name", config_.name},
            {"submitted", submitted_.load()},       {"decoded", decoded_.load()},
            {"dropped", dropped_.load()},           {"frame_id", last_frame_.load()},
            {"decode_ms", last_decode_ms_.load()},  {"decode_samples", latency.samples},
            {"decode_p50_ms", latency.p50_ms},      {"decode_p95_ms", latency.p95_ms},
            {"awaiting_idr", awaiting_idr_.load()}, {"error", error_}};
}
void Decoder::run(StopToken stop) noexcept {
    (void)setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), -8);
    AMediaCodec *codec{};
    auto &events = *events_;
    try {
        auto configure = [&](bool vendor_hints) {
            codec = AMediaCodec_createCodecByName(config_.name.c_str());
            if (!codec)
                throw std::runtime_error("Hardware video decoder is unavailable");
            // Must be set before configure, and each attempt uses a new codec.
            if (AMediaCodec_setAsyncNotifyCallback(
                    codec, AMediaCodecOnAsyncNotifyCallback{on_input, on_output, on_format, on_error},
                    &events) != AMEDIA_OK)
                throw std::runtime_error("Decoder callbacks are unavailable");
            std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)> format(
                AMediaFormat_new(), AMediaFormat_delete);
            if (!format)
                throw std::runtime_error("Decoder format allocation failed");
            AMediaFormat_setString(format.get(), AMEDIAFORMAT_KEY_MIME, config_.mime.c_str());
            AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_WIDTH,
                                  static_cast<int32_t>(config_.width));
            AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_HEIGHT,
                                  static_cast<int32_t>(config_.height));
            AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, 8 * 1024 * 1024);
            AMediaFormat_setInt32(format.get(), "priority", 0);
            AMediaFormat_setFloat(format.get(), "operating-rate",
                                  static_cast<float>(config_.fps * 2));
            if (vendor_hints) {
                AMediaFormat_setInt32(format.get(), "low-latency", 1);
                AMediaFormat_setInt32(format.get(), "vendor.qti-ext-dec-low-latency.enable", 1);
            }
            const auto result =
                AMediaCodec_configure(codec, format.get(), window_.get(), nullptr, 0);
            if (result != AMEDIA_OK) {
                AMediaCodec_delete(codec);
                codec = nullptr;
            }
            return result == AMEDIA_OK;
        };
        if (!configure(true) && !configure(false))
            throw std::runtime_error("Decoder rejected the negotiated stream");
        if (AMediaCodec_start(codec) != AMEDIA_OK)
            throw std::runtime_error("Decoder failed to start");
        struct Timing {
            wire::VideoHeader header{};
            uint64_t queued_ns{}, received_ns{};
        };
        std::array<Timing, 512> timings{};
        bool wait_idr = true;
        std::optional<MediaQueue::Lease> pending;
        uint64_t pending_since{};
        running_.store(true);
        while (!stop.stop_requested()) {
            bool work = false;
            struct Output {
                size_t index{};
                uint64_t id{}, queued_ns{}, received_ns{};
                wire::VideoHeader header{};
            };
            std::optional<Output> newest;
            auto release = [&](size_t index, bool render) {
                if (AMediaCodec_releaseOutputBuffer(codec, index, render) != AMEDIA_OK)
                    throw std::runtime_error("Decoder output release failed");
            };
            if (events.has_failed())
                throw std::runtime_error("Decoder output failed");
            if (events.take_format_change()) {
                std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)> format(
                    AMediaCodec_getOutputFormat(codec), AMediaFormat_delete);
                int32_t width{}, height{};
                if (!format ||
                    !AMediaFormat_getInt32(format.get(), AMEDIAFORMAT_KEY_WIDTH, &width) ||
                    !AMediaFormat_getInt32(format.get(), AMEDIAFORMAT_KEY_HEIGHT, &height) ||
                    width <= 0 || height <= 0 || width > 4096 || height > 4096)
                    throw std::runtime_error("Decoder returned invalid output dimensions");
            }
            // Drain output before submitting more input. One worker owns every codec
            // call, which avoids flush/reset races between codec API threads.
            CodecOutput ready;
            for (unsigned i = 0; i < 8 && events.take(events.outputs, ready); ++i) {
                {
                    work = true;
                    const auto output = ready.index;
                    const auto id = ready.presentation_us < 0
                                        ? 0
                                        : static_cast<uint64_t>(ready.presentation_us);
                    const auto timing = timings[id % timings.size()];
                    const auto now = monotonic_ns();
                    const bool render = timing.header.frame_id == id && id > last_frame_.load() &&
                                        now >= timing.queued_ns &&
                                        now - timing.queued_ns < 50'000'000 &&
                                        !(ready.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG);
                    if (render && (!newest || id > newest->id)) {
                        if (newest) {
                            release(newest->index, false);
                            dropped_.fetch_add(1);
                        }
                        newest = Output{output, id, timing.queued_ns,
                                        timing.received_ns, timing.header};
                    } else {
                        release(output, false);
                        dropped_.fetch_add(1);
                    }
                }
            }
            if (newest) {
                const auto now = monotonic_ns();
                bool fresh = now - newest->queued_ns < 50'000'000;
                if (fresh && config_.present) {
                    // This exact token becomes SurfaceTexture.getTimestamp(),
                    // independent of access-unit PTS and decoder reordering.
                    fresh = config_.present(newest->header, now, newest->received_ns, newest->queued_ns);
                    if (fresh && AMediaCodec_releaseOutputBufferAtTime(
                                     codec, newest->index, static_cast<int64_t>(now)) != AMEDIA_OK)
                        throw std::runtime_error("Decoder texture release failed");
                    if (!fresh)
                        release(newest->index, false);
                } else
                    release(newest->index, fresh);
                if (fresh) {
                    last_frame_.store(newest->id);
                    decoded_.fetch_add(1);
                    last_decode_ms_.store(static_cast<double>(now - newest->queued_ns) / 1e6);
                    // Diagnostics must never stall codec release on a reader.
                    std::unique_lock lock(latency_mutex_, std::try_to_lock);
                    if (lock.owns_lock())
                        latencies_.add(last_decode_ms_.load());
                } else
                    dropped_.fetch_add(1);
            }
            if (!pending) {
                pending = queue_.pop();
                pending_since = monotonic_ns();
            }
            if (pending) {
                const auto now = monotonic_ns();
                const auto idr = (pending->header.flags & 1) != 0;
                if ((wait_idr && !idr) || now - pending->received_ns > decoder_abandon_ns ||
                    now - pending_since > decoder_abandon_ns) {
                    queue_.release(pending->slot);
                    pending.reset();
                    dropped_.fetch_add(1);
                    wait_idr = true;
                    awaiting_idr_.store(true);
                    want_keyframe();
                } else {
                    size_t input{};
                    if (events.take(events.inputs, input)) {
                        size_t capacity{};
                        auto *buffer = AMediaCodec_getInputBuffer(codec, input, &capacity);
                        if (!buffer || capacity < pending->bytes.size())
                            throw std::runtime_error("Decoder input buffer is too small");
                        std::memcpy(buffer, pending->bytes.data(), pending->bytes.size());
                        const auto id = pending->header.frame_id;
                        timings[id % timings.size()] = {pending->header, now, pending->received_ns};
                        if (AMediaCodec_queueInputBuffer(codec, input, 0,
                                                         pending->bytes.size(), id, 0) != AMEDIA_OK)
                            throw std::runtime_error("Decoder input submission failed");
                        queue_.release(pending->slot);
                        pending.reset();
                        submitted_.fetch_add(1);
                        if (idr) {
                            wait_idr = false;
                            awaiting_idr_.store(false);
                        }
                        work = true;
                    }
                }
            }
            // Callbacks and new frames wake this at once. The timeout only keeps stop
            // requests and the abandon check moving.
            if (!work)
                events.wait(std::chrono::milliseconds(2));
        }
        if (pending)
            queue_.release(pending->slot);
    } catch (const std::exception &error) {
        std::lock_guard lock(error_mutex_);
        error_ = error.what();
    } catch (...) {
        std::lock_guard lock(error_mutex_);
        error_ = "Video decoder failed";
    }
    if (codec) {
        (void)AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
    }
    running_.store(false);
}
} // namespace pocketvr::android
