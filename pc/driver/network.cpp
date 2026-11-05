// SPDX-License-Identifier: Apache-2.0
#include "network.hpp"
#include "encoder/pipeline.hpp"
#include "identity_store.hpp"
#include "mdns.hpp"
#include <pocketvr/discovery.hpp>
#include <pocketvr/session.hpp>
#include <pocketvr/reassembler.hpp>
#include <pocketvr/media_queue.hpp>
#include <pocketvr/telemetry.hpp>
#include <pocketvr/adaptive.hpp>
#include <pocketvr/latency_log.hpp>
#include <pocketvr/timing.hpp>
#include <pocketvr/stream_format.hpp>
#include <spdlog/spdlog.h>
#include <sodium.h>
#include <deque>
#include <fstream>

namespace pocketvr::driver {
namespace {
void network_log(spdlog::level::level_enum level, const char *operation,
                 const char *message) noexcept {
    try {
        spdlog::log(level, "{}: {}", operation, message);
    } catch (...) {
    }
}
struct ClockReply {
    Endpoint destination{};
    uint64_t phone_send{}, pc_receive{};
};
class Connection final : public std::enable_shared_from_this<Connection> {
    struct Frame {
        EncodedFrame encoded;
        wire::VideoHeader header;
    };
    State &state_;
    std::shared_ptr<UdpTransport> udp_;
    std::atomic<std::shared_ptr<ITransport>> transport_;
    bool tcp_mode_{}, wired_link_{};
    Worker tcp_receive_;
    Endpoint control_peer_;
    PacketCipher receiver_, sender_;
    uint32_t session_;
    Key identity_{};
    std::atomic<uint64_t> peer_{};
    std::mutex queue_mutex_;
    std::deque<Frame> frames_;
    size_t queued_bytes_{};
    SpscQueue<ClockReply, 32> clocks_;
    std::atomic<std::shared_ptr<EncoderPipeline>> pipeline_;
    mutable std::mutex pipeline_snapshot_mutex_;
    PipelineStats retired_pipeline_{};
    Gpu *gpu_{};
    Compositor *compositor_{};
    EncoderConfig encoder_config_{};
    bool software_only_{};
    std::atomic<bool> video_enabled_{true};
    std::atomic<uint64_t> video_boundary_{};
    StreamFormat current_format_{}, pending_format_{};
    std::optional<StreamFormatController> format_control_;
    enum class FormatPhase { stable, preparing, committing, restoring };
    FormatPhase format_phase_{FormatPhase::stable};
    uint64_t format_revision_{}, format_deadline_{}, format_changes_{}, format_reset_baseline_{};
    bool format_supported_{}, format_blocked_{}, format_phone_ready_{};
    bool format_rejected_{};
    std::string format_reason_{"peer did not negotiate format changes"};
    Worker transmit_;
    win::Handle pace_timer_{CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)};
    std::atomic<unsigned> send_spread_pct_{};
    std::atomic<uint64_t> frame_interval_ns_{16'666'667};
    mutable std::mutex latency_mutex_;
    LatencyJoin latency_;
    std::ofstream latency_file_;
    uint64_t latency_file_bytes_{};
    std::atomic<bool> latency_log_{};
    win::Handle transmit_wake_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::atomic<bool> accepting_{true};
    KeyframeRequestGate keyframe_requests_;
    std::atomic<unsigned> parity_pct_{10};
    std::atomic<uint64_t> bytes_{}, packets_{}, dropped_{}, last_pose_{}, last_clock_{};
    bool have_sequence_{};
    uint32_t last_sequence_{};
    bool have_input_sequence_{};
    uint32_t last_input_sequence_{};
    PhoneStatsInbox phone_stats_;
    mutable std::mutex adaptive_mutex_;
    PacingPll pacing_;
    double pacing_lead_ms_{};
    std::optional<StreamBitrateController> bitrate_control_;
    std::optional<StreamFecController> fec_control_;
    std::array<unsigned, 2> fec_settings_{};
    uint64_t fec_adjustments_{};
    std::string fec_reason_{"waiting for fresh FEC feedback"};
    std::optional<StreamLoadGuard> load_guard_;
    ThermalInbox thermal_;
    bool thermal_supported_{}, guard_enabled_{true};
    std::string guard_reason_{"normal"};
    void update_guard(uint64_t now) {
        if (!pipeline_.load() || !load_guard_) return;
        const auto stats = pipeline_.load()->stats();
        const bool fresh = stats.encode_samples >= 20 && stats.encode_sample_ns &&
            now >= stats.encode_sample_ns && now - stats.encode_sample_ns <= 3'000'000'000;
        const auto decision = load_guard_->update(now / 1'000'000, guard_enabled_,
            {thermal_.level(), thermal_.fresh(now)}, stats.encode_p95_ms, fresh);
        const auto fps = format_phase_ == FormatPhase::stable ? current_format_.fps : 0U;
        pipeline_.load()->input_fps_limit(std::min(decision.fps, fps)); guard_reason_ = decision.reason;
    }
    std::array<uint32_t, 3> bitrate_settings_{};
    bool adaptive_enabled_{};
    uint64_t feedback_sequence_{}, adjustments_{};
    std::string bitrate_reason_{"waiting for phone feedback"}, last_change_reason_;
    static uint64_t packed(Endpoint peer) {
        return (static_cast<uint64_t>(peer.address) << 16) | peer.port;
    }
    static Endpoint unpacked(uint64_t peer) {
        return {static_cast<uint32_t>(peer >> 16), static_cast<uint16_t>(peer)};
    }
    bool send(wire::Type type, std::span<const uint8_t> payload, Endpoint destination) {
        std::array<uint8_t, wire::max_datagram> packet{};
        const auto count = sender_.seal_into(type, 0, monotonic_ns(), payload, packet);
        if (!count || !transport_.load()->send(std::span(packet).first(*count), destination))
            return false;
        bytes_.fetch_add(*count);
        packets_.fetch_add(1);
        return true;
    }
    // Windows sleeps too coarsely for this without a high-resolution timer. Packets
    // due within a fraction of a millisecond simply leave together.
    void wait_until(uint64_t due) const {
        const auto now = monotonic_ns();
        if (!pace_timer_ || due <= now + 300'000)
            return;
        LARGE_INTEGER delay{};
        delay.QuadPart = -static_cast<LONGLONG>((due - now) / 100);
        if (SetWaitableTimer(pace_timer_.get(), &delay, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(pace_timer_.get(), 20);
    }
    void transmit(StopToken stop) noexcept {
        raise_thread_priority();
        try {
            while (!stop.stop_requested()) {
                TimedHaptic event;
                while (state_.haptics.pop(event)) {
                    const auto now=monotonic_ns();
                    const auto peer=peer_.load();
                    if (state_.haptic_owner.load()==2&&peer&&now>=event.timestamp_ns&&now-event.timestamp_ns<100'000'000) {
                        std::array<uint8_t,haptic_packet_size> payload{};
                        if (encode_haptic(event.pulse,payload)) (void)send(wire::Type::haptic,payload,unpacked(peer));
                    }
                }
                ClockReply reply;
                while (clocks_.pop(reply)) {
                    std::array<uint8_t, 24> payload{};
                    wire::Writer writer(payload);
                    writer.u64(reply.phone_send);
                    writer.u64(reply.pc_receive);
                    writer.u64(monotonic_ns());
                    (void)send(wire::Type::pong, payload, reply.destination);
                }
                std::optional<Frame> frame;
                {
                    std::lock_guard lock(queue_mutex_);
                    if (!frames_.empty()) {
                        frame = std::move(frames_.front());
                        queued_bytes_ -= frame->encoded.bytes.size();
                        frames_.pop_front();
                    }
                }
                if (!frame) {
                    // A nominal 1 ms Sleep can consume a Windows timer tick.
                    // Wake on encoded video/clock work rather than polling it.
                    WaitForSingleObject(transmit_wake_.get(), 20);
                    continue;
                }
                const auto destination = peer_.load();
                if (!video_enabled_.load() || frame->header.render_ns < video_boundary_.load() ||
                    !destination || monotonic_ns() - frame->header.render_ns > 100'000'000) {
                    dropped_.fetch_add(1);
                    pipeline_.load()->request_idr();
                    continue;
                }
                const auto data_packets =
                    (frame->encoded.bytes.size() + stream_shard_bytes - 1) / stream_shard_bytes;
                const auto groups = (data_packets + stream_group_k - 1) / stream_group_k;
                const auto parity = stream_parity_shards(data_packets, parity_pct_.load());
                const auto send_start = monotonic_ns();
                const auto spread = wired_link_ ? 0U : send_spread_pct_.load();
                const auto total = static_cast<uint32_t>(data_packets + groups * parity);
                const auto interval = frame_interval_ns_.load();
                // Packets in an average frame at the current bitrate, parity included.
                const auto typical = static_cast<uint32_t>(
                    static_cast<uint64_t>(pipeline_.load()->stats().accepted_bitrate) / 8 * interval /
                    1'000'000'000 / stream_shard_bytes * (100 + parity_pct_.load()) / 100);
                uint32_t packet_index = 0;
                size_t unsent = 0;
                const bool sent = packetize_frame(
                    frame->encoded.bytes, frame->header, parity,
                    [&](const auto &header, auto shard) {
                        if (stop.stop_requested() ||
                            !video_enabled_.load() ||
                            monotonic_ns() - frame->header.render_ns > 100'000'000)
                            return false;
                        wait_until(paced_send_ns(send_start, packet_index++, total, interval, spread, typical));
                        std::array<uint8_t, wire::video_header_size + stream_shard_bytes> payload{};
                        if (!wire::encode_video_header(header, payload))
                            return false;
                        std::copy(shard.begin(), shard.end(),
                                  payload.begin() + wire::video_header_size);
                        if (send(header.shard < header.k ? wire::Type::video : wire::Type::fec,
                                 payload, unpacked(destination)))
                            return true;
                        // On UDP this is one lost packet, which parity can still repair.
                        // Giving up would cost a keyframe, the largest frame there is.
                        return !tcp_mode_ && ++unsent <= groups * parity;
                    });
                if (!sent) {
                    dropped_.fetch_add(1);
                    pipeline_.load()->request_idr();
                } else {
                    const auto send_end = monotonic_ns();
                    state_.last_stream_ns.store(send_end);
                    std::lock_guard lock(latency_mutex_);
                    latency_.add_pc({frame->header.frame_id, frame->header.render_ns, send_start, send_end,
                                     static_cast<uint32_t>(frame->encoded.bytes.size()), frame->encoded.encode_ms});
                }
            }
        } catch (const std::exception &e) {
            network_log(spdlog::level::err, "Session transmit stopped", e.what());
            accepting_.store(false);
        } catch (...) {
            accepting_.store(false);
        }
    }

  public:
    void initial_thermal(bool supported, int status) {
        thermal_supported_ = supported;
        if (supported) thermal_.initial(status, monotonic_ns());
    }
    void thermal(const nlohmann::json &message) {
        std::lock_guard lock(adaptive_mutex_);
        if (thermal_supported_) (void)thermal_.accept(message, monotonic_ns());
    }
    Connection(State &state, std::shared_ptr<UdpTransport> udp, Endpoint peer,
               const SessionMaterial &material, bool tcp_mode, bool wired_link)
        : state_(state), udp_(std::move(udp)), transport_(std::shared_ptr<ITransport>(udp_)),
          tcp_mode_(tcp_mode), wired_link_(wired_link), control_peer_(peer), receiver_(material.id, material.media),
          sender_(material.id, material.media), session_(material.id),
          identity_(material.peer_identity) {
        if (!transmit_wake_)
            throw std::runtime_error("Transmit wake event allocation failed");
    }
    const Key &identity() const noexcept {
        return identity_;
    }
    ~Connection() {
        close();
        pipeline_.store(nullptr);
    }
    void close() {
        accepting_.store(false);
        state_.phone_input.release();
        tcp_receive_.request_stop();
        transmit_.request_stop();
        SetEvent(transmit_wake_.get());
        if (tcp_receive_.joinable())
            tcp_receive_.join();
        if (transmit_.joinable())
            transmit_.join();
        if (pipeline_.load())
            pipeline_.load()->stop();
        state_.phone_input.release();
    }
    void start(Gpu &gpu, Compositor &compositor, EncoderConfig config, bool software_only,
               const Settings &settings) {
        gpu_ = &gpu; compositor_ = &compositor; encoder_config_ = config; software_only_ = software_only;
        current_format_ = pending_format_ = {config.width, config.height, config.fps};
        load_guard_.emplace(config.fps);
        frame_interval_ns_.store(1'000'000'000ULL / config.fps);
        bitrate_settings(settings);
        config.bitrate = bitrate_control_->bitrate();
        encoder_config_.bitrate = config.bitrate;
        restart_pipeline(current_format_);
        transmit_ = Worker([this](StopToken stop) { transmit(stop); });
        update_guard(monotonic_ns());
    }
    void restart_pipeline(StreamFormat format) {
        const auto old = pipeline_.load();
        if (old) old->stop();
        {
            std::lock_guard lock(queue_mutex_); frames_.clear(); queued_bytes_ = 0;
        }
        auto config = encoder_config_;
        config.width = format.width; config.height = format.height; config.fps = format.fps;
        config.bitrate = bitrate_control_->bitrate();
        std::weak_ptr<Connection> weak = shared_from_this();
        auto next = EncoderPipeline::start(
            *compositor_,
            [gpu = gpu_, config, software = software_only_] {
                return scale_encoder(*gpu, config, create_encoder(*gpu, config, software));
            },
            [weak](EncodedFrame &&encoded, const wire::VideoHeader &header) {
                const auto connection = weak.lock();
                if (!connection || !connection->accepting_.load() || !connection->peer_.load() ||
                    !connection->video_enabled_.load() || header.render_ns < connection->video_boundary_.load())
                    return false;
                std::unique_lock lock(connection->queue_mutex_, std::try_to_lock);
                if (!lock.owns_lock() || !connection->video_enabled_.load() || connection->frames_.size() >= 2 || encoded.bytes.empty() ||
                    encoded.bytes.size() > 8 * 1024 * 1024 ||
                    connection->queued_bytes_ + encoded.bytes.size() > 16 * 1024 * 1024)
                    return false;
                const auto size = encoded.bytes.size();
                connection->frames_.push_back({std::move(encoded), header});
                connection->queued_bytes_ += size;
                SetEvent(connection->transmit_wake_.get());
                return true;
            },
            config.bitrate,
            [weak, codec = config.codec](const EncodedFrame &frame) noexcept {
                if (const auto connection = weak.lock())
                    connection->state_.encoded_dump.record(frame.bytes, frame.idr, codec);
            });
        next->input_fps_limit(format_phase_ == FormatPhase::stable ? current_format_.fps : 0U);
        std::lock_guard snapshot_lock(pipeline_snapshot_mutex_);
        if (old) {
            const auto stats = old->stats();
            retired_pipeline_.encoded += stats.encoded; retired_pipeline_.dropped += stats.dropped;
            retired_pipeline_.resets += stats.resets; retired_pipeline_.guard_skipped += stats.guard_skipped;
        }
        pipeline_.store(std::move(next));
    }
    bool matches(uint32_t id, Endpoint source, const UdpTransport *transport) const {
        return !tcp_mode_ && id == session_ && source.address == control_peer_.address &&
               transport == udp_.get();
    }
    bool media_alive() const {
        return transport_.load()->alive();
    }
    void attach_tcp(TcpListener &listener, StopToken stop) {
        const auto deadline = monotonic_ns() + 5'000'000'000;
        while (!stop.stop_requested() && monotonic_ns() < deadline) {
            auto stream = listener.accept(std::chrono::milliseconds(20));
            if (!stream || stream->peer().address != control_peer_.address)
                continue;
            network_log(spdlog::level::info, "USB media setup", "Loopback socket accepted");
            auto transport = std::make_shared<TcpTransport>(std::move(stream));
            std::array<uint8_t, wire::max_datagram> bytes{};
            Endpoint source;
            std::optional<size_t> count;
            while (!stop.stop_requested() && transport->alive() && monotonic_ns() < deadline && !count)
                count = transport->receive(bytes, source, std::chrono::milliseconds(100));
            if (!count) {
                network_log(spdlog::level::warn, "USB media setup", "Initial record unavailable before deadline");
                continue;
            }
            transport_.store(transport);
            receive(std::span(bytes).first(*count), source);
            // Only a correctly authenticated initial ping may bind this socket.
            if (!peer_.load()) {
                network_log(spdlog::level::warn, "USB media setup", "Initial record did not authenticate as a ping");
                continue;
            }
            tcp_receive_ = Worker([this, transport](StopToken token) {
                try {
                    std::array<uint8_t, wire::max_datagram> packet{};
                    while (!token.stop_requested() && transport->alive()) {
                        Endpoint sender;
                        if (const auto size =
                                transport->receive(packet, sender, std::chrono::milliseconds(1)))
                            receive(std::span(packet).first(*size), sender);
                    }
                } catch (const std::exception &error) {
                    network_log(spdlog::level::warn, "USB media receiver stopped", error.what());
                    transport->interrupt();
                }
            });
            return;
        }
        throw std::runtime_error("USB media socket did not authenticate in time");
    }
    // Called only by the server's single UDP receiver thread.
    void receive(std::span<const uint8_t> packet, Endpoint source) {
        if (!accepting_.load())
            return;
        std::array<uint8_t, wire::max_payload> plain{};
        wire::Header header;
        const auto size = receiver_.open_into(packet, header, plain);
        if (!size || header.device != 0)
            return;
        const auto payload = std::span(plain).first(*size);
        const auto now = monotonic_ns();
        if (header.type == wire::Type::ping && payload.size() == 8) {
            if (now - last_clock_.load() < 40'000'000)
                return;
            wire::Reader reader(payload);
            uint64_t phone_time{};
            reader.u64(phone_time);
            peer_.store(packed(source));
            last_clock_.store(now);
            if (clocks_.push({source, phone_time, now}))
                SetEvent(transmit_wake_.get());
        } else if (header.type == wire::Type::keyframe && peer_.load() == packed(source)) {
            phone_keyframe_request();
        } else if (header.type == wire::Type::input && peer_.load()==packed(source)) {
            const auto input=decode_input(payload);
            if (!input || (input->timestamp_ns>now&&input->timestamp_ns-now>20'000'000) ||
                (now>=input->timestamp_ns&&now-input->timestamp_ns>100'000'000) ||
                (have_input_sequence_ && (input->sequence==last_input_sequence_ ||
                 static_cast<uint32_t>(input->sequence-last_input_sequence_)>UINT32_MAX/2))) return;
            have_input_sequence_=true; last_input_sequence_=input->sequence;
            state_.phone_controls(*input,now);
        } else if (header.type == wire::Type::pose && peer_.load() == packed(source)) {
            const auto pose = wire::decode_pose(payload);
            if (!pose || (pose->sensor_ns > now && pose->sensor_ns - now > 100'000'000) ||
                (pose->sensor_ns <= now && now - pose->sensor_ns > 1'000'000'000))
                return;
            if (have_sequence_ &&
                (pose->sequence == last_sequence_ ||
                 static_cast<uint32_t>(pose->sequence - last_sequence_) > UINT32_MAX / 2))
                return;
            have_sequence_ = true;
            last_sequence_ = pose->sequence;
            state_.head.publish({pose->pose, pose->sensor_ns, pose->tracking});
            state_.pose_sequence.fetch_add(1);
            state_.pose_sequence.notify_one();
            last_pose_.store(now);
        }
    }
    void idr() {
        if (pipeline_.load())
            pipeline_.load()->request_idr();
    }
    void phone_keyframe_request() {
        if (keyframe_requests_.accept(monotonic_ns()))
            idr();
    }
    PipelineStats encoder_stats() const {
        std::lock_guard lock(pipeline_snapshot_mutex_);
        auto stats = pipeline_.load() ? pipeline_.load()->stats() : PipelineStats{};
        stats.encoded += retired_pipeline_.encoded; stats.dropped += retired_pipeline_.dropped;
        stats.resets += retired_pipeline_.resets; stats.guard_skipped += retired_pipeline_.guard_skipped;
        return stats;
    }
    void format_capabilities(std::vector<StreamFormat> ladder, bool supported) {
        std::lock_guard lock(adaptive_mutex_);
        format_supported_ = supported && ladder.size() > 1;
        format_control_.emplace(std::move(ladder));
        if (format_supported_) format_reason_ = "waiting for fresh format feedback";
    }
    void format_reply(const nlohmann::json &message) {
        std::lock_guard lock(adaptive_mutex_);
        if (!message.contains("request_id") || !message.at("request_id").is_number_integer() ||
            message.at("request_id") != format_revision_ || format_phase_ == FormatPhase::stable) return;
        const auto type = message.value("type", std::string{});
        if (type == "stream.rejected" && format_phase_ != FormatPhase::restoring) format_rejected_ = true;
        if ((type == "stream.prepared" && format_phase_ == FormatPhase::preparing) ||
            (type == "stream.committed" && format_phase_ == FormatPhase::committing) ||
            (type == "stream.restored" && format_phase_ == FormatPhase::restoring)) format_phone_ready_ = true;
    }
    void format_tick(ControlChannel &channel, uint64_t now) {
        std::lock_guard lock(adaptive_mutex_);
        if (!format_supported_ || !format_control_) return;
        const auto stage = [&](const char *type, StreamFormat format) {
            video_enabled_.store(false); format_phone_ready_ = format_rejected_ = false;
            video_boundary_.store(now); format_deadline_ = now + 5'000'000'000;
            restart_pipeline(format);
            format_reset_baseline_ = retired_pipeline_.resets;
            channel.send({{"type", type}, {"request_id", format_revision_},
                {"render_boundary_ns", now}, {"format", stream_format_json(format)}});
            try { spdlog::info("Video {} request {}: {}x{} at {} fps: {}", type,
                format_revision_, format.width, format.height, format.fps, format_reason_); } catch (...) {}
        };
        if (format_phase_ != FormatPhase::stable) {
            const auto encoder = encoder_stats();
            if (format_rejected_ || now > format_deadline_ || encoder.resets > format_reset_baseline_) {
                if (format_phase_ == FormatPhase::restoring)
                    throw std::runtime_error("Video format rollback failed; stream remains gated");
                format_phase_ = FormatPhase::restoring; format_blocked_ = true;
                format_reason_ = "format change failed; restoring previous format; automatic changes disabled for this session";
                stage("stream.abort", current_format_); return;
            }
            if (!encoder.ready || !format_phone_ready_) return;
            if (format_phase_ == FormatPhase::preparing) {
                format_phase_ = FormatPhase::committing; format_phone_ready_ = false;
                format_deadline_ = now + 5'000'000'000;
                channel.send({{"type", "stream.commit"}, {"request_id", format_revision_}}); return;
            }
            if (format_phase_ == FormatPhase::committing) {
                current_format_ = pending_format_; ++format_changes_;
                format_reason_ = "negotiated format applied";
            }
            format_phase_ = FormatPhase::stable; format_control_->settled(now / 1'000'000);
            pipeline_.load()->request_idr(); update_guard(now); video_enabled_.store(true); return;
        }
        if (format_blocked_) return;
        const auto dump = state_.encoded_dump.status();
        if (dump.at("state") == "recording" || dump.at("state") == "waiting for keyframe") {
            format_control_->settled(now / 1'000'000);
            format_reason_ = "holding format during bounded diagnostic recording"; return;
        }
        const auto report = phone_stats_.status(now);
        FormatFeedback feedback;
        feedback.fresh = report.at("available").get<bool>();
        if (feedback.fresh) {
            const auto &v = report.at("values");
            feedback.sequence = report.at("seq").get<uint64_t>();
            feedback.clock_ready = v.at("clock_ready").get<bool>();
            feedback.decoded = v.at("decoder_decoded").get<uint64_t>();
            feedback.accepted = v.at("accepted_frames").get<uint64_t>();
            feedback.drops = v.at("reassembly_dropped_frames").get<uint64_t>();
            feedback.decoder_drops = v.at("decoder_dropped").get<uint64_t>() +
                v.at("sink_rejected_frames").get<uint64_t>();
            feedback.late = v.at("missed_vsync").get<uint64_t>();
            feedback.decode_p95_ms = v.at("decode_samples").get<uint64_t>() >= 20 ?
                v.at("decode_p95_ms").get<double>() : 0.;
            feedback.decode_fresh = v.at("decode_samples").get<uint64_t>() >= 20;
        }
        const auto encoder = encoder_stats();
        feedback.encode_fresh = encoder.encode_samples >= 20 && encoder.encode_sample_ns &&
            now >= encoder.encode_sample_ns && now - encoder.encode_sample_ns <= 3'000'000'000;
        feedback.encode_p95_ms = encoder.encode_p95_ms;
        feedback.bitrate_floor = encoder.accepted_bitrate && encoder.accepted_bitrate <= bitrate_settings_[1];
        feedback.thermal = thermal_.fresh(now) ? thermal_.level() : -1;
        feedback.recovery_allowed = !thermal_supported_ || thermal_.fresh(now) || thermal_.level() < 2;
        const auto next = format_control_->update(now / 1'000'000, current_format_, adaptive_enabled_, feedback);
        format_reason_ = format_control_->reason;
        if (next) {
            pending_format_ = *next; ++format_revision_; format_phase_ = FormatPhase::preparing;
            stage("stream.prepare", pending_format_);
        }
    }
    nlohmann::json status() const {
        const auto stats = encoder_stats();
        return {{"authenticated", true},
                {"udp_peer_verified", !tcp_mode_ && peer_.load() != 0},
                {"media_peer_verified", peer_.load() != 0},
                {"transport", tcp_mode_ ? "tcp" : "udp"},
                {"wired_link", wired_link_},
                {"send_spread_pct", wired_link_ ? 0U : send_spread_pct_.load()},
                {"encoded_frames", stats.encoded},
                {"encoder_resets", stats.resets},
                {"encoder_ready", stats.ready},
                {"encoder_backend", encoder_backend_name(stats.backend)},
                {"hardware_encoder", stats.hardware},
                {"dropped_frames", stats.dropped + dropped_.load()},
                {"sent_bytes", bytes_.load()},
                {"sent_packets", packets_.load()},
                {"last_pose_pc_ns", last_pose_.load()},
                {"phone_stats", phone_stats_.status(monotonic_ns())},
                {"latency", latency_summary()},
                {"adaptation", bitrate_status()}};
    }
    nlohmann::json latency_summary() const {
        std::lock_guard lock(latency_mutex_);
        return latency_.summary();
    }
    void latency_samples(const nlohmann::json &message) {
        const auto &samples = message.at("samples");
        if (!samples.is_array() || samples.size() > 240)
            throw std::invalid_argument("Invalid latency report");
        // The send thread takes this lock for every frame, so disk writes happen after it.
        std::vector<FrameLatency> joined;
        {
            std::lock_guard lock(latency_mutex_);
            for (const auto &item : samples) {
                if (!item.is_array() || item.size() != 5)
                    throw std::invalid_argument("Invalid latency sample");
                const auto frame = latency_.add_phone({item[0].get<uint64_t>(), item[1].get<uint64_t>(), item[2].get<uint64_t>(),
                                                       item[3].get<uint64_t>(), item[4].get<uint64_t>()});
                if (frame && latency_log_.load())
                    joined.push_back(*frame);
            }
        }
        for (const auto &frame : joined)
            log_latency(frame);
    }
    void log_latency(const FrameLatency &frame) {
        // Capped, so a forgotten setting cannot fill the disk.
        constexpr uint64_t limit = 32 * 1024 * 1024;
        if (latency_file_bytes_ > limit)
            return;
        if (!latency_file_.is_open()) {
            const auto path = win::appdata(true) / "logs" / "latency.csv";
            latency_file_bytes_ = prepare_latency_file(path, limit);
            latency_file_.open(path, std::ios::app);
            if (!latency_file_bytes_)
                latency_file_ << latency_csv_header();
        }
        const auto row = latency_csv_row(frame);
        latency_file_ << row;
        latency_file_bytes_ += row.size();
    }
    nlohmann::json bitrate_status() const {
        std::lock_guard lock(adaptive_mutex_);
        const auto stats = encoder_stats();
        return {{"enabled", adaptive_enabled_},
                {"requested_bitrate_bps", stats.requested_bitrate},
                {"encoder_accepted_bitrate_bps", stats.accepted_bitrate},
                {"bitrate_rejected", stats.bitrate_rejected},
                {"minimum_bitrate_bps", bitrate_settings_[1]},
                {"maximum_bitrate_bps", bitrate_settings_[2]},
                {"adjustments", adjustments_},
                {"pacing", {{"arrival_lead_ms", pacing_lead_ms_}, {"phase_ms", pacing_.phase_ms()}}},
                {"reason", bitrate_reason_},
                {"last_change_reason", last_change_reason_},
                {"format", {{"supported", format_supported_}, {"blocked", format_blocked_},
                    {"changing", format_phase_ != FormatPhase::stable}, {"changes", format_changes_},
                    {"request_id", format_revision_}, {"current", stream_format_json(current_format_)},
                    {"requested", stream_format_json(pending_format_)}, {"reason", format_reason_}}},
                {"fec", {{"adaptive", fec_settings_[1] != 0}, {"floor_pct", fec_settings_[0]},
                    {"requested_ratio_pct", parity_pct_.load()}, {"changes", fec_adjustments_},
                    {"tcp_bypass", tcp_mode_}, {"reason", fec_reason_}}},
                {"guard", {{"enabled", guard_enabled_}, {"input_fps_cap", stats.input_fps_limit},
                    {"skipped_frames", stats.guard_skipped}, {"encode_p95_ms", stats.encode_p95_ms},
                    {"encode_samples", stats.encode_samples}, {"thermal_supported", thermal_supported_},
                    {"thermal_status", thermal_.level()}, {"thermal_fresh", thermal_.fresh(monotonic_ns())},
                    {"thermal_rejected", thermal_.rejected()}, {"reason", guard_reason_}}}};
    }
    void bitrate_settings(const Settings &settings) {
        send_spread_pct_.store(settings.get<unsigned>("video.send_spread_pct"));
        latency_log_.store(settings.get<bool>("debug.latency_log"));
        if (pipeline_.load() && state_.encoded_dump.waiting()) pipeline_.load()->request_idr();
        std::lock_guard lock(adaptive_mutex_);
        guard_enabled_ = settings.get<bool>("power.thermal_guard");
        // A cable rarely loses packets, so automatic FEC starts at zero there and only
        // rises if frames really are lost. A fixed ratio is an explicit user choice.
        const bool automatic_fec = !tcp_mode_ && settings.get<bool>("fec.adaptive");
        const std::array<unsigned, 2> fec_settings{
            tcp_mode_ || (wired_link_ && automatic_fec) ? 0U : settings.get<unsigned>("fec.ratio_pct"),
            automatic_fec ? 1U : 0U};
        if (!fec_control_ || fec_settings != fec_settings_) {
            fec_settings_ = fec_settings;
            fec_control_.emplace(fec_settings[0], fec_settings[1] != 0);
            parity_pct_.store(fec_control_->ratio());
            fec_reason_ = tcp_mode_ ? "TCP bypasses redundant FEC" : fec_settings[1] ?
                "waiting for fresh FEC feedback" : "fixed FEC setting";
        }
        const auto bits = [&](const char *key) {
            return static_cast<uint32_t>(settings.get<double>(key) * 1'000'000);
        };
        const std::array values{bits("video.bitrate_mbps"), bits("video.min_bitrate_mbps"),
                                bits("video.max_bitrate_mbps")};
        const bool enabled = settings.get<bool>("video.adaptive");
        if (bitrate_control_ && values == bitrate_settings_ && enabled == adaptive_enabled_)
            return;
        bitrate_settings_ = values;
        adaptive_enabled_ = enabled;
        bitrate_control_.emplace(values[0], values[1], values[2], enabled);
        feedback_sequence_ = 0;
        bitrate_reason_ = enabled ? "waiting for fresh phone feedback" : "fixed quality";
        if (pipeline_.load())
            pipeline_.load()->bitrate(bitrate_control_->bitrate());
    }
    void adapt(uint64_t now_ns) {
        std::lock_guard lock(adaptive_mutex_);
        update_guard(now_ns);
        if (!bitrate_control_ || !pipeline_.load())
            return;
        const auto latest = phone_stats_.status(now_ns);
        std::optional<BitrateFeedback> feedback;
        std::optional<FecFeedback> fec_feedback;
        if (latest.at("available").get<bool>()) {
            const auto seq = latest.at("seq").get<uint64_t>();
            if (seq == feedback_sequence_)
                return;
            feedback_sequence_ = seq;
            const auto &v = latest.at("values");
            fec_feedback = FecFeedback{seq, v.at("accepted_frames").get<uint64_t>(),
                v.at("reassembly_dropped_frames").get<uint64_t>(),
                v.at("fec_recovered_groups").get<uint64_t>(), v.at("clock_ready").get<bool>()};
            feedback = BitrateFeedback{seq,
                                       v.at("accepted_frames").get<uint64_t>(),
                                       v.at("reassembly_dropped_frames").get<uint64_t>(),
                                       v.at("sink_rejected_frames").get<uint64_t>(),
                                       v.at("decoder_decoded").get<uint64_t>(),
                                       v.at("decoder_dropped").get<uint64_t>(),
                                       v.at("missed_vsync").get<uint64_t>(),
                                       v.at("clock_ready").get<bool>()};
        } else
            feedback_sequence_ = 0;
        if (fec_control_) {
            const auto fec = fec_control_->update(now_ns / 1'000'000, fec_feedback ? &*fec_feedback : nullptr);
            if (!tcp_mode_) fec_reason_ = StreamFecController::reason(fec.reason);
            if (fec.changed) { parity_pct_.store(fec.ratio); ++fec_adjustments_; }
        }
        if (pipeline_.load()->stats().bitrate_rejected) {
            bitrate_reason_ = "encoder rejected live updates; reconnect to retry";
            return; // FEC adaptation above remains independent of codec tuning.
        }
        const auto decision =
            bitrate_control_->update(now_ns / 1'000'000, feedback ? &*feedback : nullptr);
        bitrate_reason_ = StreamBitrateController::reason(decision.reason);
        if (decision.changed) {
            pipeline_.load()->bitrate(decision.bits_per_second);
            ++adjustments_;
            last_change_reason_ = bitrate_reason_;
            spdlog::info("Adaptive bitrate requested {} bps: {}", decision.bits_per_second,
                         bitrate_reason_);
        }
    }
    void phone_stats(const nlohmann::json &message) {
        if (!phone_stats_.accept(message, monotonic_ns()))
            return;
        // Lock PC vsync to the phone's display so each frame lands just before its
        // refresh. Only at 60 fps with a stable format: the lead means nothing else.
        const auto &values = message.at("values");
        std::lock_guard lock(adaptive_mutex_);
        if (values.at("arrival_samples").get<uint64_t>() < 30 || format_phase_ != FormatPhase::stable ||
            current_format_.fps != 60) {
            state_.pacing_pending_ns.store(0);
            return;
        }
        pacing_lead_ms_ = values.at("arrival_lead_ms").get<double>();
        const auto step = pacing_.report(pacing_lead_ms_, state_.target_lead_ms.load());
        state_.pacing_pending_ns.store(static_cast<int64_t>(step * 1e6));
    }
};
} // namespace
class NetworkServer::Impl {
    struct Listener {
        std::unique_ptr<TcpListener> control;
        std::shared_ptr<UdpTransport> media;
        std::unique_ptr<TcpListener> tcp_media;
        std::unique_ptr<win::MdnsAdvertiser> mdns;
        uint64_t last_discovery{};
    };
    State &state_;
    Gpu &gpu_;
    Compositor &compositor_;
    NetworkConfig config_;
    win::IdentityStore identities_;
    std::vector<Listener> listeners_;
    mutable std::mutex connection_mutex_, pairing_mutex_;
    std::shared_ptr<Connection> active_;
    // One slot runs the live session while the other lets a newcomer prove itself,
    // so a connection that has not been accepted can never disturb the session.
    struct ControlSlot {
        Worker worker;
        std::shared_ptr<TcpStream> stream; // Guarded by connection_mutex_.
        std::atomic<bool> done{true};
    };
    std::array<ControlSlot, 2> control_slots_;
    ControlSlot *session_slot_{}; // Guarded by connection_mutex_.
    std::atomic<bool> session_open_{false};
    Worker accept_, receive_;
    std::atomic<uint64_t> pairing_revision_{};
    mutable std::mutex approval_mutex_;
    nlohmann::json pending_phone_;
    int approval_{};
    uint64_t approval_request_{}; // Only the newest request is on screen.
    std::atomic<uint64_t> desktop_seen_ns_{};
    bool desktop_open() const {
        const auto seen = desktop_seen_ns_.load();
        return seen && monotonic_ns() - seen < 10'000'000'000;
    }
    // The newest accepted phone wins. A phone that lost its link comes back before
    // its old session times out, and must not be refused for the ten seconds that
    // session takes to expire. Throws if this connection cannot take over.
    bool approved_by_owner(const Key &key, TcpStream &stream, StopToken stop) {
        uint64_t request{};
        {
            const auto hex = win::key_hex(key);
            std::lock_guard lock(approval_mutex_);
            pending_phone_ = {{"id", hex.substr(0, 8)}, {"key", hex}, {"ip", stream.peer().ip()}};
            approval_ = 0;
            request = ++approval_request_;
        }
        const auto deadline = monotonic_ns() + 60'000'000'000;
        int answer = 0;
        while (!answer && !stop.stop_requested() && stream.alive() && monotonic_ns() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::lock_guard lock(approval_mutex_);
            if (approval_request_ != request)
                return false; // Another phone took the prompt; this one can try again.
            answer = approval_;
        }
        std::lock_guard lock(approval_mutex_);
        if (approval_request_ == request)
            pending_phone_ = nullptr;
        return answer > 0;
    }
    void take_session(ControlSlot &slot, TcpStream &stream, StopToken stop) {
        {
            std::lock_guard lock(connection_mutex_);
            if (!stream.alive())
                throw std::runtime_error("Connection replaced during setup");
            if (session_slot_ && session_slot_->stream) {
                network_log(spdlog::level::info, "Phone session ended",
                            "Replaced by a newly accepted connection");
                session_slot_->stream->interrupt();
            }
            session_slot_ = &slot;
        }
        const auto deadline = monotonic_ns() + 3'000'000'000;
        for (bool open = false; !session_open_.compare_exchange_strong(open, true); open = false) {
            if (stop.stop_requested() || !stream.alive() || monotonic_ns() >= deadline)
                throw std::runtime_error("Previous phone session did not end in time");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    void control(StopToken stop, ControlSlot &slot, std::shared_ptr<TcpStream> stream,
                 std::shared_ptr<UdpTransport> media, TcpListener *tcp_media) noexcept {
        const auto revision = pairing_revision_.load();
        bool owns_session = false;
        try {
            PairingPolicy policy;
            policy.allow_new = true;
            policy.lookup_identity = [&](const Key &key) -> std::optional<Key> {
                for (const auto &peer : identities_.peers()) {
                    const auto stored = win::parse_key(peer.at("identity"));
                    if (stored == key) return stored;
                }
                return {};
            };
            // No code: while the PC app is open the first phone is trusted and remembered,
            // where its owner can see it. Other new phones need one click in that app,
            // unless they arrive over the USB cable.
            // The handshake still signs both identities and derives fresh session keys.
            policy.approve = [&](const Key &key, StopToken token) {
                const bool loopback = (stream->peer().address & 0xff000000) == 0x7f000000;
                return (identities_.peers().empty() && desktop_open()) || identities_.known(key) ||
                       (loopback && config_.trust_loopback) ||
                       approved_by_owner(key, *stream, token);
            };
            const auto material =
                authenticate_session(*stream, PeerRole::pc, identities_.identity(), policy, stop);
            ControlChannel channel(*stream, material);
            std::optional<nlohmann::json> hello;
            const auto deadline = monotonic_ns() + 10'000'000'000;
            while (!stop.stop_requested() && stream->alive() && !hello && monotonic_ns() < deadline)
                hello = channel.receive(std::chrono::milliseconds(100));
            if (!hello || hello->value("type", "") != "hello")
                throw std::runtime_error("Phone capability hello missing");
            if (hello->value("protocol", 0) != wire::session_protocol) {
                channel.send({{"type", "disconnect"},
                              {"reason", "This phone app does not match the PC. Open the PocketVR "
                                         "app on your PC and choose Install on phone."}});
                throw std::runtime_error("Phone app protocol does not match");
            }
            const auto &caps = hello->at("capabilities");
            const bool thermal_supported = hello->value("thermal_guard", 0) == 1;
            const auto initial_thermal = thermal_supported ? caps.value("thermal_status", -1) : -1;
            if (initial_thermal < -1 || initial_thermal > 6) throw std::invalid_argument("Invalid thermal capability");
            const auto transport = hello->value("transport", std::string("udp"));
            if (transport != "udp" && transport != "tcp")
                throw std::invalid_argument("Unknown media transport");
            const bool tcp_mode = transport == "tcp";
            const bool wired_link = tcp_mode || hello->value("link", std::string{}) == "usb";
            if (tcp_mode && (!tcp_media || (stream->peer().address & 0xff000000) != 0x7f000000)) {
                channel.send({{"type", "disconnect"},
                              {"reason", "USB TCP needs an available loopback media listener; "
                                         "check the PC ports and adb reverse setup"}});
                throw std::invalid_argument("USB TCP requires a loopback adb reverse connection");
            }
            const auto model = caps.at("model").get<std::string>();
            if (model.empty() || model.size() > 128 || !caps.at("codecs").is_array() ||
                caps.at("codecs").size() > 32)
                throw std::invalid_argument("Invalid phone capabilities");
            const auto startup = state_.request({{"method", "settings.get"}});
            const Settings snapshot(startup.at("values"));
            const auto settings = snapshot.values();
            EncoderConfig encoder;
            encoder.width = config_.width;
            encoder.height = config_.height;
            encoder.fps = snapshot.get<uint32_t>("video.fps");
            encoder.bitrate =
                static_cast<uint32_t>(snapshot.get<double>("video.bitrate_mbps") * 1'000'000);
            encoder.slices = snapshot.get<uint32_t>("video.slices");
            encoder.preset = snapshot.get<std::string>("video.preset");
            encoder.codec =
                snapshot.get<std::string>("video.codec") == "hevc" ? Codec::hevc : Codec::h264;
            auto mime = encoder.codec == Codec::hevc ? "video/hevc" : "video/avc";
            const auto decodes = [&] {
                for (const auto &codec : caps.at("codecs")) {
                    if (codec.value("mime", "") != mime)
                        continue;
                    if (encoder.width == 2160 && encoder.height == 1080 &&
                        codec.value("supports_2160x1080_60", false))
                        return true;
                    for (const auto &size : codec.value("sizes", nlohmann::json::array()))
                        if (size.value("width", 0U) == encoder.width &&
                            size.value("height", 0U) == encoder.height &&
                            size.value("fps", 0U) >= encoder.fps)
                            return true;
                }
                return false;
            };
            // A manual size becomes the ceiling. If this phone cannot decode it, stream
            // at full size rather than refuse the connection over a preference.
            if (const auto resolution = snapshot.get<std::string>("video.resolution");
                resolution != "auto") {
                const auto split = resolution.find('x');
                encoder.width = static_cast<uint32_t>(std::stoul(resolution.substr(0, split)));
                encoder.height = static_cast<uint32_t>(std::stoul(resolution.substr(split + 1)));
                if (!decodes()) {
                    network_log(spdlog::level::warn, "Manual resolution not supported by this phone", resolution.c_str());
                    encoder.width = config_.width;
                    encoder.height = config_.height;
                }
            }
            // HEVC is a preference; a phone without it still gets a picture.
            if (encoder.codec == Codec::hevc && !decodes()) {
                encoder.codec = Codec::h264;
                mime = "video/avc";
            }
            if (!decodes()) {
                channel.send(
                    {{"type", "disconnect"},
                     {"reason",
                      "Phone does not report support for the requested stream size/rate/codec"}});
                throw std::runtime_error("Requested phone decoder configuration is unsupported");
            }
            take_session(slot, *stream, stop);
            owns_session = true;
            auto connection =
                std::make_shared<Connection>(state_, media, stream->peer(), material, tcp_mode, wired_link);
            connection->initial_thermal(thermal_supported, initial_thermal);
            struct CloseSession {
                std::shared_ptr<Connection> connection;
                ~CloseSession() {
                    connection->close();
                }
            } close_session{connection};
            connection->start(gpu_, compositor_, encoder, config_.software_only, snapshot);
            const auto formats = stream_format_ladder(caps.at("codecs"), mime,
                {encoder.width, encoder.height, encoder.fps});
            const bool format_supported = hello->value("format_change", 0) == 1;
            connection->format_capabilities(formats, format_supported);
            auto format_values = nlohmann::json::array();
            for (const auto format : formats) format_values.push_back(stream_format_json(format));
            {
                std::lock_guard pairing_lock(pairing_mutex_);
                if (pairing_revision_.load() != revision)
                    throw std::runtime_error("Pairing was revoked during connection setup");
                identities_.trust(material.peer_identity, model);
                std::lock_guard lock(connection_mutex_);
                active_ = connection;
            }
            channel.send({{"type", "ready"},
                          {"viewer_calibration", 1},
                          {"phone_stats", 1},
                          {"phone_input", 1},
                          {"adaptive_bitrate", 1},
                          {"format_change", format_supported ? 1 : 0},
                          {"profiles", state_.request({{"method", "profiles.list"}})},
                          {"formats", format_values},
                          {"thermal_guard", thermal_supported ? 1 : 0},
                          {"projection", startup.at("projection")},
                          {"stream",
                           {{"codec", encoder.codec == Codec::h264 ? "h264" : "hevc"},
                            {"width", encoder.width},
                            {"height", encoder.height},
                            {"fps", encoder.fps},
                            {"udp_port", media->local_endpoint().port},
                            {"transport", transport},
                            {"tcp_port", tcp_mode ? tcp_media->local().port : 0},
                            {"projection_id", startup.at("projection").at("id")}}},
                          {"settings", settings}});
            if (tcp_mode)
                connection->attach_tcp(*tcp_media, stop);
            state_.session_connected.store(true);
            auto next_ping = monotonic_ns(), last_message = next_ping;
            auto reset_window_ns = next_ping;
            uint64_t reset_baseline{};
            auto last_projection = startup.at("projection").at("id").get<uint32_t>();
            nlohmann::json last_quality;
            uint64_t menu_revision{};
            // One malformed report must not end a game session. Failures to send or
            // authenticate are other exception types and still end it.
            const auto tolerate = [](const char *operation, auto action) {
                try {
                    action();
                } catch (const nlohmann::json::exception &error) {
                    network_log(spdlog::level::warn, operation, error.what());
                } catch (const std::invalid_argument &error) {
                    network_log(spdlog::level::warn, operation, error.what());
                }
            };
            while (!stop.stop_requested() && stream->alive()) {
                if (auto message = channel.receive(std::chrono::milliseconds(100))) {
                    last_message = monotonic_ns();
                    const auto type = message->value("type", "");
                    if (type == "ping")
                        channel.send({{"type", "pong"}});
                    else if (type == "idr")
                        connection->phone_keyframe_request();
                    else if (type == "phone.stats")
                        tolerate("Phone stats ignored", [&] { connection->phone_stats(*message); });
                    else if (type == "stream.prepared" || type == "stream.committed" ||
                             type == "stream.restored" || type == "stream.rejected")
                        tolerate("Format reply ignored", [&] { connection->format_reply(*message); });
                    else if (type == "recenter")
                        tolerate("Recenter ignored", [&] { (void)state_.request({{"method", "recenter"}}); });
                    else if (type == "viewer.set") {
                        const auto request_id = message->value("request_id", nlohmann::json{});
                        try {
                            if (!request_id.is_number_integer() || request_id <= 0 ||
                                request_id > INT64_MAX)
                                throw std::invalid_argument("Invalid viewer request identifier");
                            const auto response = state_.request(
                                {{"method", "viewer.set"}, {"values", message->at("values")}});
                            connection->idr();
                            channel.send({{"type", "viewer.result"},
                                          {"request_id", request_id},
                                          {"ok", true},
                                          {"settings", response.at("values")},
                                          {"projection", response.at("projection")}});
                        } catch (const std::exception &e) {
                            channel.send({{"type", "viewer.result"},
                                          {"request_id", request_id},
                                          {"ok", false},
                                          {"error", e.what()}});
                        }
                    } else if (type == "input.mapping.set") {
                        const auto id=message->value("request_id",nlohmann::json{});
                        try {
                            if (!id.is_number_unsigned()||id==0) throw std::invalid_argument("Invalid mapping request ID");
                            const auto result=state_.request({{"method","input.mapping.set"},{"values",message->at("values")}});
                            channel.send({{"type","input.mapping.result"},{"request_id",id},{"ok",true},{"settings",result.at("values")}});
                        } catch (const std::exception &error) {
                            channel.send({{"type","input.mapping.result"},{"request_id",id},{"ok",false},{"error",error.what()}});
                        }
                    } else if (type == "settings.set") {
                        // A rejected value must not end the game session.
                        try {
                            const auto response = state_.request({{"method", "settings.set"},
                                                                  {"key", message->at("key")},
                                                                  {"value", message->at("value")}});
                            channel.send({{"type", "settings"},
                                          {"values", response.at("values")},
                                          {"projection", response.at("projection")}});
                        } catch (const std::invalid_argument &error) {
                            network_log(spdlog::level::warn, "Phone setting rejected", error.what());
                        } catch (const nlohmann::json::exception &error) {
                            network_log(spdlog::level::warn, "Phone setting rejected", error.what());
                        }
                    } else if (type == "profile.apply") {
                        try {
                            const auto response = state_.request({{"method", "profiles.apply"}, {"name", message->at("name")}});
                            channel.send({{"type", "settings"},
                                          {"values", response.at("values")},
                                          {"projection", response.at("projection")}});
                        } catch (const std::exception &error) {
                            network_log(spdlog::level::warn, "Phone profile rejected", error.what());
                        }
                    } else if (type == "latency.samples") {
                        tolerate("Latency report ignored", [&] { connection->latency_samples(*message); });
                    } else if (type == "phone.thermal") {
                        tolerate("Thermal report ignored", [&] { connection->thermal(*message); });
                    } else if (type == "disconnect")
                        break;
                }
                if (auto [menu_seen, lines] = state_.menu_snapshot(); menu_seen != menu_revision) {
                    menu_revision = menu_seen;
                    channel.send({{"type", "menu"}, {"lines", lines}});
                }
                const auto now = monotonic_ns();
                if (!connection->media_alive())
                    throw std::runtime_error("USB media connection closed");
                if (state_.viewer_geometry()->id != last_projection) {
                    const auto changed = state_.request({{"method", "settings.get"}});
                    last_projection = changed.at("projection").at("id").get<uint32_t>();
                    connection->bitrate_settings(Settings(changed.at("values")));
                    channel.send({{"type", "settings"},
                                  {"values", changed.at("values")},
                                  {"projection", changed.at("projection")}});
                }
                connection->adapt(now);
                connection->format_tick(channel, now);
                const auto quality = connection->bitrate_status();
                if (quality != last_quality) {
                    channel.send({{"type", "quality"}, {"values", quality}});
                    last_quality = quality;
                }
                if (now - last_message > 10'000'000'000)
                    throw std::runtime_error("Phone control heartbeat expired");
                if (now >= next_ping) {
                    channel.send({{"type", "ping"}});
                    next_ping = now + 2'000'000'000;
                }
                // A rare stall is recovered by the encoder restart. Only repeated
                // resets in a short window mean encoding has really failed.
                const auto resets = connection->encoder_stats().resets;
                if (now - reset_window_ns > 60'000'000'000) {
                    reset_window_ns = now;
                    reset_baseline = resets;
                }
                if (resets - reset_baseline >= 3) {
                    channel.send({{"type", "disconnect"},
                                  {"reason", "Encoder initialization/reset failed; inspect the PC "
                                             "logs or select H.264"}});
                    break;
                }
            }
        } catch (const std::exception &e) {
            network_log(spdlog::level::info, "Phone session ended", e.what());
        } catch (...) {
            network_log(spdlog::level::err, "Phone session ended", "Unknown exception");
        }
        stream->interrupt();
        std::shared_ptr<Connection> stopped;
        if (owns_session) {
            state_.session_connected.store(false);
            state_.pacing_pending_ns.store(0);
        }
        {
            std::lock_guard lock(connection_mutex_);
            if (owns_session)
                stopped = std::move(active_);
            if (session_slot_ == &slot)
                session_slot_ = nullptr;
            slot.stream.reset();
        }
        // Hold ownership while joining the encoder worker. Its weak output
        // callback must never become the last owner and join its own thread.
        if (stopped)
            stopped->close();
        if (owns_session)
            session_open_.store(false);
        slot.done.store(true);
    }

  public:
    Impl(State &state, Gpu &gpu, Compositor &compositor, NetworkConfig config)
        : state_(state), gpu_(gpu), compositor_(compositor), config_(std::move(config)),
          identities_(config_.identity_directory) {
        if (config_.addresses.empty())
            config_.addresses = win::private_addresses();
        if (config_.addresses.size() > 16)
            throw std::invalid_argument("At most 16 private interfaces may be bound");
        for (auto address : config_.addresses) {
            address.port = config_.control_port;
            auto listener = std::make_unique<TcpListener>(address);
            address.port = config_.media_port;
            auto media = std::make_shared<UdpTransport>(address);
            std::unique_ptr<TcpListener> tcp_media;
            if ((address.address & 0xff000000) == 0x7f000000)
                try {
                    tcp_media = std::make_unique<TcpListener>(address);
                } catch (const std::exception &error) {
                    spdlog::warn("USB TCP listener unavailable; UDP remains active: {}",
                                 error.what());
                }
            std::unique_ptr<win::MdnsAdvertiser> mdns;
            if (config_.advertise && (address.address & 0xff000000) != 0x7f000000) {
                try {
                    for (const auto &nic : win::private_interfaces())
                        if (nic.address.address == address.address) {
                            mdns = std::make_unique<win::MdnsAdvertiser>(
                                nic, listener->local().port, media->local_endpoint().port,
                                identities_.identity().public_key());
                            break;
                        }
                } catch (const std::exception &e) {
                    spdlog::warn("mDNS unavailable: {}; broadcast/manual IP remain available",
                                 e.what());
                }
            }
            listeners_.push_back(
                {std::move(listener), std::move(media), std::move(tcp_media), std::move(mdns), 0});
        }
        receive_ = Worker([this](StopToken stop) {
            raise_thread_priority();
            try {
                std::array<uint8_t, wire::max_datagram> bytes{};
                std::vector<UdpTransport *> sockets;
                for (const auto &listener : listeners_)
                    sockets.push_back(listener.media.get());
                while (!stop.stop_requested()) {
                    bool received = false;
                    for (auto &listener : listeners_)
                        for (unsigned i = 0; i < 64; ++i) {
                            Endpoint peer;
                            const auto size =
                                listener.media->receive(bytes, peer, std::chrono::milliseconds(0));
                            if (!size)
                                break;
                            received = true;
                            if (const auto nonce =
                                    parse_discovery_query(std::span(bytes).first(*size))) {
                                const auto now = monotonic_ns();
                                if (now - listener.last_discovery >= 100'000'000) {
                                    listener.last_discovery = now;
                                    const auto reply = discovery_reply(
                                        *nonce, {listener.control->local().port,
                                                 listener.media->local_endpoint().port,
                                                 win::computer_name(),
                                                 identities_.identity().public_key()});
                                    (void)listener.media->send(reply, peer);
                                }
                                continue;
                            }
                            const auto header = wire::decode_header(std::span(bytes).first(*size));
                            if (!header)
                                continue;
                            std::shared_ptr<Connection> connection;
                            {
                                std::lock_guard lock(connection_mutex_);
                                connection = active_;
                            }
                            if (connection &&
                                connection->matches(header->session, peer, listener.media.get()))
                                connection->receive(std::span(bytes).first(*size), peer);
                        }
                    // Wakes on the next packet. The timeout only bounds shutdown.
                    if (!received)
                        (void)UdpTransport::wait_any(sockets, std::chrono::milliseconds(20));
                }
            } catch (const std::exception &e) {
                network_log(spdlog::level::err, "UDP receiver stopped", e.what());
            } catch (...) {
                network_log(spdlog::level::err, "UDP receiver stopped", "Unknown exception");
            }
        });
        accept_ = Worker([this](StopToken stop) {
            try {
                while (!stop.stop_requested()) {
                    for (const auto &listener : listeners_) {
                        auto accepted = listener.control->accept(std::chrono::milliseconds(0));
                        if (!accepted)
                            continue;
                        // Never touch the live session here: this connection is unproven.
                        // It may only displace another connection still proving itself,
                        // so a silent stray cannot block a returning phone.
                        ControlSlot *slot{};
                        {
                            std::lock_guard lock(connection_mutex_);
                            for (auto &candidate : control_slots_)
                                if (&candidate != session_slot_ && (!slot || candidate.done.load()))
                                    slot = &candidate;
                            if (!slot->done.load() && slot->stream)
                                slot->stream->interrupt();
                        }
                        const auto deadline = monotonic_ns() + 3'000'000'000;
                        while (!slot->done.load() && !stop.stop_requested() &&
                               monotonic_ns() < deadline)
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        if (!slot->done.load() || stop.stop_requested()) {
                            accepted->interrupt();
                            continue;
                        }
                        std::shared_ptr<TcpStream> stream = std::move(accepted);
                        {
                            std::lock_guard lock(connection_mutex_);
                            slot->stream = stream;
                        }
                        slot->done.store(false);
                        slot->worker = Worker([this, slot, stream, media = listener.media,
                                               tcp_media = listener.tcp_media.get()](StopToken token) {
                            control(token, *slot, stream, media, tcp_media);
                        });
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            } catch (const std::exception &e) {
                network_log(spdlog::level::err, "TCP listener stopped", e.what());
            } catch (...) {
                network_log(spdlog::level::err, "TCP listener stopped", "Unknown exception");
            }
        });
    }
    ~Impl() {
        accept_.request_stop();
        if (accept_.joinable())
            accept_.join();
        for (auto &slot : control_slots_)
            slot.worker.request_stop();
        receive_.request_stop();
        {
            std::lock_guard lock(connection_mutex_);
            for (auto &slot : control_slots_)
                if (slot.stream)
                    slot.stream->interrupt();
        }
        for (auto &slot : control_slots_)
            if (slot.worker.joinable())
                slot.worker.join();
        if (receive_.joinable())
            receive_.join();
    }
    nlohmann::json endpoints() const {
        auto endpoints = nlohmann::json::array();
        for (const auto &listener : listeners_)
            endpoints.push_back(
                {{"ip", listener.control->local().ip()},
                 {"control_port", listener.control->local().port},
                 {"media_port", listener.media->local_endpoint().port},
                 {"udp_receive_buffer_bytes", listener.media->receive_buffer_bytes()},
                 {"udp_send_buffer_bytes", listener.media->send_buffer_bytes()},
                 {"udp_buffer_tuning_applied", listener.media->buffer_tuning_applied()},
                 {"tcp_media_port", listener.tcp_media ? listener.tcp_media->local().port : 0}});
        return endpoints;
    }
    nlohmann::json request(const nlohmann::json &input) {
        const auto method = input.at("method").get<std::string>();
        if (method == "network.status") {
            if (input.value("desktop", false))
                desktop_seen_ns_.store(monotonic_ns());
            std::shared_ptr<Connection> connection;
            {
                std::lock_guard lock(connection_mutex_);
                connection = active_;
            }
            const bool control_active =
                std::any_of(control_slots_.begin(), control_slots_.end(),
                            [](const ControlSlot &slot) { return !slot.done.load(); });
            nlohmann::json pending;
            {
                std::lock_guard lock(approval_mutex_);
                pending = pending_phone_;
            }
            return {{"ok", true},
                    {"control_active", control_active},
                    {"pending_phone", pending},
                    {"endpoints", endpoints()},
                    {"session", connection ? connection->status() : nlohmann::json(nullptr)}};
        }
        if (method == "pairing.answer") {
            std::lock_guard lock(approval_mutex_);
            // The answer belongs to the phone that was on screen. Another phone can take
            // the prompt just before the click, and must not inherit it.
            if (pending_phone_.is_null() ||
                input.value("phone", std::string{}) != pending_phone_.at("key").get<std::string>())
                return {{"ok", false}, {"error", "That phone is no longer waiting"}};
            approval_ = input.at("allow").get<bool>() ? 1 : -1;
            return {{"ok", true}};
        }
        if (method == "pairing.list")
            return {{"ok", true}, {"peers", identities_.peers()}};
        if (method == "pairing.forget") {
            const auto key = win::parse_key(input.at("identity"));
            std::lock_guard pairing_lock(pairing_mutex_);
            identities_.forget(key);
            pairing_revision_.fetch_add(1);
            {
                std::lock_guard lock(connection_mutex_);
                if (active_ && active_->identity() == key && session_slot_ && session_slot_->stream)
                    session_slot_->stream->interrupt();
            }
            return {{"ok", true}};
        }
        throw std::invalid_argument("Unknown network/pairing method");
    }
};
NetworkServer::NetworkServer(State &state, Gpu &gpu, Compositor &compositor, NetworkConfig config)
    : impl_(std::make_unique<Impl>(state, gpu, compositor, std::move(config))) {}
NetworkServer::~NetworkServer() = default;
nlohmann::json NetworkServer::request(const nlohmann::json &request) {
    return impl_->request(request);
}
nlohmann::json NetworkServer::endpoints() const {
    return impl_->endpoints();
}
} // namespace pocketvr::driver
