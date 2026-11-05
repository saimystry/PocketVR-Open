// SPDX-License-Identifier: Apache-2.0
#include "encoder/mf.hpp"
#include "encoded_dump.hpp"
#include <gtest/gtest.h>
#include <pocketvr/transport.hpp>
#include <thread>
#include <fstream>

using namespace pocketvr;
using namespace pocketvr::driver;
TEST(MediaFoundation, SoftwareH264EncodeAndDecodeActualGpuFrame) {
    Gpu gpu(true);
    EncoderConfig config;
    config.width = config.height = 64;
    config.bitrate = 1'000'000;
    config.preset = "fast";
    auto encoder = create_encoder(gpu, config, true);
    EXPECT_FALSE(encoder->hardware());
    std::array<uint8_t, 64 * 64 * 4> pixels{};
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = pixels[i + 1] = pixels[i + 2] = 128;
        pixels[i + 3] = 255;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = description.Height = 64;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{pixels.data(), 64 * 4, 0};
    Com<ID3D11Texture2D> texture;
    ASSERT_HRESULT_SUCCEEDED(gpu.device->CreateTexture2D(&description, &initial, &texture));
    MfDecoder decoder(Codec::h264, 64, 64);
    const auto directory = std::filesystem::temp_directory_path() /
        ("PocketVR-real-encoded-dump-" + std::to_string(monotonic_ns()));
    EncodedDump capture(directory);
    capture.set_enabled(true);
    wire::Bytes first_access_unit;
    std::array<uint8_t, 64 * 64 * 3 / 2> reference{};
    ASSERT_TRUE(bgra_to_nv12(pixels, 64 * 4, 64, 64, reference));
    for (uint64_t id = 1; id <= 3; ++id) {
        ASSERT_TRUE(encoder->submit(texture.Get(), id, id == 1 || id == 3));
        std::optional<EncodedFrame> output;
        const auto deadline = monotonic_ns() + 5'000'000'000;
        while (!output && monotonic_ns() < deadline) {
            output = encoder->poll();
            if (!output)
                std::this_thread::yield();
        }
        ASSERT_TRUE(output);
        EXPECT_EQ(output->frame_id, id);
        EXPECT_FALSE(output->bytes.empty());
        if (id == 1 || id == 3)
            EXPECT_TRUE(output->idr);
        if (id == 1)
            EXPECT_TRUE(contains_parameter_sets(output->bytes, Codec::h264));
        capture.record(output->bytes, output->idr, Codec::h264);
        if (id == 1) first_access_unit = output->bytes;
        ASSERT_TRUE(decoder.submit(output->bytes, id));
        std::optional<DecodedFrame> decoded;
        while (!decoded && monotonic_ns() < deadline) {
            decoded = decoder.poll();
            if (!decoded)
                std::this_thread::yield();
        }
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->frame_id, id);
        EXPECT_EQ(decoded->width, 64);
        EXPECT_GT(psnr(std::span(decoded->nv12).first(4096), std::span(reference).first(4096)), 35);
    }
    capture.set_enabled(false); // Drain and close before reading the real file.
    const auto status = capture.status();
    ASSERT_GE(status.at("frames").get<uint64_t>(), 1);
    std::ifstream file(status.at("path").get<std::string>(), std::ios::binary);
    wire::Bytes recorded(first_access_unit.size());
    ASSERT_TRUE(file.read(reinterpret_cast<char *>(recorded.data()), static_cast<std::streamsize>(recorded.size())));
    EXPECT_EQ(recorded, first_access_unit);
    MfDecoder replay(Codec::h264, 64, 64);
    ASSERT_TRUE(replay.submit(recorded, 1));
    std::optional<DecodedFrame> replayed;
    const auto deadline = monotonic_ns() + 5'000'000'000;
    while (!replayed && monotonic_ns() < deadline) replayed = replay.poll();
    ASSERT_TRUE(replayed);
    EXPECT_GT(psnr(std::span(replayed->nv12).first(4096), std::span(reference).first(4096)), 35);
    file.close();
    std::filesystem::remove_all(directory);
}
