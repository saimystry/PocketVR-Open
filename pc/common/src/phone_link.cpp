// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/phone_link.hpp>
#include <pocketvr/media_queue.hpp>
#include <algorithm>
#ifdef __ANDROID__
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace pocketvr {
PhoneLink::PhoneLink(Endpoint server, const SessionMaterial &material, FrameSink sink,
                     std::unique_ptr<ITransport> transport, uint32_t fps,
                     KeyframeFlag keyframe_wanted)
    // The client binds an ephemeral wildcard port for routing/roaming. Only the
    // explicit private server endpoint is accepted, then AEAD is checked.
    : server_(server), transport_(transport ? std::move(transport)
                                            : std::make_unique<UdpTransport>(Endpoint{0, 0}, true)),
      cipher_(material.id, material.media), assembler_(8 * 1024 * 1024, 3, 33'333'334,
                                                     FecKind::reed_solomon, fps), sink_(std::move(sink)),
      keyframe_wanted_(keyframe_wanted ? std::move(keyframe_wanted)
                                       : std::make_shared<std::atomic<bool>>()) {
    if (!server.private_or_local() || !server.port)
        throw std::invalid_argument("Invalid private media endpoint");
    worker_ = Worker([this](StopToken stop) { run(stop); });
}
PhoneLink::~PhoneLink() {
    worker_.request_stop();
    if (worker_.joinable())
        worker_.join();
}
bool PhoneLink::pose(const wire::PosePacket &value) {
    return poses_.push(value);
}
PhoneLinkStats PhoneLink::stats() const {
    return {running_.load(),      clock_ready_.load(),    received_.load(),
            frames_.load(),       dropped_.load(),        sent_poses_.load(),
            rtt_.load(),          received_bytes_.load(), reassembly_dropped_.load(),
            fec_recovered_.load()};
}
bool PhoneLink::send(wire::Type type, std::span<const uint8_t> payload) {
    std::array<uint8_t, wire::max_datagram> bytes{};
    const auto size = cipher_.seal_into(type, 0, monotonic_ns(), payload, bytes);
    return size && transport_->send(std::span(bytes).first(*size), server_);
}
void PhoneLink::run(StopToken stop) noexcept {
#ifdef __ANDROID__
    // Video and poses pass through here, so it must not wait behind background work.
    (void)setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), -8);
