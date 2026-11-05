// SPDX-License-Identifier: Apache-2.0
#include <jni.h>
#include "discovery.hpp"
#include "sensors.hpp"
#include "decoder.hpp"
#include "renderer.hpp"
#include <android/log.h>
#include <android/native_window_jni.h>
#include <pocketvr/session.hpp>
#include <pocketvr/phone_link.hpp>
#include <pocketvr/settings.hpp>
#include <pocketvr/stream_format.hpp>
#include <nlohmann/json.hpp>
#include <sodium.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <algorithm>
#include <cmath>

using namespace pocketvr;
using Json = nlohmann::json;
namespace {
// NDK r26 libc++ supplies the shared_ptr atomic free functions on all supported
// API levels; this also keeps a codec alive while a frame callback borrows it.
class DecoderRoute {
    struct Target { std::shared_ptr<pocketvr::android::Decoder> decoder; uint64_t boundary{}; };
    std::shared_ptr<Target> target_;
  public:
    explicit DecoderRoute(std::shared_ptr<pocketvr::android::Decoder> decoder) { store(std::move(decoder)); }
    void store(std::shared_ptr<pocketvr::android::Decoder> decoder, uint64_t boundary = 0) {
        std::shared_ptr<Target> next;
        if (decoder) next = std::make_shared<Target>(Target{std::move(decoder),boundary});
        std::atomic_store(&target_,std::move(next));
    }
    bool enqueue(FrameView frame) const {
        const auto target = std::atomic_load(&target_);
        return target && frame.header.render_ns >= target->boundary && target->decoder->enqueue(frame);
    }
};
std::string hex(const Key &key) {
    std::array<char, 65> text{};
    sodium_bin2hex(text.data(), text.size(), key.data(), key.size());
    return text.data();
}
Key unhex(const std::string &text) {
    Key key{};
    size_t used{};
    if (text.size() != 64 ||
        sodium_hex2bin(key.data(), key.size(), text.data(), text.size(), nullptr, &used, nullptr) ||
        used != key.size())
        throw std::invalid_argument("Invalid pinned PC identity");
    return key;
}
// The shared protocol code reports low-level failures. The phone is the only
// place the user can act on them, so translate the common ones into the step
// that actually unblocks the connection.
std::string explain(const std::string &message) {
    if (message.find("TCP connection") != std::string::npos)
        return "Could not reach the PC. Start SteamVR on your gaming PC, then check the PocketVR app "
               "there: it says if something is wrong. On Wi-Fi, keep both on the same network.";
    if (message.find("was not approved") != std::string::npos)
        return "The PC did not allow this phone. Tap Connect, then click Allow in the PocketVR app on your PC.";
    if (message.find("Handshake") != std::string::npos)
        return "The PC did not finish connecting. If it asked, click Allow in the PocketVR app on your "
               "PC, then tap Connect.";
    if (message.find("Encoder") != std::string::npos)
        return "The PC could not start video. Update its graphics driver, or choose H.264 in Picture quality.";
    if (message.find("heartbeat") != std::string::npos)
        return "The connection to the PC was lost. Check the cable or Wi-Fi, then tap Connect.";
    if (message.find("Stored identity does not match") != std::string::npos)
        return "This looks like a different PC. Open Settings and tap Forget this PC.";
    return message;
}
std::string utf8(JNIEnv *env, jstring value) {
    if (!value)
        throw std::invalid_argument("Null string");
    const auto length = env->GetStringLength(value);
    if (length > 65504)
        throw std::invalid_argument("Java string exceeds control limit");
    const jchar *raw = env->GetStringChars(value, nullptr);
    if (!raw)
        throw std::runtime_error("Cannot read Java string");
    struct Release {
        JNIEnv *env;
        jstring value;
        const jchar *raw;
        ~Release() {
            env->ReleaseStringChars(value, raw);
        }
    } release{env, value, raw};
    std::string text;
    text.reserve(static_cast<size_t>(length));
    for (jsize i = 0; i < length; ++i) {
        uint32_t cp = raw[i];
        if (cp >= 0xd800 && cp <= 0xdbff) {
            if (++i == length || raw[i] < 0xdc00 || raw[i] > 0xdfff)
                throw std::invalid_argument("Invalid UTF-16 surrogate");
            cp = 0x10000 + ((cp - 0xd800) << 10) + (raw[i] - 0xdc00);
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
            throw std::invalid_argument("Invalid UTF-16 surrogate");
        }
        if (cp < 0x80)
            text.push_back(static_cast<char>(cp));
        else {
            if (cp < 0x800)
                text.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            else {
                if (cp < 0x10000)
                    text.push_back(static_cast<char>(0xe0 | (cp >> 12)));
                else {
                    text.push_back(static_cast<char>(0xf0 | (cp >> 18)));
                    text.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
                }
                text.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            }
            text.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }
    return text;
}
class Client final {
    class SetupError : public std::runtime_error { using std::runtime_error::runtime_error; };
    Identity identity_;
    Json capabilities_;
    pocketvr::android::DiscoveryScanner scanner_;
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    Json status_{{"state", "Ready to connect"}, {"connected", false}};
    std::shared_ptr<TcpStream> stream_;
    Worker control_;
    std::atomic<unsigned> rotation_{};
    std::weak_ptr<pocketvr::android::Sensors> sensors_;
    std::weak_ptr<PhoneLink> media_;
    uint32_t input_sequence_{};
    std::optional<Json> mapping_request_;
    uint64_t mapping_revision_{};
    std::shared_ptr<GyroBiasCache> calibration_{std::make_shared<GyroBiasCache>()};
    std::shared_ptr<pocketvr::android::Renderer> renderer_;
    bool grid_{};
    int view_mode_{};
    double view_size_{1};
    uint64_t view_size_changed_ns_{};
    uint64_t pc_recenter_due_ns_{};
    std::vector<std::pair<std::string, Json>> pending_settings_;
    double display_hz_{60};
    int stats_mode_{-1};
    std::atomic<uint64_t> display_generation_{};
    std::atomic<int> thermal_status_{-1};
    std::atomic<uint64_t> thermal_sample_ns_{};
    Json viewer_;
    uint64_t viewer_revision_{};
    void state(std::string text, std::string error = "") {
        std::lock_guard lock(mutex_);
        status_["state"] = std::move(text);
        status_["error"] = std::move(error);
    }
    void run(StopToken stop, Endpoint peer, std::optional<Key> pinned, bool allow_pairing,
             bool tcp_media, bool wired_link) noexcept {
        unsigned backoff_ms = 250;
        while (!stop.stop_requested()) {
            bool permanent_error = false;
            try {
                state("Connecting to " + peer.ip());
                std::shared_ptr<TcpStream> stream = TcpStream::connect(peer);
                {
                    std::lock_guard lock(mutex_);
                    stream_ = stream;
                    status_["error"] = "";
                    status_.erase("approval");
                }
                PairingPolicy policy;
                policy.expected_identity = pinned;
                policy.allow_new = allow_pairing;
                // The phone trusts a PC on first connection; the PC decides about the phone.
                policy.approve = [](const Key &, StopToken) { return true; };
                policy.waiting = [this](const Key &) {
                    std::lock_guard lock(mutex_);
                    status_["approval"] = true;
                };
                const auto material =
                    authenticate_session(*stream, PeerRole::phone, identity_, policy, stop);
                ControlChannel channel(*stream, material);
                auto capabilities = capabilities_;
                const auto sampled = thermal_sample_ns_.load();
                capabilities["thermal_status"] = sampled && monotonic_ns() - sampled < 2'000'000'000 ? thermal_status_.load() : -1;
                channel.send({{"type", "hello"},
                              {"protocol", wire::session_protocol},
                              {"capabilities", capabilities}, {"thermal_guard", 1}, {"format_change", 1},
                              {"transport", tcp_media ? "tcp" : "udp"},
                              {"link", wired_link ? "usb" : "network"}});
                const auto deadline = monotonic_ns() + 10'000'000'000;
                std::optional<Json> ready;
                while (!stop.stop_requested() && stream->alive() && monotonic_ns() < deadline &&
                       !ready)
                    ready = channel.receive(std::chrono::milliseconds(100));
                if (!ready || ready->value("type", "") != "ready")
                    throw SetupError(
                        ready ? ready->value("reason", "PC rejected capabilities")
                              : "The PC did not answer. If this keeps happening, update PocketVR on the PC.");
                const auto &video = ready->at("stream");
                if (tcp_media && video.value("transport", std::string("udp")) != "tcp")
                    throw SetupError("This PC does not support USB TCP media");
                const auto codec = video.at("codec").get<std::string>();
                const auto width = video.at("width").get<uint32_t>(),
                           height = video.at("height").get<uint32_t>();
                if ((codec != "h264" && codec != "hevc") || !width || !height || width > 4096 ||
                    height > 4096 || (width & 1) || (height & 1))
                    throw std::runtime_error("PC stream configuration is invalid");
                pinned = material.peer_identity;
                const bool viewer_supported = ready->value("viewer_calibration", 0) == 1;
                const bool stats_supported = ready->value("phone_stats", 0) == 1;
                const bool thermal_supported = ready->value("thermal_guard", 0) == 1;
                uint64_t stats_sequence{}, last_stats{};
                uint64_t thermal_sequence{}, last_thermal{};
                uint64_t viewer_sent{};
                {
                    std::lock_guard lock(mutex_);
                    status_["state"] = "Connected; waiting for video";
                    status_["connected"] = true;
                    status_["connections"] = status_.value("connections", uint64_t{}) + 1;
                    status_["peer_identity"] = hex(material.peer_identity);
                    status_["stream"] = video;
                    status_["format_changing"] = false;
                    status_["format_error"] = "";
                    status_["settings"] = ready->at("settings");
                    status_["projection"] = ready->value("projection", Json::object());
                    status_["phone_stats_supported"] = stats_supported;
                    status_["phone_input_supported"] = ready->value("phone_input",0)==1;
                    status_["phone_stats_reports"] = 0;
                    status_["phone_stats"] = nullptr;
                    status_["quality"] = nullptr;
                }
                auto next_ping = monotonic_ns();
                auto last_receive = next_ping;
                const auto media_started = next_ping;
                const auto udp_port = video.at(tcp_media ? "tcp_port" : "udp_port").get<uint32_t>();
                if (!udp_port || udp_port > 65535)
                    throw std::runtime_error("Invalid PC media port");
                std::shared_ptr<pocketvr::android::Renderer> renderer;
                uint64_t generation{};
                {
                    std::lock_guard lock(mutex_);
                    renderer = renderer_;
                    generation = display_generation_.load();
                }
                std::shared_ptr<pocketvr::android::Decoder> decoder;
                const auto keyframe_wanted = std::make_shared<std::atomic<bool>>();
                uint32_t projection_id = video.value("projection_id", 1U);
                if (renderer) renderer->configure(Settings(ready->at("settings")), projection_id);
                {
                    std::lock_guard lock(mutex_);
                    status_.erase("approval");
                    status_["profiles"] = ready->value("profiles", Json::object());
                }
                {
                    // The phone's eye size wins at connection; after that, the newest change.
                    std::lock_guard lock(mutex_);
                    pending_settings_.clear();
                    queue_setting("display.eye_size", view_size_);
                }
                const auto make_decoder = [&](StreamFormat format) -> std::shared_ptr<pocketvr::android::Decoder> {
                    if (!renderer) return {};
                    const auto renderer_session = renderer->begin_stream();
                    pocketvr::android::DecoderConfig config;
                    config.present = [renderer, renderer_session](const auto &header, uint64_t timestamp,
                                                                  uint64_t received, uint64_t queued) {
                        return renderer->frame(header, timestamp, renderer_session, received, queued);
                    };
                    config.mime = codec == "hevc" ? "video/hevc" : "video/avc";
                    config.width = format.width;
                    config.height = format.height;
                    config.fps = format.fps;
                    config.keyframe_wanted = keyframe_wanted;
                    for (const auto &candidate : capabilities_.at("codecs")) {
                        if (candidate.value("mime", "") != config.mime)
                            continue;
                        const auto sizes = candidate.value("sizes", Json::array());
                        const bool supported = std::any_of(sizes.begin(), sizes.end(), [&](const auto &size) {
                            return size.value("width", 0U) == config.width &&
                                   size.value("height", 0U) == config.height &&
                                   size.value("fps", 0U) >= config.fps;
                        });
                        if (supported) {
                            config.name = candidate.at("name").get<std::string>();
                            break;
                        }
                    }
                    if (config.name.empty())
                        throw SetupError("No hardware decoder supports the negotiated stream; change PC video settings");
                    return std::make_shared<pocketvr::android::Decoder>(
                        renderer->decoder_window(), std::move(config));
                };
                auto current_format = parse_stream_format(video);
                decoder = make_decoder(current_format);
                const auto decoder_route = std::make_shared<DecoderRoute>(decoder);
                const Endpoint media_peer{peer.address, static_cast<uint16_t>(udp_port)};
                std::unique_ptr<ITransport> media_transport;
                if (tcp_media)
                    media_transport =
                        std::make_unique<TcpTransport>(TcpStream::connect(media_peer));
                auto media = std::make_shared<PhoneLink>(
                    media_peer, material,
                    [decoder_route](FrameView frame) { return decoder_route->enqueue(frame); },
                    std::move(media_transport), video.at("fps").get<uint32_t>(), keyframe_wanted);
                auto sensors = std::make_shared<pocketvr::android::Sensors>(media, calibration_);
                sensors->configure(ready->at("settings"));
                sensors->rotation(rotation_.load());
                if (renderer)
                    renderer->tracking(sensors);
                {
                    std::lock_guard lock(mutex_);
                    sensors_ = sensors;
                    media_=media;
                }
                uint64_t last_idr{}, decoder_restart_window{};
                unsigned decoder_restarts{};
                bool idr_pending = true;
                const bool format_supported = ready->value("format_change", 0) == 1;
                const auto allowed_formats = stream_format_ladder(capabilities_.at("codecs"),
                    codec == "hevc" ? "video/hevc" : "video/avc", current_format);
                uint64_t format_revision{}, format_boundary{};
                StreamFormat pending_format = current_format;
                enum class FormatPhase { stable, preparing, prepared, restoring, failed };
                auto format_phase = FormatPhase::stable;
                uint64_t format_deadline{};
                const auto change_decoder = [&](StreamFormat next, uint64_t boundary) {
                    format_deadline = monotonic_ns() + 10'000'000'000;
                    decoder_route->store(nullptr);
                    media->video_boundary(boundary, next.fps);
                    if (decoder) decoder->stop();
                    decoder.reset(); // Join/stop old codec before using its Surface again.
                    decoder = make_decoder(next);
                    pending_format = next;
                    format_boundary = boundary;
                    std::lock_guard lock(mutex_);
                    status_["format_changing"] = true;
                };
                while (!stop.stop_requested() && stream->alive()) {
                    std::optional<Json> mapping;
                    {
                        std::lock_guard lock(mutex_);
                        mapping=std::move(mapping_request_); mapping_request_.reset();
                    }
                    if (mapping) channel.send(*mapping);
                    Json viewer;
                    uint64_t revision{};
                    {
                        std::lock_guard lock(mutex_);
                        if (!viewer_.is_null() && viewer_sent != viewer_revision_) {
                            if (viewer_supported) {
                                viewer = viewer_;
                                revision = viewer_revision_;
                            } else
                                status_["viewer_sync"] =
                                    "Saved on phone; this PC needs a newer PocketVR driver";
                        }
                    }
                    std::vector<std::pair<std::string, Json>> outgoing;
                    {
                        std::lock_guard lock(mutex_);
                        outgoing.swap(pending_settings_);
                    }
                    for (const auto &[key, value] : outgoing)
                        if (key == "profile")
                            channel.send({{"type", "profile.apply"}, {"name", value}});
                        else
                            channel.send({{"type", "settings.set"}, {"key", key}, {"value", value}});
                    if (revision) {
                        channel.send(
                            {{"type", "viewer.set"}, {"request_id", revision}, {"values", viewer}});
                        viewer_sent = revision;
                        std::lock_guard lock(mutex_);
                        if (revision == viewer_revision_)
                            status_["viewer_sync"] = "Sending calibration to PC";
                    }
                    if (auto message = channel.receive(std::chrono::milliseconds(100))) {
                        last_receive = monotonic_ns();
                        const auto type = message->value("type", "");
                        if (type == "disconnect")
                            throw std::runtime_error(message->value("reason", "PC disconnected"));
                        if (type == "ping")
                            channel.send({{"type", "pong"}});
                        if (format_supported && (type == "stream.prepare" || type == "stream.abort")) {
                            const auto id = message->at("request_id");
                            const auto boundary = message->at("render_boundary_ns");
                            const auto next = parse_stream_format(message->at("format"));
                            const bool abort = type == "stream.abort";
                            if (!id.is_number_integer() || id <= 0 || id > INT64_MAX ||
                                !boundary.is_number_integer() || boundary <= 0 || boundary > INT64_MAX ||
                                std::find(allowed_formats.begin(), allowed_formats.end(), next) == allowed_formats.end())
                                throw std::runtime_error("PC sent an unsupported format transaction");
                            if ((!abort && id > format_revision && format_phase == FormatPhase::stable) ||
                                (abort && id == format_revision && boundary > format_boundary)) {
                                format_revision = id.get<uint64_t>();
                                format_phase = abort ? FormatPhase::restoring : FormatPhase::preparing;
                                try { change_decoder(next, boundary.get<uint64_t>()); }
                                catch (const std::exception &error) {
                                    if (abort) throw;
                                    format_phase = FormatPhase::failed;
                                    channel.send({{"type", "stream.rejected"}, {"request_id", format_revision}});
                                    std::lock_guard lock(mutex_); status_["format_error"] = error.what();
                                }
                            }
                        }
                        if (format_supported && type == "stream.commit" &&
                            message->at("request_id") == format_revision && format_phase == FormatPhase::prepared) {
                            decoder_route->store(decoder, format_boundary); current_format = pending_format;
                            format_phase = FormatPhase::stable; idr_pending = true;
                            channel.send({{"type", "stream.committed"}, {"request_id", format_revision}});
                            std::lock_guard lock(mutex_);
                            status_["format_changing"] = false;
                            for (const auto key : {"width", "height", "fps"})
                                status_["stream"][key] = stream_format_json(current_format).at(key);
                        }
                        if (type == "viewer.result") {
                            std::lock_guard lock(mutex_);
                            if (message->value("request_id", uint64_t{}) == viewer_revision_) {
                                if (message->value("ok", false)) {
                                    projection_id =
                                        message->at("projection").at("id").get<uint32_t>();
                                    if (renderer)
                                        renderer->configure(Settings(message->at("settings")),
                                                            projection_id);
                                    status_["settings"] = message->at("settings");
                                    status_["projection"] = message->at("projection");
                                    status_["viewer_sync"] = "Calibration applied on PC";
                                } else
                                    status_["viewer_sync"] =
                                        "PC rejected calibration: " +
                                        message->value("error", std::string("Invalid settings"));
                            }
                        }
                        if (type == "menu" && renderer)
                            renderer->menu(message->at("lines").get<std::vector<std::string>>());
                        if (type == "settings") {
                            sensors->configure(message->at("values"));
                            if (message->contains("projection"))
                                projection_id = message->at("projection").at("id").get<uint32_t>();
                            const Settings incoming(message->at("values"));
                            if (renderer)
                                renderer->configure(incoming, projection_id);
                            std::lock_guard lock(mutex_);
                            status_["settings"] = message->at("values");
                            // A change made on the PC becomes this phone's saved value, or
                            // the old one would overwrite it at the next connection.
                            const auto eye = incoming.get<double>("display.eye_size");
                            if (std::abs(eye - view_size_) > 1e-6 && pending_settings_.empty() &&
                                monotonic_ns() - view_size_changed_ns_ > 1'500'000'000) {
                                view_size_ = eye;
                                status_["eye_size"] = eye;
                                if (renderer_)
                                    renderer_->view_size(eye);
                            }
                            const auto lenses = incoming.viewer_values();
                            if (viewer_sent == viewer_revision_ && lenses != viewer_) {
                                viewer_ = lenses;
                                status_["viewer_from_pc"] = lenses;
                            }
                        }
                        if (type=="input.mapping.result") {
                            std::lock_guard lock(mutex_);
                            if (message->value("request_id",uint64_t{})==mapping_revision_) {
                                if (message->value("ok",false)) {
                                    status_["settings"]=message->at("settings");
                                    status_["mapping_status"]="Input mapping saved on PC";
                                } else status_["mapping_status"]="Mapping rejected: "+message->value("error",std::string("Invalid mapping"));
                            }
                        }
                        if (type == "quality") {
                            if (renderer && message->at("values").contains("guard"))
                                renderer->cooling(message->at("values").at("guard").value("input_fps_cap", 60U) == 0);
                            std::lock_guard lock(mutex_);
                            status_["quality"] = message->at("values");
                        }
                    }
                    const auto now = monotonic_ns();
                    bool recenter_pc = false;
                    {
                        std::lock_guard lock(mutex_);
                        if (pc_recenter_due_ns_ && now >= pc_recenter_due_ns_) {
                            pc_recenter_due_ns_ = 0;
                            recenter_pc = true;
                        }
                    }
                    if (recenter_pc)
                        channel.send({{"type", "recenter"}});
                    if (format_phase == FormatPhase::preparing || format_phase == FormatPhase::restoring) {
                        const auto configuring = decoder ? decoder->status() : Json(nullptr);
                        if (decoder && !configuring.at("error").get<std::string>().empty()) {
                            if (format_phase == FormatPhase::restoring)
                                throw std::runtime_error("Previous decoder format could not be restored");
                            channel.send({{"type", "stream.rejected"}, {"request_id", format_revision}});
                            format_phase = FormatPhase::failed;
                        } else if (!decoder || configuring.at("running").get<bool>()) {
                            const bool restored = format_phase == FormatPhase::restoring;
                            if (restored) {
                                decoder_route->store(decoder, format_boundary); current_format = pending_format;
                                format_phase = FormatPhase::stable; idr_pending = true;
                                std::lock_guard lock(mutex_);
                                status_["format_changing"] = false;
                                for (const auto key : {"width", "height", "fps"})
                                    status_["stream"][key] = stream_format_json(current_format).at(key);
                            } else format_phase = FormatPhase::prepared;
                            channel.send({{"type", restored ? "stream.restored" : "stream.prepared"},
                                {"request_id", format_revision}});
                        }
                    }
                    if (format_phase != FormatPhase::stable && now > format_deadline)
                        throw std::runtime_error("Video format transaction timed out");
                    if (thermal_supported && (!last_thermal || now - last_thermal >= 1'000'000'000)) {
                        const auto sample = thermal_sample_ns_.load();
                        const auto level = thermal_status_.load();
                        if (level >= 0 && sample && now >= sample && now - sample < 1'000'000'000)
                            channel.send({{"type", "phone.thermal"}, {"seq", ++thermal_sequence},
                                {"status", level}, {"sample_age_ms", (now - sample) / 1'000'000}});
                        last_thermal = now;
                    }
                    if (display_generation_.load() != generation)
                        break;
                    const auto network = media->stats();
                    const auto video_status = decoder ? decoder->status() : Json(nullptr);
                    if (stats_supported && (!last_stats || now - last_stats >= 1'000'000'000)) {
                        auto report = empty_phone_stats();
                        report["clock_ready"] = network.clock_ready;
                        report["rtt_ms"] = static_cast<double>(network.rtt_ns) / 1e6;
                        report["media_packets"] = network.received;
                        report["media_bytes"] = network.received_bytes;
                        report["accepted_frames"] = network.frames;
                        report["sink_rejected_frames"] = network.dropped;
                        report["reassembly_dropped_frames"] = network.reassembly_dropped;
                        report["fec_recovered_groups"] = network.fec_recovered;
                        if (decoder) {
                            report["decoder_submitted"] = video_status.at("submitted");
                            report["decoder_decoded"] = video_status.at("decoded");
                            report["decoder_dropped"] = video_status.at("dropped");
                            report["decode_samples"] = video_status.at("decode_samples");
                            report["decode_p50_ms"] = video_status.at("decode_p50_ms");
                            report["decode_p95_ms"] = video_status.at("decode_p95_ms");
                        }
                        if (renderer) {
                            const auto draw = renderer->status();
                            report["display_fps"] = draw.at("display_fps");
                            report["video_fps"] = draw.at("video_fps");
                            report["render_cpu_ms"] = draw.at("render_ms");
                            report["repeated_frames"] = draw.at("repeated");
                            report["missed_vsync"] = draw.at("missed_vsync");
                            report["arrival_lead_ms"] = draw.at("arrival_lead_ms");
                            report["arrival_samples"] = draw.at("arrival_samples");
                        }
                        if (renderer && network.clock_ready) {
                            // In the PC's clock, so the PC can join them with its own timings.
                            const auto offset = media->pc_offset_ns();
                            const auto to_pc = [offset](uint64_t time) {
                                return static_cast<uint64_t>(static_cast<int64_t>(time) + offset);
                            };
                            auto samples = Json::array();
                            for (const auto &frame : renderer->take_latency())
                                samples.push_back({frame.frame_id, to_pc(frame.received_ns), to_pc(frame.queued_ns),
                                                   to_pc(frame.decoded_ns), to_pc(frame.displayed_ns)});
                            if (!samples.empty())
                                channel.send({{"type", "latency.samples"}, {"samples", samples}});
                        }
                        validate_phone_stats(report);
                        channel.send({{"type", "phone.stats"},
                                      {"seq", ++stats_sequence},
                                      {"values", report}});
                        last_stats = now;
                        std::lock_guard lock(mutex_);
                        status_["phone_stats_reports"] = stats_sequence;
                        status_["phone_stats"] = std::move(report);
                    }
                    idr_pending = media->take_idr_request() || idr_pending;
                    if (decoder)
                        idr_pending = decoder->take_idr_request() || idr_pending;
                    if (decoder && network.clock_ready && video_status.at("running").get<bool>() &&
                        idr_pending && (!last_idr || now - last_idr >= keyframe_request_gap_ns)) {
                        channel.send({{"type", "idr"}});
                        last_idr = now;
                        idr_pending = false;
                    }
                    if (decoder && format_phase == FormatPhase::stable && !video_status.at("error").get<std::string>().empty()) {
                        // A codec failure only needs a new codec and a keyframe. Tracking,
                        // keys and sockets stay up, so the picture returns in a moment.
                        const auto error = video_status.at("error").get<std::string>();
                        if (now - decoder_restart_window > 30'000'000'000) {
                            decoder_restart_window = now;
                            decoder_restarts = 0;
                        }
                        if (++decoder_restarts > 3)
                            throw std::runtime_error(error);
                        __android_log_print(ANDROID_LOG_WARN, "PocketVR", "Restarting decoder: %s", error.c_str());
                        decoder_route->store(nullptr);
                        decoder->stop();
                        decoder.reset(); // Join/stop old codec before using its Surface again.
                        decoder = make_decoder(current_format);
                        decoder_route->store(decoder, format_boundary);
                        idr_pending = true;
                        last_idr = 0;
                        continue;
                    }
                    if (renderer && !renderer->status().at("error").get<std::string>().empty())
                        throw std::runtime_error(renderer->status().at("error").get<std::string>());
                    if (now - media_started > 5'000'000'000)
                        backoff_ms = 250;
                    if (!network.running && now - media_started > 1'000'000'000)
                        throw std::runtime_error("Media socket stopped");
                    {
                        std::lock_guard lock(mutex_);
                        status_["clock_ready"] = network.clock_ready;
                        status_["rtt_ms"] = static_cast<double>(network.rtt_ns) / 1e6;
                        status_["media_packets"] = network.received;
                        status_["poses_sent"] = network.poses;
                        status_["sensors"] = sensors->status();
                        status_["decoder"] = video_status;
                        status_["state"] = decoder && video_status.at("decoded").get<uint64_t>() > 0
                                               ? "Stereo renderer connected"
                                               : "Connected; waiting for video";
                    }
                    if (now - last_receive > 10'000'000'000)
                        throw std::runtime_error("PC stopped responding");
                    if (now >= next_ping) {
                        channel.send({{"type", "ping"}});
                        next_ping = now + 2'000'000'000;
                    }
                }
            } catch (const SetupError &e) {
                __android_log_print(ANDROID_LOG_ERROR, "PocketVR", "Session setup failed: %s", e.what());
                permanent_error = true;
                state("Configuration needs attention; reconnect after changing settings",
                      explain(e.what()));
            } catch (const std::exception &e) {
                __android_log_print(ANDROID_LOG_WARN, "PocketVR", "Session ended: %s", e.what());
                if (!stop.stop_requested())
                    state(pinned ? "Connection lost; reconnecting" : "Pairing stopped",
                          explain(e.what()));
            } catch (...) {
                if (!stop.stop_requested())
                    state("Connection stopped", "Unexpected native client error");
            }
            {
                std::lock_guard lock(mutex_);
                if (stream_)
                    stream_->interrupt();
                stream_.reset();
                media_.reset(); mapping_request_.reset();
                if (status_.value("mapping_status",std::string{})=="Saving input mapping")
                    status_["mapping_status"]="Mapping outcome uncertain; reconnect and review PC settings";
                status_["connected"] = false;
                if (!viewer_.is_null())
                    status_["viewer_sync"] = "Saved on phone; waiting for PC";
            }
            if (permanent_error || !pinned || stop.stop_requested())
                break;
            std::unique_lock lock(mutex_);
            wait_with_stop(changed_, lock, stop, std::chrono::milliseconds(backoff_ms),
                           [] { return false; });
            backoff_ms = std::min(backoff_ms * 2, 8'000U);
        }
    }

  public:
    void display_hz(double hz) {
        if (!std::isfinite(hz) || hz < 20 || hz > 240) throw std::invalid_argument("Display refresh rate");
        std::lock_guard lock(mutex_);
        display_hz_ = hz;
        if (renderer_) renderer_->display_hz(hz);
    }
    void thermal(int status) {
        if (status < -1 || status > 6) throw std::invalid_argument("Thermal status range");
        thermal_status_.store(status); thermal_sample_ns_.store(monotonic_ns());
    }
    std::string viewer(const Json &values) {
        Settings candidate;
        candidate.set_viewer(values);
        const auto canonical = candidate.viewer_values();
        std::lock_guard lock(mutex_);
        if (viewer_revision_ == INT64_MAX)
            throw std::runtime_error("Viewer revision exhausted; reopen PocketVR");
        viewer_ = canonical;
        status_.erase("viewer_from_pc");
        ++viewer_revision_;
        status_["viewer_sync"] = "Saved on phone; waiting for PC";
        if (renderer_ && !status_.value("connected", false))
            renderer_->configure(candidate, 1);
        return canonical.dump();
    }
    Client(const Key &seed, Json capabilities)
        : identity_(seed), capabilities_(std::move(capabilities)) {
        if (!capabilities_.is_object() || !capabilities_.contains("codecs"))
            throw std::invalid_argument("Invalid device capabilities");
    }
    ~Client() {
        stop();
    }
    void connect(const std::string &ip, uint16_t port, const std::string &pinned,
                 bool allow_pairing, bool tcp_media, bool wired_link) {
        stop();
        const auto peer = Endpoint::parse(ip, port);
        if (tcp_media && (peer.address & 0xff000000) != 0x7f000000)
            throw std::invalid_argument("USB adb mode requires loopback (127.0.0.1)");
        std::optional<Key> key;
        if (!pinned.empty())
            key = unhex(pinned);
        control_ = Worker([this, peer, key, allow_pairing, tcp_media, wired_link](StopToken token) {
            run(token, peer, key, allow_pairing, tcp_media, wired_link);
        });
    }
    void stop() {
        control_.request_stop();
        {
            std::lock_guard lock(mutex_);
            if (stream_)
                stream_->interrupt();
        }
        changed_.notify_all();
        if (control_.joinable())
            control_.join();
        std::lock_guard lock(mutex_);
        status_["state"] = "Disconnected";
        status_["error"] = "";
        status_["connected"] = false;
        media_.reset(); mapping_request_.reset();
        if (status_.value("mapping_status",std::string{})=="Saving input mapping")
            status_["mapping_status"]="Mapping outcome uncertain; reconnect and review PC settings";
        if (renderer_) {
            (void)renderer_->begin_stream();
            renderer_->tracking({});
            renderer_->cooling(false);
        }
        if (!viewer_.is_null())
            status_["viewer_sync"] = "Saved on phone; waiting for PC";
    }
    void rotation(unsigned value) {
        if (value > 3)
            throw std::invalid_argument("Invalid display rotation");
        rotation_.store(value);
        std::lock_guard lock(mutex_);
        if (auto sensors = sensors_.lock())
            sensors->rotation(value);
    }
    void surface(std::shared_ptr<pocketvr::android::Renderer> renderer) {
        stop();
        std::lock_guard lock(mutex_);
        renderer_ = std::move(renderer);
        if (renderer_) {
            renderer_->display_hz(display_hz_);
            Settings settings;
            if (!viewer_.is_null())
                settings.set_viewer(viewer_);
            renderer_->configure(settings, 1);
            renderer_->grid(grid_);
            renderer_->view_mode(view_mode_);
            renderer_->view_size(view_size_);
            renderer_->stats(stats_mode_);
        }
        display_generation_.fetch_add(1);
    }
    void view_mode(int mode) {
        std::lock_guard lock(mutex_);
        view_mode_ = mode;
        if (renderer_)
            renderer_->view_mode(mode);
    }
    void view_size(double size) {
        std::lock_guard lock(mutex_);
        view_size_ = size;
        view_size_changed_ns_ = monotonic_ns();
        status_["eye_size"] = size;
        if (renderer_)
            renderer_->view_size(size);
        queue_setting("display.eye_size", size);
    }
    // Sent on the control channel; the PC validates, saves and echoes the settings.
    void setting(const std::string &key, Json value) {
        std::lock_guard lock(mutex_);
        queue_setting(key, std::move(value));
    }
    void queue_setting(const std::string &key, Json value) {
        std::erase_if(pending_settings_, [&](const auto &item) { return item.first == key; });
        if (pending_settings_.size() < 32)
            pending_settings_.emplace_back(key, std::move(value));
    }
    void grid(bool enabled) {
        std::lock_guard lock(mutex_);
        grid_ = enabled;
        if (renderer_)
            renderer_->grid(enabled);
    }
    void input(RawInput value) {
        std::lock_guard lock(mutex_);
        if (!valid_input(value)) throw std::invalid_argument("Invalid phone controls");
        if (!status_.value("connected",false)||!status_.value("phone_input_supported",false)) return;
        if (const auto media=media_.lock()) {
            value.sequence=++input_sequence_; value.timestamp_ns=monotonic_ns();
            (void)media->input(value);
        }
    }
    int haptic() {
        std::lock_guard lock(mutex_);
        if (!status_.value("connected",false)) return 0;
        if (const auto media=media_.lock()) if (const auto event=media->haptic()) {
            const auto amplitude=static_cast<unsigned>(std::clamp(std::lround(event->amplitude*255.f),0L,255L));
            return static_cast<int>((1U<<30)|(static_cast<unsigned>(event->hand)<<24)|(amplitude<<16)|event->duration_ms);
        }
        return 0;
    }
    void mapping(Json value) {
        if (!value.is_object()||value.size()!=17) throw std::invalid_argument("Mapping needs 17 fields");
        Settings check;
        check.set("input.mapping",value.at("mapping"));
        for (const auto name:input_buttons) check.set("input.bind_"+std::string(name),value.at("bind_"+std::string(name)));
        std::lock_guard lock(mutex_);
        if (!status_.value("connected",false)||!status_.value("phone_input_supported",false)) throw std::runtime_error("Connect to an updated PC before saving mappings");
        if (status_.value("mapping_status",std::string{})=="Saving input mapping") throw std::runtime_error("Wait for the pending mapping result");
        mapping_request_=Json{{"type","input.mapping.set"},{"request_id",++mapping_revision_},{"values",std::move(value)}};
        status_["mapping_status"]="Saving input mapping";
    }
    void stats(int mode) {
        if (mode < -1 || mode > 2)
            throw std::invalid_argument("Invalid stats overlay mode");
        std::lock_guard lock(mutex_);
        stats_mode_ = mode;
        if (renderer_)
            renderer_->stats(mode);
    }
    void recenter(bool calibration) {
        std::lock_guard lock(mutex_);
        if (auto sensors = sensors_.lock()) {
            if (calibration)
                sensors->calibrate();
            else
                sensors->recenter();
            // The PC keeps its own offset, which would otherwise add to this one. It
            // recenters on the new poses, so give those a moment to arrive.
            pc_recenter_due_ns_ = monotonic_ns() + 50'000'000;
        }
    }
    void discover(bool enabled) {
        if (enabled)
            scanner_.start();
        else
            scanner_.stop();
    }
    std::string status() const {
        Json value;
        std::shared_ptr<pocketvr::android::Renderer> renderer;
        {
            std::lock_guard lock(mutex_);
            value = status_;
            renderer = renderer_;
        }
        value["renderer"] = renderer ? renderer->status() : Json(nullptr);
        value["found_pcs"] = scanner_.results();
        return value.dump(-1, ' ', true);
    }
};
std::mutex clients_mutex;
std::unordered_map<jlong, std::shared_ptr<Client>> clients;
jlong next_client{};
std::shared_ptr<Client> client(jlong handle) {
    std::lock_guard lock(clients_mutex);
    const auto found = clients.find(handle);
    if (found == clients.end())
        throw std::invalid_argument("Native client is closed");
    return found->second;
}
void throw_java(JNIEnv *env, const char *message) noexcept {
    if (env->ExceptionCheck()) return;
    const auto type = env->FindClass("java/lang/IllegalStateException");
    if (type) env->ThrowNew(type, message);
}
template <class F> auto boundary(JNIEnv *env, F action, decltype(action()) fallback) noexcept {
    try {
        return action();
    } catch (const std::exception &e) {
        throw_java(env, e.what());
    } catch (...) {
        throw_java(env, "Unexpected native error");
    }
    return fallback;
}
} // namespace
extern "C" JNIEXPORT jstring JNICALL Java_com_pocketvr_NativeBridge_viewerCatalog(JNIEnv *env,
                                                                                  jobject) {
    return boundary(
        env,
        [&] {
            return env->NewStringUTF(
                Json{{"values", Settings().viewer_values()}, {"schema", Settings::schema()}}
                    .dump()
                    .c_str());
        },
        static_cast<jstring>(nullptr));
}
extern "C" JNIEXPORT jstring JNICALL Java_com_pocketvr_NativeBridge_validateViewer(JNIEnv *env,
                                                                                   jobject,
                                                                                   jstring values) {
    return boundary(
        env,
        [&] {
            Settings candidate;
            candidate.set_viewer(Json::parse(utf8(env, values)));
            return env->NewStringUTF(candidate.viewer_values().dump().c_str());
        },
        static_cast<jstring>(nullptr));
}
extern "C" JNIEXPORT jstring JNICALL Java_com_pocketvr_NativeBridge_viewer(JNIEnv *env, jobject,
                                                                           jlong handle,
                                                                           jstring values) {
    return boundary(
        env,
        [&] {
            return env->NewStringUTF(client(handle)->viewer(Json::parse(utf8(env, values))).c_str());
        },
        static_cast<jstring>(nullptr));
}
extern "C" JNIEXPORT jlong JNICALL Java_com_pocketvr_NativeBridge_create(JNIEnv *env, jobject,
                                                                         jbyteArray seed,
                                                                         jstring caps) {
    return boundary(
        env,
        [&]() -> jlong {
            if (!seed || env->GetArrayLength(seed) != 32)
                throw std::invalid_argument("Identity seed must be 32 bytes");
            Key key{};
            env->GetByteArrayRegion(seed, 0, 32, reinterpret_cast<jbyte *>(key.data()));
            struct Wipe {
                Key &key;
                ~Wipe() {
                    sodium_memzero(key.data(), key.size());
                }
            } wipe{key};
            auto value = std::make_shared<Client>(key, Json::parse(utf8(env, caps)));
            std::lock_guard lock(clients_mutex);
            if (next_client == INT64_MAX) throw std::runtime_error("Client handle space exhausted");
            const auto handle = ++next_client;
            clients.emplace(handle, std::move(value));
            return handle;
        },
        jlong{});
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeBridge_destroy(JNIEnv *env, jobject,
                                                                         jlong handle) {
    (void)boundary(
        env,
        [&] {
            auto value = client(handle);
            value->stop();
            std::lock_guard lock(clients_mutex);
            clients.erase(handle);
            return true;
        },
        false);
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeBridge_connect(JNIEnv *env, jobject,
                                                                         jlong handle, jstring ip,
                                                                         jint port, jstring pinned,
                                                                         jboolean allow_pairing,
                                                                         jboolean tcp_media,
                                                                         jboolean wired_link) {
    (void)boundary(
        env,
        [&] {
            if (port < 1 || port > 65535)
                throw std::invalid_argument("Control port");
            client(handle)->connect(utf8(env, ip), static_cast<uint16_t>(port), utf8(env, pinned),
                                   allow_pairing != 0, tcp_media != 0, wired_link != 0);
            return true;
        },
        false);
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeBridge_stop(JNIEnv *env, jobject,
                                                                      jlong handle) {
    (void)boundary(
        env,
        [&] {
            client(handle)->stop();
            return true;
        },
        false);
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeBridge_discover(JNIEnv *env, jobject,
                                                                          jlong handle,
                                                                          jboolean enabled) {
    (void)boundary(
        env,
        [&] {
            client(handle)->discover(enabled != 0);
            return true;
        },
        false);
}
extern "C" JNIEXPORT jstring JNICALL Java_com_pocketvr_NativeBridge_status(JNIEnv *env, jobject,
                                                                           jlong handle) {
    return boundary(
        env, [&] { return env->NewStringUTF(client(handle)->status().c_str()); },
        static_cast<jstring>(nullptr));
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeBridge_rotation(JNIEnv *env, jobject,
                                                                          jlong handle,
                                                                          jint rotation) {
    (void)boundary(
        env,
        [&] {
            client(handle)->rotation(static_cast<unsigned>(rotation));
            return true;
        },
        false);
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeBridge_input(JNIEnv *env,jobject,jlong handle,
    jint buttons,jfloat lx,jfloat ly,jfloat rx,jfloat ry,jfloat lt,jfloat rt,jboolean active,jboolean gamepad) {
    (void)boundary(env,[&] {
        RawInput value; value.buttons=static_cast<uint32_t>(buttons); value.axes={lx,ly,rx,ry,lt,rt};
        value.active=active; value.gamepad=gamepad; client(handle)->input(value); return true;
    },false);
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeBridge_setInputMapping(JNIEnv *env,jobject,jlong handle,jstring values) {
    (void)boundary(env,[&] { client(handle)->mapping(Json::parse(utf8(env,values))); return true; },false);
}
extern "C" JNIEXPORT jint JNICALL Java_com_pocketvr_NativeBridge_takeHaptic(JNIEnv *env,jobject,jlong handle) {
    return boundary(env,[&] { return static_cast<jint>(client(handle)->haptic()); },static_cast<jint>(0));
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeBridge_displayRate(JNIEnv *env, jobject,
                                                                             jlong handle, jdouble hz) {
    (void)boundary(env, [&] { client(handle)->display_hz(hz); return true; }, false);
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeBridge_thermal(JNIEnv *env, jobject,
                                                                       jlong handle, jint status) {
    (void)boundary(env, [&] { client(handle)->thermal(status); return true; }, false);
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeBridge_recenter(JNIEnv *env, jobject,
                                                                          jlong handle,
                                                                          jboolean calibration) {
    (void)boundary(
        env,
        [&] {
            client(handle)->recenter(calibration);
            return true;
        },
        false);
}

extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeBridge_surface(JNIEnv *env, jobject,
                                                                         jlong handle,
                                                                         jobject surface,
                                                                         jobject texture,
                                                                         jboolean single_buffer) {
    (void)boundary(
        env,
        [&] {
            std::shared_ptr<ANativeWindow> window;
            if (surface) {
                auto *native = ANativeWindow_fromSurface(env, surface);
                if (!native)
                    throw std::runtime_error("Video surface is unavailable");
                window = std::shared_ptr<ANativeWindow>(native, ANativeWindow_release);
            }
            if (static_cast<bool>(surface) != static_cast<bool>(texture))
                throw std::invalid_argument("Renderer requires both display and decoder texture");
            auto renderer =
                surface
                    ? std::make_shared<pocketvr::android::Renderer>(env, texture, std::move(window),
                                                                    single_buffer != 0)
                    : nullptr;
            client(handle)->surface(std::move(renderer));
            return true;
        },
        false);
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeBridge_viewMode(JNIEnv *env, jobject,
                                                                          jlong handle, jint mode) {
    (void)boundary(env, [&] { client(handle)->view_mode(mode); return true; }, false);
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeBridge_setting(JNIEnv *env, jobject,
                                                                         jlong handle, jstring key,
                                                                         jstring value) {
    (void)boundary(env, [&] { client(handle)->setting(utf8(env, key), Json::parse(utf8(env, value))); return true; }, false);
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeBridge_viewSize(JNIEnv *env, jobject,
                                                                          jlong handle, jdouble size) {
    (void)boundary(env, [&] { client(handle)->view_size(size); return true; }, false);
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeBridge_grid(JNIEnv *env, jobject,
                                                                      jlong handle,
                                                                      jboolean enabled) {
    (void)boundary(
        env,
        [&] {
            client(handle)->grid(enabled != 0);
            return true;
        },
        false);
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeBridge_stats(JNIEnv *env, jobject,
                                                                       jlong handle, jint mode) {
    (void)boundary(
        env,
        [&] {
            client(handle)->stats(mode);
            return true;
        },
        false);
}
