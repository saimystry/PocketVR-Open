// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/session.hpp>
#include <pocketvr/phone_link.hpp>
#include <gtest/gtest.h>
#include <cstring>
#include <future>
#include <thread>
#include <condition_variable>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#endif

using namespace pocketvr;
TEST(TcpMedia, StalledReaderSkipsRecordsWithoutBreakingFraming) {
    TcpListener listener(Endpoint::parse("127.0.0.1", 0));
    auto sender_stream = TcpStream::connect(listener.local());
    auto receiver = listener.accept(std::chrono::milliseconds(1000));
    ASSERT_TRUE(receiver);
    const auto peer = sender_stream->peer();
    TcpTransport sender(std::move(sender_stream));
    std::array<uint8_t, 1000> record{};
    const auto stamp = [&](uint32_t id) { std::memcpy(record.data(), &id, sizeof(id)); };
    uint32_t sent{};
    bool skipped = false;
    const auto until = monotonic_ns() + 20'000'000'000;
    while (!skipped && monotonic_ns() < until) {
        stamp(sent);
        if (sender.send(record, peer))
            ++sent;
        else
            skipped = true;
    }
    ASSERT_TRUE(skipped);
    // Windows, which sends the video, skips whole records. Other systems can write part
    // of one, and a stalled partial record is meant to close the stream.
#ifdef _WIN32
    ASSERT_TRUE(sender.alive()); // The stall costs a record, not the session.
#else
    if (!sender.alive())
        return;
#endif
    for (uint32_t id = 0; id < sent; ++id) {
        const auto received = receiver->receive_record(std::chrono::milliseconds(1000));
        ASSERT_TRUE(received);
        ASSERT_EQ(received->size(), record.size());
        uint32_t value{};
        std::memcpy(&value, received->data(), sizeof(value));
        ASSERT_EQ(value, id);
    }
    stamp(sent);
    ASSERT_TRUE(sender.send(record, peer));
    const auto after = receiver->receive_record(std::chrono::milliseconds(1000));
    ASSERT_TRUE(after);
    uint32_t value{};
    std::memcpy(&value, after->data(), sizeof(value));
    EXPECT_EQ(value, sent);
}
TEST(PhoneLink, TcpAdapterSendsAuthenticatedClockAndDetectsClosure) {
    TcpListener listener(Endpoint::parse("127.0.0.1", 0));
    auto phone_stream = TcpStream::connect(listener.local());
    auto pc_stream = listener.accept(std::chrono::milliseconds(1000));
    ASSERT_TRUE(pc_stream);
    TcpTransport pc(std::move(pc_stream));
    SessionMaterial material;
    material.id = 339;
    material.media.transmit.fill(17);
    material.media.receive.fill(28);
    PacketCipher cipher(material.id, {material.media.receive, material.media.transmit});
    PhoneLink phone(listener.local(), material, {},
                    std::make_unique<TcpTransport>(std::move(phone_stream)));
    const auto until = monotonic_ns() + 3'000'000'000;
    std::array<uint8_t, wire::max_datagram> bytes{};
    while (!phone.stats().clock_ready && monotonic_ns() < until) {
        Endpoint sender;
        const auto size = pc.receive(bytes, sender, std::chrono::milliseconds(200));
        if (!size)
            continue;
        wire::Header header;
        const auto payload = cipher.open(std::span(bytes).first(*size), header);
        ASSERT_TRUE(payload);
        ASSERT_EQ(header.type, wire::Type::ping);
        wire::Reader reader(*payload);
        uint64_t sent{};
        ASSERT_TRUE(reader.u64(sent));
        std::array<uint8_t, 24> pong{};
        wire::Writer writer(pong);
        writer.u64(sent);
        writer.u64(monotonic_ns());
        writer.u64(monotonic_ns());
        const auto sealed = cipher.seal(wire::Type::pong, 0, monotonic_ns(), pong);
        ASSERT_TRUE(sealed && pc.send(*sealed, sender));
    }
    ASSERT_TRUE(phone.stats().clock_ready);
    pc.interrupt();
    const auto closed_by = monotonic_ns() + 500'000'000;
    while (phone.stats().running && monotonic_ns() < closed_by)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_FALSE(phone.stats().running);
}
TEST(PhoneLink, KeyframeFlagSendsAnAuthenticatedRequestOverMedia) {
    UdpTransport pc(Endpoint::parse("127.0.0.1", 0));
    SessionMaterial material;
    material.id = 340;
    material.media.transmit.fill(19);
    material.media.receive.fill(29);
    PacketCipher cipher(material.id, {material.media.receive, material.media.transmit});
    const auto wanted = std::make_shared<std::atomic<bool>>(true);
    PhoneLink phone(pc.local_endpoint(), material, {}, {}, 60, wanted);
    const auto until = monotonic_ns() + 3'000'000'000;
    std::array<uint8_t, wire::max_datagram> bytes{};
    bool requested = false;
    while (!requested && monotonic_ns() < until) {
        Endpoint sender;
        const auto size = pc.receive(bytes, sender, std::chrono::milliseconds(200));
        if (!size)
            continue;
        wire::Header header;
        const auto payload = cipher.open(std::span(bytes).first(*size), header);
        ASSERT_TRUE(payload);
        if (header.type == wire::Type::keyframe) {
            requested = true;
            continue;
        }
        ASSERT_EQ(header.type, wire::Type::ping);
        wire::Reader reader(*payload);
        uint64_t sent{};
        ASSERT_TRUE(reader.u64(sent));
        std::array<uint8_t, 24> pong{};
        wire::Writer writer(pong);
        writer.u64(sent);
        writer.u64(monotonic_ns());
        writer.u64(monotonic_ns());
        const auto sealed = cipher.seal(wire::Type::pong, 0, monotonic_ns(), pong);
        ASSERT_TRUE(sealed && pc.send(*sealed, sender));
    }
    EXPECT_TRUE(requested);
    EXPECT_FALSE(wanted->load());
}
TEST(PhoneLink, ClockSyncConvertsFreshPoseAndRejectsUnsolicitedPong) {
    UdpTransport pc(Endpoint::parse("127.0.0.1", 0));
    SessionMaterial material;
    material.id = 178;
    material.media.transmit.fill(17);
    material.media.receive.fill(28);
    SessionKeys pc_keys{material.media.receive, material.media.transmit};
    PacketCipher cipher(material.id, pc_keys);
    // Simulate a PC monotonic clock three seconds ahead of the phone.
    constexpr uint64_t offset = 3'000'000'000;
    const wire::Bytes frame_bytes(16000, 137);
    std::atomic<bool> frame_verified{};
    std::atomic<uint64_t> expected_frame{17}, sink_calls{};
    PhoneLink link(pc.local_endpoint(), material, [&](FrameView frame) {
        sink_calls.fetch_add(1);
        const bool valid =
            frame.header.frame_id == expected_frame.load() && std::equal(frame.bytes.begin(), frame.bytes.end(),
                                                      frame_bytes.begin(), frame_bytes.end());
        frame_verified.store(valid);
        return valid;
    });
    std::array<uint8_t, wire::max_datagram> packet{};
    Endpoint phone;
    wire::Header header;
    const auto deadline = monotonic_ns() + 4'000'000'000;
    unsigned pings{};
    while (!link.stats().clock_ready && monotonic_ns() < deadline) {
        const auto size = pc.receive(packet, phone, std::chrono::milliseconds(100));
        if (!size)
            continue;
        const auto payload = cipher.open(std::span(packet).first(*size), header);
        ASSERT_TRUE(payload);
        ASSERT_EQ(header.type, wire::Type::ping);
        wire::Reader reader(*payload);
        uint64_t t0{};
        ASSERT_TRUE(reader.u64(t0));
        std::array<uint8_t, 24> pong{};
        auto reply = [&](uint64_t timestamp) {
            wire::Writer writer(pong);
            writer.u64(timestamp);
            writer.u64(monotonic_ns() + offset);
            writer.u64(monotonic_ns() + offset);
            const auto sealed = cipher.seal(wire::Type::pong, 0, monotonic_ns() + offset, pong);
            return sealed && pc.send(*sealed, phone);
        };
        ASSERT_TRUE(reply(t0 + 1)); // Authenticated but not an outstanding request.
        if (pings == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            EXPECT_FALSE(link.stats().clock_ready);
        }
        ASSERT_TRUE(reply(t0));
        ++pings;
    }
    ASSERT_TRUE(link.stats().clock_ready);
    EXPECT_GE(pings, 3);
    wire::PosePacket pose;
    pose.sequence = 4;
    pose.sensor_ns = monotonic_ns();
    pose.pose.orientation = Quat::exponential({0, 0.2, 0});
    ASSERT_TRUE(link.pose(pose));
    std::optional<wire::PosePacket> received;
    while (!received && monotonic_ns() < deadline) {
        const auto size = pc.receive(packet, phone, std::chrono::milliseconds(100));
        if (!size)
            continue;
        const auto payload = cipher.open(std::span(packet).first(*size), header);
        if (payload && header.type == wire::Type::pose)
            received = wire::decode_pose(*payload);
    }
    ASSERT_TRUE(received);
    EXPECT_EQ(received->sequence, pose.sequence);
    EXPECT_NEAR(static_cast<double>(received->sensor_ns),
                static_cast<double>(pose.sensor_ns + offset), 10'000'000);
    EXPECT_NEAR(received->pose.orientation.w, pose.pose.orientation.w, 1e-6);
    wire::VideoHeader metadata;
    metadata.frame_id = 17;
    metadata.render_ns = monotonic_ns() + offset;
    ASSERT_TRUE(packetize_frame(frame_bytes, metadata, 2, [&](const auto &video, auto shard) {
        if (video.shard == 0)
            return true; // Recover one missing data shard.
        std::array<uint8_t, wire::video_header_size + stream_shard_bytes> bytes{};
        if (!wire::encode_video_header(video, bytes))
            return false;
        std::copy(shard.begin(), shard.end(), bytes.begin() + wire::video_header_size);
        const auto sealed = cipher.seal(video.shard < video.k ? wire::Type::video : wire::Type::fec,
                                        0, monotonic_ns() + offset, bytes);
        return sealed && pc.send(*sealed, phone);
    }));
    while (!frame_verified.load() && monotonic_ns() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(frame_verified.load());
    EXPECT_EQ(link.stats().frames, 1);
    // Delayed authenticated packets from the previous format must never reach
    // the replacement decoder, even when their frame IDs are newer.
    const auto boundary=monotonic_ns()+offset;
    link.video_boundary(boundary,30);expected_frame.store(19);frame_verified.store(false);
    for(const auto id:{18U,19U}) {
        metadata.frame_id=id;metadata.render_ns=id==18?boundary-1:boundary+1;
        ASSERT_TRUE(packetize_frame(frame_bytes,metadata,0,[&](const auto &video,auto shard) {
            std::array<uint8_t,wire::video_header_size+stream_shard_bytes> bytes{};
            if(!wire::encode_video_header(video,bytes)) return false;
            std::copy(shard.begin(),shard.end(),bytes.begin()+wire::video_header_size);
            const auto sealed=cipher.seal(wire::Type::video,0,monotonic_ns()+offset,bytes);
            return sealed&&pc.send(*sealed,phone);
        }));
    }
    while(!frame_verified.load()&&monotonic_ns()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(frame_verified.load());EXPECT_EQ(sink_calls.load(),2);EXPECT_EQ(link.stats().frames,2);
}
TEST(Session, RealTcpMutualPairingAndEncryptedControlExchange) {
    Identity pc, phone;
    TcpListener listener(Endpoint::parse("127.0.0.1", 0));
    std::promise<void> phone_waiting;
    auto waiting = phone_waiting.get_future();
    std::promise<void> result;
    auto server_result = result.get_future();
    Worker server([&] {
        try {
            auto stream = listener.accept(std::chrono::milliseconds(1000));
            if (!stream)
                throw std::runtime_error("Test server accept timed out");
            PairingPolicy policy;
            policy.allow_new = true;
            policy.approve = [&](const Key &, StopToken) {
                if (waiting.wait_for(std::chrono::seconds(3)) != std::future_status::ready)
                    throw std::runtime_error("Phone was not waiting before PC approval");
                return true;
            };
            const auto material = authenticate_session(*stream, PeerRole::pc, pc, policy);
            ControlCipher cipher(material);
            auto record = stream->receive_record(std::chrono::milliseconds(1000));
            if (!record)
                throw std::runtime_error("Encrypted request missing");
            const auto plain = cipher.open(*record);
            if (!plain || *plain != wire::Bytes({1, 2, 3}))
                throw std::runtime_error("Encrypted request wrong");
            const auto reply = cipher.seal(*plain);
            if (!reply || !stream->send_record(*reply, std::chrono::milliseconds(1000)))
                throw std::runtime_error("Encrypted reply failed");
            result.set_value();
        } catch (...) {
            result.set_exception(std::current_exception());
        }
    });
    auto stream = TcpStream::connect(listener.local());
    PairingPolicy policy;
    policy.allow_new = true;
    policy.waiting = [&](const Key &) { phone_waiting.set_value(); };
    policy.approve = [](const Key &, StopToken) { return true; };
    const auto material = authenticate_session(*stream, PeerRole::phone, phone, policy);
    ControlCipher cipher(material);
    const wire::Bytes plain{1, 2, 3};
    const auto record = cipher.seal(plain);
    ASSERT_TRUE(record);
    ASSERT_TRUE(stream->send_record(*record, std::chrono::milliseconds(1000)));
    const auto reply = stream->receive_record(std::chrono::milliseconds(1000));
    ASSERT_TRUE(reply);
    EXPECT_EQ(cipher.open(*reply), plain);
    EXPECT_NO_THROW(server_result.get());
}
TEST(Session, OneMissingPinRequiresConfirmationOnBothSides) {
    Identity pc, phone;
    TcpListener listener(Endpoint::parse("127.0.0.1", 0));
    std::atomic<unsigned> confirmations{};
    std::promise<void> result;
    auto server_result = result.get_future();
    Worker server([&] {
        try {
            auto stream = listener.accept(std::chrono::milliseconds(1000));
            if (!stream)
                throw std::runtime_error("Accept timed out");
            PairingPolicy policy;
            policy.allow_new = true;
            policy.lookup_identity = [&](const Key &key) -> std::optional<Key> {
                return key == phone.public_key() ? std::optional(phone.public_key()) : std::nullopt;
            };
            policy.approve = [&](const Key &, StopToken) {
                ++confirmations;
                return true;
            };
            (void)authenticate_session(*stream, PeerRole::pc, pc, policy);
            result.set_value();
        } catch (...) {
            result.set_exception(std::current_exception());
        }
    });
    auto stream = TcpStream::connect(listener.local());
    PairingPolicy policy;
    policy.allow_new = true;
    policy.approve = [&](const Key &, StopToken) {
        ++confirmations;
        return true;
    };
    (void)authenticate_session(*stream, PeerRole::phone, phone, policy);
    EXPECT_NO_THROW(server_result.get());
    EXPECT_EQ(confirmations.load(), 2);
}
TEST(Session, StoredIdentityMismatchAndRevocationFailClosed) {
    for (const bool revoked : {false, true}) {
        Identity pc, phone, foreign;
        TcpListener listener(Endpoint::parse("127.0.0.1", 0));
        std::promise<bool> rejected;
        auto result = rejected.get_future();
        Worker server([&] {
            bool failed = false;
            try {
                auto stream = listener.accept(std::chrono::milliseconds(1000));
                if (!stream) throw std::runtime_error("Accept failed");
                unsigned lookups{};
                PairingPolicy policy;
                policy.lookup_identity = [&](const Key &) -> std::optional<Key> {
                    ++lookups;
                    if (!revoked) return foreign.public_key();
                    return lookups == 1 ? std::optional(phone.public_key()) : std::nullopt;
                };
                (void)authenticate_session(*stream, PeerRole::pc, pc, policy);
            } catch (const std::exception &) { failed = true; }
            rejected.set_value(failed);
        });
        auto stream = TcpStream::connect(listener.local());
        PairingPolicy policy;
        policy.expected_identity = pc.public_key();
        EXPECT_THROW((void)authenticate_session(*stream, PeerRole::phone, phone, policy), std::runtime_error);
        EXPECT_TRUE(result.get());
    }
}
TEST(Tcp, LargeCoalescedRecordsTimeoutAndDisconnect) {
    TcpListener listener(Endpoint::parse("127.0.0.1", 0));
    auto client = TcpStream::connect(listener.local());
    auto server = listener.accept(std::chrono::milliseconds(1000));
    ASSERT_TRUE(server);
    EXPECT_FALSE(server->receive_record(std::chrono::milliseconds(1)));
    const wire::Bytes first(65'000, 17), second(230, 42);
    Worker writer([&] {
        EXPECT_TRUE(client->send_record(first, std::chrono::milliseconds(1000)));
        EXPECT_TRUE(client->send_record(second, std::chrono::milliseconds(1000)));
    });
    EXPECT_EQ(server->receive_record(std::chrono::milliseconds(1000)), first);
    EXPECT_EQ(server->receive_record(std::chrono::milliseconds(1000)), second);
    writer.join();
    client->interrupt();
    EXPECT_FALSE(server->receive_record(std::chrono::milliseconds(1000)));
    EXPECT_FALSE(server->alive());
    EXPECT_THROW(TcpListener(Endpoint::parse("0.0.0.0", 0)), std::invalid_argument);
    EXPECT_THROW(TcpStream::connect(Endpoint::parse("8.8.8.8", 9943)), std::invalid_argument);
}
TEST(Tcp, PrefixAndPayloadResumeAcrossSingleByteReads) {
    TcpListener listener(Endpoint::parse("127.0.0.1", 0));
    const auto raw = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct Close {
        decltype(raw) socket;
        ~Close() {
#ifdef _WIN32
            closesocket(socket);
#else
            close(socket);
#endif
        }
    } close{raw};
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(listener.local().address);
    address.sin_port = htons(listener.local().port);
    ASSERT_EQ(::connect(raw, reinterpret_cast<const sockaddr *>(&address), sizeof(address)), 0);
    int one = 1;
    ASSERT_EQ(setsockopt(raw, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof(one)), 0);
    auto stream = listener.accept(std::chrono::milliseconds(1000));
    ASSERT_TRUE(stream);
    const wire::Bytes payload{1, 2, 3, 4, 5};
    const auto record = wire::ControlFramer::frame(payload);
    for (size_t i = 0; i < record.size(); ++i) {
        ASSERT_EQ(::send(raw, reinterpret_cast<const char *>(&record[i]), 1, 0), 1);
        const auto received = stream->receive_record(std::chrono::milliseconds(50));
        if (i + 1 < record.size()) EXPECT_FALSE(received);
        else EXPECT_EQ(received, payload);
        EXPECT_TRUE(stream->alive());
    }
}
TEST(Session, ControlChannelAcceptsExactMaximumAndRejectsOneByteOver) {
    TcpListener listener(Endpoint::parse("127.0.0.1", 0));
    auto client = TcpStream::connect(listener.local());
    auto server = listener.accept(std::chrono::milliseconds(1000));
    ASSERT_TRUE(server);
    SessionMaterial a, b;
    a.id = b.id = 991;
    a.control.transmit.fill(21); a.control.receive.fill(32);
    b.control.transmit = a.control.receive; b.control.receive = a.control.transmit;
    ControlChannel sender(*client, a), receiver(*server, b);
    const nlohmann::json maximum{{"x", std::string(ControlCipher::max_plaintext - 8, 'x')}};
    ASSERT_EQ(maximum.dump().size(), ControlCipher::max_plaintext);
    std::promise<bool> rejected;
    auto result = rejected.get_future();
    Worker writer([&] {
        try {
            sender.send(maximum);
            auto oversized = maximum;
            oversized["x"].get_ref<std::string &>().push_back('x');
            sender.send(oversized);
            rejected.set_value(false);
        } catch (const std::exception &) { rejected.set_value(true); }
    });
    const auto received = receiver.receive(std::chrono::milliseconds(1000));
    ASSERT_TRUE(received);
    EXPECT_EQ(*received, maximum);
    EXPECT_TRUE(result.get());
}
TEST(PhoneLink, AuthenticatedInputAndFreshHapticsRejectStaleTamperedAndWrongDevice) {
    UdpTransport pc(Endpoint::parse("127.0.0.1",0));
    SessionMaterial material; material.id=812;
    material.media.transmit.fill(31); material.media.receive.fill(72);
    PacketCipher cipher(material.id,{material.media.receive,material.media.transmit});
    PhoneLink phone(pc.local_endpoint(),material);
    Endpoint peer; std::array<uint8_t,wire::max_datagram> bytes{};
    const auto deadline=monotonic_ns()+3'000'000'000;
    while (!phone.stats().clock_ready&&monotonic_ns()<deadline) {
        Endpoint source; const auto count=pc.receive(bytes,source,std::chrono::milliseconds(20));
        if (!count) continue;
        wire::Header header; const auto plain=cipher.open(std::span(bytes).first(*count),header);
        if (!plain||header.type!=wire::Type::ping) continue;
        peer=source;
        wire::Reader reader(*plain); uint64_t phone_sent{}; ASSERT_TRUE(reader.u64(phone_sent));
        std::array<uint8_t,24> pong{}; wire::Writer writer(pong);
        writer.u64(phone_sent); writer.u64(monotonic_ns()); writer.u64(monotonic_ns());
        const auto packet=cipher.seal(wire::Type::pong,0,monotonic_ns(),pong);
        ASSERT_TRUE(packet&&pc.send(*packet,peer));
    }
    ASSERT_TRUE(phone.stats().clock_ready);
    RawInput input; input.active=true; input.buttons=1U<<13; input.sequence=77; input.timestamp_ns=monotonic_ns();
    ASSERT_TRUE(phone.input(input));
    bool received=false;
    while (!received&&monotonic_ns()<deadline) {
        Endpoint source; const auto count=pc.receive(bytes,source,std::chrono::milliseconds(20));
        if (!count) continue;
        wire::Header header; const auto plain=cipher.open(std::span(bytes).first(*count),header);
        if (!plain||header.type!=wire::Type::input) continue;
        const auto parsed=decode_input(*plain); ASSERT_TRUE(parsed);
        EXPECT_EQ(parsed->buttons,input.buttons); EXPECT_EQ(parsed->sequence,77);
        received=true;
    }
    ASSERT_TRUE(received);
    const HapticPulse pulse{50,1,.5f,120}; std::array<uint8_t,haptic_packet_size> payload{};
    ASSERT_TRUE(encode_haptic(pulse,payload));
    const auto send=[&](uint64_t time,uint16_t device,bool corrupt) {
        auto packet=cipher.seal(wire::Type::haptic,device,time,payload);
        if (!packet) return false;
        if (corrupt) packet->back()^=1;
        return pc.send(*packet,peer);
    };
    ASSERT_TRUE(send(monotonic_ns()-500'000'000,0,false));
    ASSERT_TRUE(send(monotonic_ns(),1,false));
    ASSERT_TRUE(send(monotonic_ns(),0,true));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(phone.haptic());
    ASSERT_TRUE(send(monotonic_ns(),0,false));
    std::optional<HapticPulse> event;
    while (!(event=phone.haptic())&&monotonic_ns()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(event); EXPECT_EQ(event->duration_ms,50); EXPECT_EQ(event->hand,1);
}
TEST(Worker, DestructorCancelsControlWaitAndJoins) {
    std::mutex mutex;
    std::condition_variable_any condition;
    std::promise<void> started;
    std::atomic<bool> exited{};
    const auto begin = std::chrono::steady_clock::now();
    {
        Worker worker([&](StopToken stop) {
            std::unique_lock lock(mutex);
            started.set_value();
            EXPECT_FALSE(wait_with_stop(condition, lock, stop, std::chrono::seconds(10),
                                        [] { return false; }));
            exited.store(true);
        });
        started.get_future().wait();
    }
    EXPECT_TRUE(exited.load());
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds(500));
}
TEST(ControlSecurity, AuthenticatedNonObjectAndDeepJsonCloseTheConnection) {
    const auto deep =
        std::string("{\"x\":") + std::string(40, '[') + "0" + std::string(40, ']') + "}";
    for (const auto &payload : {deep, std::string("[1]")}) {
        TcpListener listener(Endpoint::parse("127.0.0.1", 0));
        auto sender = TcpStream::connect(listener.local());
        auto receiver = listener.accept(std::chrono::milliseconds(1000));
        ASSERT_TRUE(receiver);
        SessionMaterial material;
        material.id = 7;
        material.control.transmit.fill(42);
        material.control.receive.fill(42);
        ControlCipher cipher(material);
        ControlChannel channel(*receiver, material);
        const auto record = cipher.seal(
            std::span(reinterpret_cast<const uint8_t *>(payload.data()), payload.size()));
        ASSERT_TRUE(record);
        ASSERT_TRUE(sender->send_record(*record, std::chrono::milliseconds(1000)));
        EXPECT_THROW(channel.receive(std::chrono::milliseconds(1000)), std::exception);
        EXPECT_FALSE(receiver->alive());
    }
}