#endif
    running_.store(true);
    try {
        ClockSync clock;
        std::array<uint64_t, 16> outstanding{};
        size_t next_outstanding{};
        unsigned samples{};
        uint64_t next_ping{}, last_pong{}, newest_frame{}, boundary{}, last_keyframe{};
        std::array<uint8_t, wire::max_datagram> packet{};
        std::array<uint8_t, wire::max_payload> plain{};
        while (!stop.stop_requested() && transport_->alive()) {
            const auto changed = video_boundary_.load();
            if (changed != boundary) {
                assembler_.format_rate(video_fps_.load()); boundary = changed;
                needs_idr_.store(true);
            }
            reassembly_dropped_.store(assembler_.stats().dropped);
            fec_recovered_.store(assembler_.stats().recovered);
            const auto now = monotonic_ns();
            if (now >= next_ping) {
                std::array<uint8_t, 8> ping{};
                wire::Writer writer(ping);
                writer.u64(now);
                if (send(wire::Type::ping, ping)) {
                    outstanding[next_outstanding] = now;
                    next_outstanding = (next_outstanding + 1) % outstanding.size();
                }
                next_ping = now + (samples < 20 ? 100'000'000 : 500'000'000);
            }
            if (last_pong && now - last_pong > 3'000'000'000) {
                clock_ready_.store(false);
                clock.reset();
                samples = 0;
                last_pong = 0;
            }
            wire::PosePacket pose, newest;
            bool have_pose = false;
            while (poses_.pop(pose)) {
                newest = pose;
                have_pose = true;
            }
            const auto pose_now = monotonic_ns();
            if (have_pose && clock_ready_.load() && newest.sensor_ns <= pose_now &&
                pose_now - newest.sensor_ns < 50'000'000) {
                const auto pc_time = clock.phone_to_pc_ns(static_cast<int64_t>(newest.sensor_ns));
                if (pc_time) {
                    newest.sensor_ns = static_cast<uint64_t>(*pc_time);
                    std::array<uint8_t, wire::pose_size> payload{};
                    if (wire::encode_pose(newest, payload) && send(wire::Type::pose, payload))
                        sent_poses_.fetch_add(1);
                }
            }
            if (clock_ready_.load() && keyframe_wanted_->load() &&
                (!last_keyframe || now - last_keyframe >= keyframe_request_gap_ns)) {
                const std::array<uint8_t, 1> request{};
                keyframe_wanted_->store(false);
                if (send(wire::Type::keyframe, request))
                    last_keyframe = now;
            }
            Endpoint source;
            RawInput input, latest_input;
            bool have_input=false;
            while (inputs_.pop(input)) { latest_input=input; have_input=true; }
            if (have_input && clock_ready_.load() && latest_input.timestamp_ns<=now &&
                now-latest_input.timestamp_ns<100'000'000) {
                if (const auto pc_time=clock.phone_to_pc_ns(static_cast<int64_t>(latest_input.timestamp_ns))) {
                    latest_input.timestamp_ns=static_cast<uint64_t>(*pc_time);
                    std::array<uint8_t,input_packet_size> bytes{};
                    if (encode_input(latest_input,bytes)) (void)send(wire::Type::input,bytes);
                }
            }
            const auto size = transport_->receive(packet, source, std::chrono::milliseconds(1));
            if (!size || source != server_) {
                assembler_.expire(now);
                continue;
            }
            wire::Header header;
            const auto count = cipher_.open_into(std::span(packet).first(*size), header, plain);
            if (!count || header.device != 0)
                continue;
            received_.fetch_add(1);
            received_bytes_.fetch_add(*size);
            const auto payload = std::span(plain).first(*count);
            const auto arrived = monotonic_ns();
            if (header.type == wire::Type::pong && payload.size() == 24) {
                wire::Reader reader(payload);
                uint64_t p0{}, c1{}, c2{};
                reader.u64(p0);
                reader.u64(c1);
                reader.u64(c2);
                const auto pending = std::find(outstanding.begin(), outstanding.end(), p0);
                if (!p0 || pending == outstanding.end() || p0 > arrived ||
                    arrived - p0 > 200'000'000 || c1 > INT64_MAX || c2 > INT64_MAX)
                    continue;
                *pending = 0;
                if (clock.observe(static_cast<int64_t>(p0), static_cast<int64_t>(c1),
                                  static_cast<int64_t>(c2), static_cast<int64_t>(arrived))) {
                    last_pong = arrived;
                    ++samples;
                    rtt_.store(clock.rtt_ns());
                    clock_ready_.store(samples >= 3);
                    if (const auto pc_time = clock.phone_to_pc_ns(static_cast<int64_t>(p0)))
                        pc_offset_ns_.store(static_cast<int64_t>(*pc_time) - static_cast<int64_t>(p0));
                }
            } else if (header.type==wire::Type::haptic && clock_ready_.load() && header.send_ns<=INT64_MAX) {
                const auto pulse=decode_haptic(payload);
                const auto sent=clock.pc_to_phone_ns(static_cast<int64_t>(header.send_ns));
                if (pulse&&sent&&*sent>0&&static_cast<uint64_t>(*sent)<=arrived+20'000'000&&
                    (static_cast<uint64_t>(*sent)>arrived||arrived-static_cast<uint64_t>(*sent)<100'000'000))
                    (void)haptics_.push({*pulse,arrived});
            } else if (sink_ &&
                       (header.type == wire::Type::video || header.type == wire::Type::fec) &&
                       payload.size() > wire::video_header_size) {
                const auto video = wire::decode_video_header(payload);
                if (!video || video->render_ns < video_boundary_.load() || !clock_ready_.load() || video->render_ns > INT64_MAX ||
                    (header.type == wire::Type::video) != (video->shard < video->k))
                    continue;
                const auto rendered = clock.pc_to_phone_ns(static_cast<int64_t>(video->render_ns));
                if (!rendered || static_cast<uint64_t>(*rendered) > arrived + 100'000'000 ||
                    arrived > static_cast<uint64_t>(*rendered) + 100'000'000)
                    continue;
                if (auto frame = assembler_.push(*video, payload.subspan(wire::video_header_size),
                                                 arrived)) {
                    if (frame->header.frame_id > newest_frame && sink_ && sink_(*frame)) {
                        newest_frame = frame->header.frame_id;
                        frames_.fetch_add(1);
                    } else {
                        dropped_.fetch_add(1);
                        want_keyframe();
                    }
                }
                if (assembler_.stats().needs_idr) {
                    want_keyframe();
                    assembler_.clear_idr_request();
                }
            }
        }
    } catch (...) {
        needs_idr_.store(true);
    }
    clock_ready_.store(false);
    running_.store(false);
}
} // namespace pocketvr
