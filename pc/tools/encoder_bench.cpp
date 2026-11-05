// SPDX-License-Identifier: Apache-2.0
#include "encoder/mf.hpp"
#include <pocketvr/transport.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <thread>

using namespace pocketvr;
using namespace pocketvr::driver;
static double percentile(std::vector<double> values, double p) {
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<size_t>(std::ceil(p * static_cast<double>(values.size()))) - 1;
    return values[std::min(index, values.size() - 1)];
}
int main(int argc, char **argv) {
    try {
        EncoderConfig config;
        unsigned frames = 300;
        bool software = false, warp = false, quality = false, stereo_test = false;
        bool machine_readable = false, require_hardware = false;
        std::string output, dump;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--machine-readable") { machine_readable = true; continue; }
            if (arg == "--require-hardware") { require_hardware = true; continue; }
            if (arg == "--help") {
                std::cout << "Usage: pocketvr_encoder_bench [--width 2160] [--height 1080] "
                             "[--frames 300] [--fps 60] [--bitrate 60000000] [--codec h264|hevc] "
                             "[--preset fast|balanced|quality] [--software] [--warp] [--quality] "
                             "[--output metrics.json] [--dump stream.h264] [--stereo-test-pattern] "
                             "[--machine-readable] [--require-hardware]\nMeasures the selected "
                             "real encoder. Optional quality uses the OS software decoder and "
                             "sampled luma PSNR; it is outside timing loops.\n";
                return 0;
            }
            if (arg == "--software") {
                software = true;
                continue;
            }
            if (arg == "--warp") {
                warp = true;
                continue;
            }
            if (arg == "--quality") {
                quality = true;
                continue;
            }
            if (arg == "--stereo-test-pattern") {
                stereo_test = true;
                continue;
            }
            if (i + 1 >= argc)
                throw std::invalid_argument("Missing benchmark option value");
            const std::string value = argv[++i];
            if (arg == "--width")
                config.width = static_cast<uint32_t>(std::stoul(value));
            else if (arg == "--height")
                config.height = static_cast<uint32_t>(std::stoul(value));
            else if (arg == "--frames")
                frames = static_cast<unsigned>(std::stoul(value));
            else if (arg == "--fps")
                config.fps = static_cast<uint32_t>(std::stoul(value));
            else if (arg == "--bitrate")
                config.bitrate = static_cast<uint32_t>(std::stoul(value));
            else if (arg == "--codec") {
                if (value != "h264" && value != "hevc")
                    throw std::invalid_argument("Codec must be h264 or hevc");
                config.codec = value == "hevc" ? Codec::hevc : Codec::h264;
            } else if (arg == "--preset") {
                if (value != "fast" && value != "balanced" && value != "quality")
                    throw std::invalid_argument("Unknown preset");
                config.preset = value;
            } else if (arg == "--output")
                output = value;
            else if (arg == "--dump")
                dump = value;
            else
                throw std::invalid_argument("Unknown benchmark option");
        }
        if (frames < 1 || frames > 3600)
            throw std::invalid_argument("Frames must be 1..3600");
        if (require_hardware && (software || warp))
            throw std::invalid_argument("Hardware benchmark cannot use software/WARP flags");
        Gpu gpu(warp);
        auto encoder = create_encoder(gpu, config, software);
        if (require_hardware && !encoder->hardware())
            throw std::runtime_error("No hardware encoder available for the selected settings");
        std::unique_ptr<MfDecoder> decoder;
        if (quality)
            decoder = std::make_unique<MfDecoder>(config.codec, config.width, config.height);
        wire::Bytes source(static_cast<size_t>(config.width) * config.height * 4),
            reference(static_cast<size_t>(config.width) * config.height * 3 / 2);
        D3D11_TEXTURE2D_DESC description{};
        description.Width = config.width;
        description.Height = config.height;
        description.MipLevels = description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        Com<ID3D11Texture2D> texture;
        check(gpu.device->CreateTexture2D(&description, nullptr, &texture),
              "Benchmark texture creation failed");
        std::vector<double> encode_ms, decode_ms, quality_db;
        uint64_t total_bytes = 0;
        std::ofstream stream;
        if (!dump.empty()) {
            stream.open(dump, std::ios::binary);
            if (!stream)
                throw std::runtime_error("Cannot open bitstream dump");
        }
        const auto beginning = monotonic_ns();
        for (uint64_t id = 1; id <= frames; ++id) {
            for (uint32_t y = 0; y < config.height; ++y)
                for (uint32_t x = 0; x < config.width; ++x) {
                    const auto at = (static_cast<size_t>(y) * config.width + x) * 4;
                    source[at] = static_cast<uint8_t>((x * 255 / config.width + id) % 256);
                    source[at + 1] = static_cast<uint8_t>(y * 255 / config.height);
                    source[at + 2] =
                        static_cast<uint8_t>((((x / 32) + (y / 32) + id / 30) & 1) ? 192 : 48);
                    source[at + 3] = 255;
                    if (stereo_test) {
                        const bool right = x >= config.width / 2, bottom = y >= config.height / 2;
                        source[at] = right && bottom ? 255 : 0;
                        source[at + 1] = (right && !bottom) || (!right && bottom) ? 255 : 0;
                        source[at + 2] = !bottom ? 255 : 0;
                    }
                }
            gpu.context->UpdateSubresource(texture.Get(), 0, nullptr, source.data(),
                                           config.width * 4, 0);
            gpu.context->Flush();
            const auto deadline = monotonic_ns() + 10'000'000'000;
            while (!encoder->submit(texture.Get(), id, id == 1)) {
                (void)encoder->poll();
                if (monotonic_ns() > deadline)
                    throw std::runtime_error("Encoder input deadline exceeded");
                std::this_thread::yield();
            }
            std::optional<EncodedFrame> encoded;
            while (!encoded) {
                encoded = encoder->poll();
                if (monotonic_ns() > deadline)
                    throw std::runtime_error("Encoder output deadline exceeded");
                if (!encoded)
                    std::this_thread::yield();
            }
            if (encoded->frame_id != id)
                throw std::runtime_error("Encoder returned wrong frame identity");
            encode_ms.push_back(encoded->encode_ms);
            total_bytes += encoded->bytes.size();
            if (stream)
                stream.write(reinterpret_cast<const char *>(encoded->bytes.data()),
                             static_cast<std::streamsize>(encoded->bytes.size()));
            if (decoder) {
                const auto start = monotonic_ns();
                if (!decoder->submit(encoded->bytes, id))
                    throw std::runtime_error("Decoder rejected benchmark access unit");
                std::optional<DecodedFrame> decoded;
                while (!decoded) {
                    decoded = decoder->poll();
                    if (monotonic_ns() > deadline)
                        throw std::runtime_error("Decoder output deadline exceeded");
                    if (!decoded)
                        std::this_thread::yield();
                }
                decode_ms.push_back(static_cast<double>(monotonic_ns() - start) * 1e-6);
                if (!bgra_to_nv12(source, config.width * 4, config.width, config.height, reference))
                    throw std::runtime_error("Reference conversion failed");
                wire::Bytes actual, expected;
                actual.reserve(static_cast<size_t>(config.width) * config.height / 16);
                expected.reserve(actual.capacity());
                for (size_t y = 0; y < config.height; y += 4)
                    for (size_t x = 0; x < config.width; x += 4) {
                        actual.push_back(decoded->nv12[y * config.width + x]);
                        expected.push_back(reference[y * config.width + x]);
                    }
                quality_db.push_back(std::min(100.0, psnr(actual, expected)));
            }
        }
        const auto wall_seconds = static_cast<double>(monotonic_ns() - beginning) * 1e-9;
        nlohmann::json report{
            {"report_version", 1},
            {"preset", config.preset},
            {"slices", config.slices},
            {"encoder", encoder->name()},
            {"hardware", encoder->hardware()},
            {"warp", warp},
            {"codec", config.codec == Codec::h264 ? "h264" : "hevc"},
            {"width", config.width},
            {"height", config.height},
            {"frames", frames},
            {"fps", config.fps},
            {"target_bitrate_mbps", static_cast<double>(config.bitrate) / 1e6},
            {"actual_bitrate_mbps",
             static_cast<double>(total_bytes) * 8 * config.fps / frames / 1e6},
            {"encode_ms_p50", percentile(encode_ms, 0.5)},
            {"encode_ms_p95", percentile(encode_ms, 0.95)},
            {"wall_fps_including_synthetic_generation_and_quality", frames / wall_seconds},
            {"gpu_vendor_id", gpu.adapter.VendorId},
            {"gpu_device_id", gpu.adapter.DeviceId},
            {"phone_decode_measured", false},
            {"headset_latency_measured", false}};
        const std::wstring adapter_name(gpu.adapter.Description);
        const auto length =
            WideCharToMultiByte(CP_UTF8, 0, adapter_name.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (length <= 0)
            throw std::runtime_error("GPU description conversion failed");
        std::string adapter_utf8(static_cast<size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, adapter_name.c_str(), -1, adapter_utf8.data(), length,
                            nullptr, nullptr);
        adapter_utf8.pop_back();
        report["gpu_description"] = adapter_utf8;
        if (quality) {
            report["software_decode_ms_p95"] = percentile(decode_ms, 0.95);
            report["sampled_luma_psnr_db_p50_capped_100"] = percentile(quality_db, 0.5);
        }
        if (!output.empty()) {
            std::ofstream file(output);
            if (!file)
                throw std::runtime_error("Cannot write benchmark report");
            file << report.dump(2) << '\n';
        }
        if (machine_readable) std::cout << "POCKETVR_BENCHMARK_JSON:" << report.dump() << '\n';
        else std::cout << report.dump(2) << '\n';
        // Deliver the child-process protocol before tearing down codec/COM objects.
        std::cout.flush();
        if (!std::cout) throw std::runtime_error("Cannot write benchmark report to stdout");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Encoder benchmark failed: " << e.what() << '\n';
        return 1;
    }
}
