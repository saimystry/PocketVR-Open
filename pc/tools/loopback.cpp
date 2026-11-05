// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/crypto.hpp>
#include <pocketvr/reassembler.hpp>
#include <pocketvr/transport.hpp>
#include "encoder/mf.hpp"
#include <iostream>
#include <numeric>
#include <thread>

using namespace pocketvr;
using namespace pocketvr::driver;
static size_t codec_round_trip(PacketCipher &sender, PacketCipher &receiver, UdpTransport &a,
                               UdpTransport &b) {
    constexpr uint32_t width = 320, height = 180;
    Gpu gpu(true);
    EncoderConfig config;
    config.width = width;
    config.height = height;
    config.bitrate = 4'000'000;
    auto encoder = create_encoder(gpu, config, true);
    MfDecoder decoder(Codec::h264, width, height);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Com<ID3D11Texture2D> texture;
    check(gpu.device->CreateTexture2D(&description, nullptr, &texture), "Loopback texture failed");
    wire::Bytes pixels(width * height * 4), reference(width * height * 3 / 2);
    Reassembler assembler(1 << 20);
    std::array<uint8_t, wire::max_datagram> received{};
    size_t completed{};
    for (uint64_t id = 2; id < 5; ++id) {
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                const auto at = (static_cast<size_t>(y) * width + x) * 4;
                pixels[at] = static_cast<uint8_t>(x < width / 2 ? 24 : 208);
                pixels[at + 1] = static_cast<uint8_t>((y + id) % 128);
                pixels[at + 2] = static_cast<uint8_t>(x < width / 2 ? 208 : 24);
                pixels[at + 3] = 255;
            }
        gpu.context->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), width * 4, 0);
        if (!encoder->submit(texture.Get(), id, id == 2))
            throw std::runtime_error("Loopback encoder rejected frame");
        const auto deadline = monotonic_ns() + 5'000'000'000;
        std::optional<EncodedFrame> encoded;
        while (!encoded && monotonic_ns() < deadline) {
            encoded = encoder->poll();
            if (!encoded)
                std::this_thread::yield();
        }
        if (!encoded || encoded->frame_id != id)
            throw std::runtime_error("Loopback encode timed out or frame identity changed");
        wire::VideoHeader metadata;
        metadata.frame_id = id;
        metadata.render_ns = monotonic_ns();
        metadata.flags = encoded->idr ? 1 : 0;
        bool fed{};
        const bool sent =
            packetize_frame(encoded->bytes, metadata, 4, [&](const auto &header, auto shard) {
                if (header.shard == 0 || header.shard == 1)
                    return true;
                std::array<uint8_t, wire::video_header_size + stream_shard_bytes> payload{};
                if (!wire::encode_video_header(header, payload))
                    return false;
                std::copy(shard.begin(), shard.end(), payload.begin() + wire::video_header_size);
                auto packet =
                    sender.seal(header.shard < header.k ? wire::Type::video : wire::Type::fec, 0,
                                monotonic_ns(), payload);
                if (!packet || !a.send(*packet, b.local_endpoint()))
                    return false;
                Endpoint peer;
                auto count = b.receive(received, peer, std::chrono::milliseconds(200));
                if (!count)
                    return false;
                wire::Header envelope;
                auto plain = receiver.open(std::span(received).first(*count), envelope);
                if (!plain)
                    return false;
                auto decoded_header = wire::decode_video_header(*plain);
                if (!decoded_header)
                    return false;
                auto frame = assembler.push(*decoded_header,
                                            std::span(*plain).subspan(wire::video_header_size),
                                            monotonic_ns());
                if (frame) {
                    if (fed || frame->header.frame_id != id ||
                        frame->bytes.size() != encoded->bytes.size() ||
                        !std::equal(frame->bytes.begin(), frame->bytes.end(),
                                    encoded->bytes.begin()))
                        return false;
                    fed = decoder.submit(frame->bytes, frame->header.frame_id);
                }
                return true;
            });
        if (!sent || !fed)
            throw std::runtime_error("Encrypted codec frame did not survive UDP/FEC loopback");
        std::optional<DecodedFrame> decoded;
        while (!decoded && monotonic_ns() < deadline) {
            decoded = decoder.poll();
            if (!decoded)
                std::this_thread::yield();
        }
        if (!decoded || decoded->frame_id != id || decoded->width != width ||
            decoded->height != height)
            throw std::runtime_error("Loopback decode timed out or dimensions/identity changed");
        if (!bgra_to_nv12(pixels, width * 4, width, height, reference) ||
            psnr(std::span(decoded->nv12).first(width * height),
                 std::span(reference).first(width * height)) < 30)
            throw std::runtime_error("Loopback decoded image quality failed");
        ++completed;
    }
    return completed;
}
static int run() {
    KeyPair client, server;
    std::array<uint8_t, 64> transcript{};
    std::copy(client.public_key.begin(), client.public_key.end(), transcript.begin());
    std::copy(server.public_key.begin(), server.public_key.end(), transcript.begin() + 32);
    PacketCipher sender(1, exchange_keys(client, server.public_key, true, transcript)),
        receiver(1, exchange_keys(server, client.public_key, false, transcript));
    UdpTransport a(Endpoint::parse("127.0.0.1", 0)), b(Endpoint::parse("127.0.0.1", 0));
    std::array<uint8_t, wire::max_datagram> received{};
    Endpoint peer;
    wire::Header envelope;
    for (uint32_t i = 0; i < 400; ++i) {
        wire::PosePacket pose;
        pose.sequence = i;
        pose.sensor_ns = monotonic_ns();
        pose.pose.orientation = Quat::exponential({0, static_cast<double>(i) * 0.0025, 0});
        pose.pose.angular_velocity = {0, 1, 0};
        std::array<uint8_t, wire::pose_size> payload{};
        if (!wire::encode_pose(pose, payload))
            return 1;
        const auto packet = sender.seal(wire::Type::pose, 0, pose.sensor_ns, payload);
        if (!packet || !a.send(*packet, b.local_endpoint()))
            return 2;
        auto count = b.receive(received, peer, std::chrono::milliseconds(1000));
        if (!count)
            return 3;
        auto plain = receiver.open(std::span(received).first(*count), envelope);
        if (!plain)
            return 4;
        auto decoded = wire::decode_pose(*plain);
        if (!decoded || decoded->sequence != i)
            return 5;
    }
    wire::Bytes frame(32017);
    std::iota(frame.begin(), frame.end(), static_cast<uint8_t>(0));
    // This correctness tool also runs while toolchains page from a network
    // share. Production reassembly retains its 33 ms deadline.
    Reassembler assembler(65536, 4, 500'000'000);
    bool complete = false;
    wire::VideoHeader metadata;
    metadata.frame_id = 1;
    metadata.render_ns = monotonic_ns();
    const auto ok = packetize_frame(frame, metadata, 4, [&](const auto &h, auto shard) {
        if (h.shard == 2 || h.shard == 3)
            return true; // Two consecutive losses in every group.
        std::array<uint8_t, wire::video_header_size + stream_shard_bytes> payload{};
        if (!wire::encode_video_header(h, payload))
            return false;
        std::copy(shard.begin(), shard.end(), payload.begin() + wire::video_header_size);
        auto packet = sender.seal(h.shard < h.k ? wire::Type::video : wire::Type::fec, 0,
                                  monotonic_ns(), payload);
        if (!packet || !a.send(*packet, b.local_endpoint()))
            return false;
        auto count = b.receive(received, peer, std::chrono::milliseconds(1000));
        if (!count)
            return false;
        auto plain = receiver.open(std::span(received).first(*count), envelope);
        if (!plain)
            return false;
        auto header = wire::decode_video_header(*plain);
        if (!header)
            return false;
        auto output = assembler.push(*header, std::span(*plain).subspan(wire::video_header_size),
                                     monotonic_ns());
        if (output) {
            complete =
                std::equal(frame.begin(), frame.end(), output->bytes.begin(), output->bytes.end());
        }
        return true;
    });
    if (!ok || !complete)
        return 6;
    const auto decoded_frames = codec_round_trip(sender, receiver, a, b);
    std::cout << "{\"poses\":400,\"video_bytes\":" << frame.size()
              << ",\"recovered_groups\":" << assembler.stats().recovered
              << ",\"transport\":\"encrypted IPv4 loopback UDP\",\"codec_decode\":\"Media "
                 "Foundation software H.264\",\"decoded_frames\":"
              << decoded_frames << "}\n";
    return 0;
}
int main(int argc, char **argv) {
    if (argc != 2 || std::string_view(argv[1]) != "--self-test") {
        std::cerr
            << "Usage: pocketvr_loopback --self-test\nRuns real encrypted UDP pose/video-shard "
               "recovery and actual software H.264 encode/decode with deliberate losses.\n";
        return 2;
    }
    try {
        const auto result = run();
        if (result)
            std::cerr << "Loopback invariant failed at stage " << result << '\n';
        return result;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
