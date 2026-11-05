// SPDX-License-Identifier: Apache-2.0
#include "compositor.hpp"
#include "encoder/pipeline.hpp"
#include "encoder/mf.hpp"
#include "network.hpp"
#include "identity_store.hpp"
#include <pocketvr/session.hpp>
#include <pocketvr/reassembler.hpp>
#include <pocketvr/discovery.hpp>
#include <pocketvr/telemetry.hpp>
#include <pocketvr/phone_link.hpp>
#include <pocketvr/stream_format.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <cstring>
#include <future>

using namespace pocketvr;
using namespace pocketvr::driver;
namespace {
class CompositorFixture : public testing::Test {
  protected:
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("PocketVR-compositor-test-" + std::to_string(monotonic_ns()));
    std::unique_ptr<State> state;
    std::unique_ptr<Gpu> gpu;
    std::unique_ptr<Compositor> compositor;
    Com<ID3D11Texture2D> sync_texture;
    win::Handle sync;
    Com<ID3D11Texture2D> captured;
    wire::VideoHeader metadata;
    void check_session(bool tcp);
    void check_format_session(bool tcp, int failure);
    void SetUp() override {
        ASSERT_TRUE(std::filesystem::create_directory(directory));
        state = std::make_unique<State>(directory / "pocketvr.json");
        gpu = std::make_unique<Gpu>(true);
        compositor = std::make_unique<Compositor>(*gpu, *state, 64, 64);
        D3D11_TEXTURE2D_DESC description{};
        description.Width = description.Height = 4;
        description.MipLevels = description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BindFlags = D3D11_BIND_RENDER_TARGET;
        description.MiscFlags =
            D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ASSERT_HRESULT_SUCCEEDED(
            gpu->device->CreateTexture2D(&description, nullptr, &sync_texture));
        Com<IDXGIResource1> resource;
        ASSERT_HRESULT_SUCCEEDED(sync_texture.As(&resource));
        HANDLE handle{};
        ASSERT_HRESULT_SUCCEEDED(resource->CreateSharedHandle(
            nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle));
        sync.reset(handle);
        compositor->sink([this](const GpuFrame &frame) {
            captured = frame.texture;
            metadata = frame.metadata;
            return false;
        });
    }
    void TearDown() override {
        captured.Reset();
        sync.reset();
        sync_texture.Reset();
        compositor.reset();
        gpu.reset();
        state.reset();
        std::filesystem::remove(directory / "pocketvr.json");
        std::filesystem::remove(directory);
    }
    vr::IVRDriverDirectModeComponent::SwapTextureSet_t texture(std::array<uint8_t, 4> color,
                                                               uint32_t pid = 1) {
        vr::IVRDriverDirectModeComponent::SwapTextureSetDesc_t description{
            16, 16, DXGI_FORMAT_R8G8B8A8_UNORM, 1};
        vr::IVRDriverDirectModeComponent::SwapTextureSet_t set{};
        compositor->CreateSwapTextureSet(pid, &description, &set);
        if (!set.rSharedTextureHandles[0])
            throw std::runtime_error("Swap allocation failed");
        Com<ID3D11Texture2D> resource;
        check(gpu->device->OpenSharedResource(
                  reinterpret_cast<HANDLE>(static_cast<uintptr_t>(set.rSharedTextureHandles[0])),
                  IID_PPV_ARGS(&resource)),
              "Test could not open swap texture");
        std::array<uint8_t, 16 * 16 * 4> pixels{};
        for (size_t i = 0; i < pixels.size(); i += 4)
            std::copy(color.begin(), color.end(), pixels.begin() + static_cast<ptrdiff_t>(i));
        gpu->context->UpdateSubresource(resource.Get(), 0, nullptr, pixels.data(), 16 * 4, 0);
        gpu->context->Flush();
        return set;
    }
    void layer(vr::SharedTextureHandle_t left, vr::SharedTextureHandle_t right,
               vr::VRTextureBounds_t bounds = {0, 0, 1, 1}) {
        vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t eyes[2]{};
        for (auto &eye : eyes) {
            eye.bounds = bounds;
            eye.mHmdPose = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
        }
        eyes[0].hTexture = left;
        eyes[1].hTexture = right;
        compositor->SubmitLayer(eyes);
    }
    void present() {
        compositor->Present(
            static_cast<vr::SharedTextureHandle_t>(reinterpret_cast<uintptr_t>(sync.get())));
    }
    wire::Bytes pixels() {
        if (!captured)
            throw std::runtime_error("No composited output");
        D3D11_TEXTURE2D_DESC description{};
        captured->GetDesc(&description);
        description.BindFlags = 0;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        description.MiscFlags = 0;
        Com<ID3D11Texture2D> staging;
        check(gpu->device->CreateTexture2D(&description, nullptr, &staging),
              "Test staging allocation failed");
        gpu->context->CopyResource(staging.Get(), captured.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(gpu->context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
              "Test GPU readback failed");
        wire::Bytes bytes(static_cast<size_t>(description.Width) * description.Height * 4);
        for (UINT row = 0; row < description.Height; ++row)
            std::memcpy(bytes.data() + static_cast<size_t>(row) * description.Width * 4,
                        static_cast<const uint8_t *>(mapped.pData) +
                            static_cast<size_t>(row) * mapped.RowPitch,
                        static_cast<size_t>(description.Width) * 4);
        gpu->context->Unmap(staging.Get(), 0);
        return bytes;
    }
};
TEST_F(CompositorFixture, StereoEyesBoundsAndRenderPose) {
    auto left = texture({255, 0, 0, 255}), right = texture({0, 0, 255, 255});
    const auto before_layer = monotonic_ns();
    layer(left.rSharedTextureHandles[0], right.rSharedTextureHandles[0]);
    const auto after_layer = monotonic_ns();
    state->head.publish({{{}, {}, {0, 7, 0}}, monotonic_ns(), 1});
    present();
    ASSERT_NE(captured, nullptr);
    const auto image = pixels();
    const size_t l = (32 * 128 + 32) * 4, r = (32 * 128 + 96) * 4;
    EXPECT_GE(image[l + 2], 254);
    EXPECT_EQ(image[l], 0);
    EXPECT_GE(image[r], 254);
    EXPECT_EQ(image[r + 2], 0);
    EXPECT_EQ(metadata.frame_id, 1);
    EXPECT_NEAR(metadata.render_pose.orientation.w, 1, 1e-8);
    EXPECT_GE(metadata.render_ns, before_layer);
    EXPECT_LE(metadata.render_ns, after_layer);
    EXPECT_DOUBLE_EQ(metadata.render_pose.angular_velocity.norm(), 0);
    const auto output = std::filesystem::path(POCKETVR_TEST_ARTIFACT_ROOT) / "stereo.ppm";
    std::filesystem::create_directories(output.parent_path());
    std::ofstream preview(output, std::ios::binary);
    preview << "P6\n128 64\n255\n";
    for (size_t i = 0; i < image.size(); i += 4) {
        const char rgb[]{static_cast<char>(image[i + 2]), static_cast<char>(image[i + 1]),
                         static_cast<char>(image[i])};
        preview.write(rgb, 3);
    }
}
TEST_F(CompositorFixture, RecenteredRenderPoseReturnsToPhoneSensorCoordinates) {
    auto left = texture({255, 0, 0, 255}), right = texture({0, 0, 255, 255});
    const auto local = Quat::exponential({0.2, 0.3, -0.1});
    const double origin = 1.8;
    const auto q = (Quat::exponential({0, origin, 0}) * local).unit();
    state->recenter_yaw.store(origin);
    vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t eyes[2]{};
    for (auto &eye : eyes) {
        eye.bounds = {0, 0, 1, 1};
        eye.mHmdPose = {{{static_cast<float>(1 - 2 * (q.y*q.y + q.z*q.z)),
                          static_cast<float>(2 * (q.x*q.y - q.z*q.w)),
                          static_cast<float>(2 * (q.x*q.z + q.y*q.w)), 0},
                         {static_cast<float>(2 * (q.x*q.y + q.z*q.w)),
                          static_cast<float>(1 - 2 * (q.x*q.x + q.z*q.z)),
                          static_cast<float>(2 * (q.y*q.z - q.x*q.w)), 0},
                         {static_cast<float>(2 * (q.x*q.z - q.y*q.w)),
                          static_cast<float>(2 * (q.y*q.z + q.x*q.w)),
                          static_cast<float>(1 - 2 * (q.x*q.x + q.y*q.y)), 0}}};
    }
    eyes[0].hTexture = left.rSharedTextureHandles[0];
    eyes[1].hTexture = right.rSharedTextureHandles[0];
    compositor->SubmitLayer(eyes);
    state->recenter_yaw.store(-0.7); // A queued layer keeps its original origin.
    present();
    ASSERT_NE(captured, nullptr);
    EXPECT_NEAR((metadata.render_pose.orientation.rotate({0, 0, -1}) -
                 local.rotate({0, 0, -1})).norm(), 0, 1e-7);
}
TEST_F(CompositorFixture, IndependentEyeBoundsRestoreUpsideDownSource) {
    auto left = texture({255, 0, 0, 255}), right = texture({255, 0, 0, 255});
    for (size_t eye = 0; eye < 2; ++eye) {
        const auto handle = (eye ? right : left).rSharedTextureHandles[0];
        Com<ID3D11Texture2D> source;
        ASSERT_HRESULT_SUCCEEDED(gpu->device->OpenSharedResource(
            reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)), IID_PPV_ARGS(&source)));
        std::array<uint8_t, 16 * 16 * 4> image{};
        for (size_t y = 0; y < 16; ++y)
            for (size_t x = 0; x < 16; ++x) {
                const bool top = eye ? y >= 8 : y < 8;
                const auto at = (y * 16 + x) * 4;
                image[at] = top ? 255 : 0;
                image[at + 1] = top ? 0 : 255;
                image[at + 3] = 255;
            }
        gpu->context->UpdateSubresource(source.Get(), 0, nullptr, image.data(), 16 * 4, 0);
    }
    gpu->context->Flush();
    vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t eyes[2]{};
    for (auto &eye : eyes)
        eye.mHmdPose = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    eyes[0].hTexture = left.rSharedTextureHandles[0];
    eyes[0].bounds = {0, 0, 1, 1};
    eyes[1].hTexture = right.rSharedTextureHandles[0];
    eyes[1].bounds = {0, 1, 1, 0};
    compositor->SubmitLayer(eyes);
    present();
    const auto output = pixels();
    for (size_t eye = 0; eye < 2; ++eye) {
        const auto top = (16 * 128 + eye * 64 + 32) * 4;
        const auto bottom = (48 * 128 + eye * 64 + 32) * 4;
        EXPECT_GE(output[top + 2], 254);
        EXPECT_EQ(output[top + 1], 0);
        EXPECT_EQ(output[bottom + 2], 0);
        EXPECT_GE(output[bottom + 1], 254);
    }
}
TEST_F(CompositorFixture, EncoderWorkerStaysWithinTheEncoderBitrateLimit) {
    class Limited final : public IVideoEncoder {
      public:
        bool submit(ID3D11Texture2D *, uint64_t, bool) override { return false; }
        std::optional<EncodedFrame> poll() override { return {}; }
        void bitrate(uint32_t value) override {
            if (value > 100'000'000)
                throw std::runtime_error("Encoder refused this bitrate");
        }
        uint32_t max_bitrate() const override { return 100'000'000; }
        std::string name() const override { return "Test limit"; }
        bool hardware() const override { return false; }
    };
    auto pipeline = EncoderPipeline::start(
        *compositor, [] { return std::unique_ptr<IVideoEncoder>(std::make_unique<Limited>()); },
        [](EncodedFrame &&, const wire::VideoHeader &) { return true; }, 60'000'000);
    pipeline->bitrate(120'000'000);
    const auto deadline = monotonic_ns() + 3'000'000'000;
    while (pipeline->stats().accepted_bitrate != 100'000'000 && monotonic_ns() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_EQ(pipeline->stats().accepted_bitrate, 100'000'000U);
    EXPECT_FALSE(pipeline->stats().bitrate_rejected); // Later changes must still work.
    pipeline->stop();
}
TEST_F(CompositorFixture, EncoderWorkerWakesForNewFrameWithoutWaitingForATimerTick) {
    class Recorder final : public IVideoEncoder {
        std::atomic<uint64_t> &submitted_;
        std::optional<uint64_t> id_;

      public:
        explicit Recorder(std::atomic<uint64_t> &submitted) : submitted_(submitted) {}
        bool submit(ID3D11Texture2D *, uint64_t id, bool) override {
            id_ = id;
            submitted_.store(monotonic_ns());
            return true;
        }
        std::optional<EncodedFrame> poll() override {
            if (!id_)
                return {};
            EncodedFrame frame{};
            frame.frame_id = *id_;
            frame.idr = true;
            frame.bytes = {1};
            id_.reset();
            return frame;
        }
        void bitrate(uint32_t) override {}
        std::string name() const override {
            return "Test recorder";
        }
        bool hardware() const override {
            return false;
        }
    };
    std::atomic<uint64_t> submitted{};
    auto pipeline = EncoderPipeline::start(
        *compositor, [&]() -> std::unique_ptr<IVideoEncoder> { return std::make_unique<Recorder>(submitted); },
        [](EncodedFrame &&, const wire::VideoHeader &) { return true; }, 1'000'000);
    const auto ready_by = monotonic_ns() + 5'000'000'000;
    while (!pipeline->stats().ready && monotonic_ns() < ready_by)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_TRUE(pipeline->stats().ready);
    const auto left = texture({255, 0, 0, 255}), right = texture({0, 0, 255, 255});
    std::vector<uint64_t> delays;
    for (unsigned frame = 0; frame < 40; ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20)); // Let the worker go idle.
        layer(left.rSharedTextureHandles[0], right.rSharedTextureHandles[0]);
        const auto start = monotonic_ns();
        present();
        const auto deadline = start + 500'000'000;
        while (submitted.load() < start && monotonic_ns() < deadline)
            std::this_thread::yield();
        ASSERT_GE(submitted.load(), start);
        delays.push_back(submitted.load() - start);
    }
    std::sort(delays.begin(), delays.end());
    // Includes Present itself. A worker that only wakes on its timer takes far longer.
    EXPECT_LT(delays[delays.size() / 2], 1'000'000U);
    pipeline->stop();
}
TEST_F(CompositorFixture, EncoderWorkerPreservesMetadataAndRecoversFromInFlightFailure) {
    class FailingEncoder final : public IVideoEncoder {
        bool submitted_{};

      public:
        bool submit(ID3D11Texture2D *, uint64_t, bool) override {
            submitted_ = true;
            return true;
        }
        std::optional<EncodedFrame> poll() override {
            if (submitted_)
                throw std::runtime_error("Injected encoder reset");
            return {};
        }
        void bitrate(uint32_t) override {}
        std::string name() const override {
            return "Test fault";
        }
        bool hardware() const override {
            return false;
        }
    };
    EncoderConfig config;
    config.width = 128;
    config.height = 64;
    config.bitrate = 1'000'000;
    std::atomic<unsigned> creations{};
    std::mutex result_mutex;
    std::vector<wire::VideoHeader> headers;
    std::vector<bool> keyframes;
    auto pipeline = EncoderPipeline::start(
        *compositor,
        [&]() -> std::unique_ptr<IVideoEncoder> {
            if (creations.fetch_add(1) == 0)
                return std::make_unique<FailingEncoder>();
            return create_encoder(*gpu, config, true);
        },
        [&](EncodedFrame &&frame, const wire::VideoHeader &header) {
            std::lock_guard lock(result_mutex);
            headers.push_back(header);
            keyframes.push_back(frame.idr);
            return !frame.bytes.empty() && frame.frame_id == header.frame_id;
        },
        config.bitrate);
    auto wait = [&](auto predicate) {
        const auto deadline = monotonic_ns() + 5'000'000'000;
        while (!predicate() && monotonic_ns() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return predicate();
    };
    ASSERT_TRUE(wait([&] { return pipeline->stats().ready; }));
    pipeline->bitrate(1'500'000);
    ASSERT_TRUE(wait([&] { return pipeline->stats().accepted_bitrate == 1'500'000; }));
    const auto left = texture({255, 0, 0, 255}), right = texture({0, 0, 255, 255});
    layer(left.rSharedTextureHandles[0], right.rSharedTextureHandles[0]);
    present();
    ASSERT_TRUE(wait([&] { return pipeline->stats().resets == 1 && pipeline->stats().ready; }));
    EXPECT_EQ(pipeline->stats().backend, EncoderBackend::mf_software);
    EXPECT_FALSE(pipeline->stats().hardware);
    ASSERT_TRUE(wait([&] { return pipeline->stats().accepted_bitrate == 1'500'000; }));
    for (uint64_t frame = 2; frame <= 5; ++frame) {
        pipeline->request_idr();
        layer(left.rSharedTextureHandles[0], right.rSharedTextureHandles[0]);
        present();
        ASSERT_TRUE(wait([&] { return pipeline->stats().encoded == frame - 1; }));
    }
    EXPECT_EQ(pipeline->stats().dropped, 1);
    pipeline.reset();
    {
        std::lock_guard lock(result_mutex);
        ASSERT_EQ(headers.size(), 4);
        for (size_t i = 0; i < headers.size(); ++i) {
            EXPECT_EQ(headers[i].frame_id, i + 2);
            EXPECT_TRUE(keyframes[i]);
            EXPECT_EQ(headers[i].flags, 1);
            EXPECT_NEAR(headers[i].render_pose.orientation.w, 1, 1e-8);
        }
    }
    // Stopping removes the sink and avoids GPU composition when nobody consumes it.
    for (int i = 0; i < 4; ++i) {
        layer(left.rSharedTextureHandles[0], right.rSharedTextureHandles[0]);
        present();
    }
    EXPECT_EQ(state->frames.load(), 5);
}
TEST_F(CompositorFixture, RejectedLiveBitrateDoesNotResetOrStopVideo) {
    struct RejectingEncoder final : IVideoEncoder {
        std::atomic<unsigned> &attempts;
        std::optional<EncodedFrame> output;
        explicit RejectingEncoder(std::atomic<unsigned> &value) : attempts(value) {}
        bool submit(ID3D11Texture2D *, uint64_t id, bool force) override {
            output = EncodedFrame{id, force, {1, 2, 3}, 1};
            return true;
        }
        std::optional<EncodedFrame> poll() override {
            return std::exchange(output, {});
        }
        void bitrate(uint32_t) override {
            ++attempts;
            throw std::runtime_error("Not modifiable");
        }
        std::string name() const override {
            return "Test immutable bitrate";
        }
        bool hardware() const override {
            return false;
        }
    };
    std::atomic<unsigned> attempts{};
    std::atomic<unsigned> tapped{};
    auto pipeline = EncoderPipeline::start(
        *compositor, [&] { return std::make_unique<RejectingEncoder>(attempts); },
        [](EncodedFrame &&, const wire::VideoHeader &) { return true; }, 60'000'000,
        [&](const EncodedFrame &) { ++tapped; throw std::runtime_error("Injected diagnostic failure"); });
    const auto wait = [&](auto predicate) {
        const auto deadline = monotonic_ns() + 3'000'000'000;
        while (!predicate() && monotonic_ns() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return predicate();
    };
    ASSERT_TRUE(wait([&] { return pipeline->stats().ready; }));
    pipeline->bitrate(48'000'000);
    ASSERT_TRUE(wait([&] { return pipeline->stats().bitrate_rejected; }));
    EXPECT_EQ(pipeline->stats().accepted_bitrate, 60'000'000U);
    pipeline->bitrate(40'000'000);
    const auto image = texture({40, 80, 120, 255});
    layer(image.rSharedTextureHandles[0], image.rSharedTextureHandles[0]);
    present();
    ASSERT_TRUE(wait([&] { return pipeline->stats().encoded == 1; }));
    EXPECT_EQ(pipeline->stats().resets, 0);
    EXPECT_EQ(attempts.load(), 1);
    EXPECT_EQ(tapped.load(), 1); // Optional capture failures cannot reset video.
}
TEST_F(CompositorFixture, EncoderBackpressureKeepsNewestUnacceptedFrame) {
    struct SlowEncoder final : IVideoEncoder {
        std::atomic<bool> &allow;
        std::atomic<uint64_t> &attempted;
        std::optional<EncodedFrame> output;
        SlowEncoder(std::atomic<bool> &a, std::atomic<uint64_t> &b) : allow(a), attempted(b) {}
        bool submit(ID3D11Texture2D *, uint64_t id, bool) override {
            attempted.store(id);
            if (!allow.load())
                return false;
            output = EncodedFrame{};
            output->frame_id = id;
            output->idr = true;
            output->bytes = {1};
            return true;
        }
        std::optional<EncodedFrame> poll() override {
            return std::exchange(output, {});
        }
        void bitrate(uint32_t) override {}
        std::string name() const override {
            return "Backpressure test";
        }
        bool hardware() const override {
            return false;
        }
    };
    std::atomic<bool> allow{};
    std::atomic<uint64_t> attempted{};
    std::atomic<uint64_t> output_id{};
    auto pipeline = EncoderPipeline::start(
        *compositor, [&] { return std::make_unique<SlowEncoder>(allow, attempted); },
        [&](EncodedFrame &&frame, const wire::VideoHeader &) {
            output_id.store(frame.frame_id);
            return true;
        });
    auto wait = [&](auto predicate) {
        const auto deadline = monotonic_ns() + 2'000'000'000;
        while (!predicate() && monotonic_ns() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return predicate();
    };
    ASSERT_TRUE(wait([&] { return pipeline->stats().ready; }));
    const auto image = texture({40, 80, 120, 255});
    layer(image.rSharedTextureHandles[0], image.rSharedTextureHandles[0]);
    present();
    ASSERT_TRUE(wait([&] { return attempted.load(); }));
    for (int i = 0; i < 2; ++i) {
        layer(image.rSharedTextureHandles[0], image.rSharedTextureHandles[0]);
        present();
    }
    ASSERT_TRUE(wait([&] { return attempted.load() == 3; }));
    allow.store(true);
    ASSERT_TRUE(wait([&] { return output_id.load() == 3; }));
    EXPECT_EQ(pipeline->stats().dropped, 2);
    EXPECT_EQ(pipeline->stats().encoded, 1);
}
TEST_F(CompositorFixture, FirstUsePairingNeedsDesktopCodeAndPhoneApprovalAndForgetRevokes) {
    const auto identity_directory = directory / "pairing-identities";
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code ignored;
            for (const auto *name : {"identity.dpapi", "paired.json", "paired.lock"})
                std::filesystem::remove(directory / name, ignored);
            std::filesystem::remove(directory, ignored);
        }
    } cleanup{identity_directory};
    Identity phone;
    NetworkConfig config;
    config.addresses = {Endpoint::parse("127.0.0.1", 0)};
    config.control_port = config.media_port = 0;
    config.width = 128;
    config.height = 64;
    config.software_only = true;
    config.identity_directory = identity_directory;
    NetworkServer network(*state, *gpu, *compositor, config);
    const auto endpoint =
        Endpoint::parse("127.0.0.1", network.endpoints()[0].at("control_port").get<uint16_t>());
    auto wait = [&](auto predicate) {
        const auto deadline = monotonic_ns() + 5'000'000'000;
        while (!predicate() && monotonic_ns() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return predicate();
    };
    PairingPolicy policy;
    policy.allow_new = true;
    policy.approve = [](const Key &, StopToken) { return true; };
    EXPECT_TRUE(network.request({{"method", "pairing.list"}}).at("peers").empty());
    const auto hello = [] {
        return nlohmann::json{
            {"type", "hello"},
            {"protocol", wire::session_protocol},
            {"capabilities",
             {{"model", "First-use test phone"},
              {"codecs", nlohmann::json::array(
                             {{{"mime", "video/avc"},
                               {"sizes", nlohmann::json::array(
                                             {{{"width", 128}, {"height", 64}, {"fps", 60}}})}}})}}}};
    };
    auto stream = TcpStream::connect(endpoint);
    const auto material = authenticate_session(*stream, PeerRole::phone, phone, policy);
    ControlChannel channel(*stream, material);
    channel.send(hello());
    const auto ready = channel.receive(std::chrono::milliseconds(1000));
    ASSERT_TRUE(ready);
    EXPECT_EQ(ready->at("type"), "ready");
    EXPECT_EQ(network.request({{"method", "pairing.list"}}).at("peers").size(), 1);
    channel.send({{"type", "ping"}});
    ASSERT_TRUE(channel.receive(std::chrono::milliseconds(1000)));
    // Forget ends the session and clears the record.
    network.request({{"method", "pairing.forget"}, {"identity", win::key_hex(phone.public_key())}});
    ASSERT_TRUE(wait([&] {
        return !network.request({{"method", "network.status"}}).at("control_active").get<bool>();
    }));
    EXPECT_TRUE(network.request({{"method", "pairing.list"}}).at("peers").empty());
    // A phone that still remembers this PC simply connects again.
    auto retry = TcpStream::connect(endpoint);
    policy.expected_identity = material.peer_identity;
    const auto again = authenticate_session(*retry, PeerRole::phone, phone, policy);
    ControlChannel second(*retry, again);
    second.send(hello());
    ASSERT_TRUE(second.receive(std::chrono::milliseconds(1000)));
    EXPECT_EQ(network.request({{"method", "pairing.list"}}).at("peers").size(), 1);
}
TEST_F(CompositorFixture, SavedSettingsFromAnOlderVersionKeepTheirValues) {
    const auto path = directory / "older-settings.json";
    auto saved = Settings().values();
    saved["video"]["bitrate_mbps"] = 77;
    saved["video"].erase("resolution");
    saved["display"].erase("eye_size");
    std::ofstream(path) << saved.dump();
    {
        State older(path);
        const auto values = older.request({{"method", "settings.get"}}).at("values");
        EXPECT_EQ(values.at("video").at("bitrate_mbps"), 77);
        EXPECT_EQ(values.at("video").at("resolution"), "auto");
        EXPECT_DOUBLE_EQ(values.at("display").at("eye_size").get<double>(), 1.35);
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}
TEST_F(CompositorFixture, ProfilesApplySaveAndSurviveARestart) {
    const auto saved_file = directory / "profiles.json";
    EXPECT_FALSE(state->request({{"method", "profiles.list"}}).at("built_in").empty());
    const auto war = state->request({{"method", "profiles.apply"}, {"name", "War Thunder"}});
    EXPECT_EQ(war.at("values").at("video").at("codec"), "hevc");
    EXPECT_EQ(war.at("values").at("video").at("bitrate_mbps"), 80);
    state->request({{"method", "settings.set"}, {"key", "display.eye_size"}, {"value", 1.2}});
    state->request({{"method", "profiles.save"}, {"name", "Mine"}});
    state->request({{"method", "settings.set"}, {"key", "display.eye_size"}, {"value", 1.4}});
    const auto mine = state->request({{"method", "profiles.apply"}, {"name", "Mine"}});
    EXPECT_DOUBLE_EQ(mine.at("values").at("display").at("eye_size").get<double>(), 1.2);
    EXPECT_THROW(state->request({{"method", "profiles.save"}, {"name", "War Thunder"}}), std::invalid_argument);
    state->request({{"method", "profiles.delete"}, {"name", "Mine"}});
    EXPECT_TRUE(state->request({{"method", "profiles.list"}}).at("saved").empty());
    std::error_code ignored;
    std::filesystem::remove(saved_file, ignored);
}
TEST_F(CompositorFixture, ANewPhoneOnTheNetworkNeedsOneClickOnThePc) {
    const auto identity_directory = directory / "approval-identities";
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code ignored;
            for (const auto *name : {"identity.dpapi", "paired.json", "paired.lock"})
                std::filesystem::remove(directory / name, ignored);
            std::filesystem::remove(directory, ignored);
        }
    } cleanup{identity_directory};
    NetworkConfig config;
    config.addresses = {Endpoint::parse("127.0.0.1", 0)};
    config.control_port = config.media_port = 0;
    config.width = 128;
    config.height = 64;
    config.software_only = true;
    config.advertise = false;
    config.trust_loopback = false;
    config.identity_directory = identity_directory;
    NetworkServer network(*state, *gpu, *compositor, config);
    const auto endpoint =
        Endpoint::parse("127.0.0.1", network.endpoints()[0].at("control_port").get<uint16_t>());
    PairingPolicy policy;
    policy.allow_new = true;
    policy.approve = [](const Key &, StopToken) { return true; };
    const auto connect = [&](Identity &phone) {
        auto stream = TcpStream::connect(endpoint);
        const auto material = authenticate_session(*stream, PeerRole::phone, phone, policy);
        ControlChannel channel(*stream, material);
        channel.send({{"type", "hello"},
                      {"protocol", wire::session_protocol},
                      {"capabilities",
                       {{"model", "Approval test phone"},
                        {"codecs", nlohmann::json::array({{{"mime", "video/avc"},
                            {"sizes", nlohmann::json::array({{{"width", 128}, {"height", 64}, {"fps", 60}}})}}})}}}});
        std::optional<nlohmann::json> ready;
        const auto deadline = monotonic_ns() + 5'000'000'000;
        while (!ready && monotonic_ns() < deadline)
            ready = channel.receive(std::chrono::milliseconds(100));
        return ready && ready->at("type") == "ready";
    };
    const auto answer_when_asked = [&](bool allow) {
        return std::async(std::launch::async, [&network, allow] {
            const auto deadline = monotonic_ns() + 5'000'000'000;
            while (monotonic_ns() < deadline) {
                const auto pending = network.request({{"method", "network.status"}}).at("pending_phone");
                if (!pending.is_null()) {
                    // An answer for a phone that is not on screen must change nothing.
                    if (network.request({{"method", "pairing.answer"}, {"allow", true},
                                         {"phone", std::string(64, '0')}}).at("ok").get<bool>())
                        return false;
                    network.request({{"method", "pairing.answer"}, {"allow", allow},
                                     {"phone", pending.at("key")}});
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        });
    };
    Identity first, refused, allowed;
    {
        // With the PC app closed, nobody can see which phone was trusted.
        auto answered = answer_when_asked(false);
        EXPECT_THROW(connect(refused), std::runtime_error);
        EXPECT_TRUE(answered.get());
        EXPECT_TRUE(network.request({{"method", "pairing.list"}}).at("peers").empty());
    }
    network.request({{"method", "network.status"}, {"desktop", true}});
    EXPECT_TRUE(connect(first)); // With it open, the first phone ever needs nothing.
    {
        auto answered = answer_when_asked(false);
        EXPECT_THROW(connect(refused), std::runtime_error);
        EXPECT_TRUE(answered.get());
    }
    EXPECT_EQ(network.request({{"method", "pairing.list"}}).at("peers").size(), 1);
    auto answered = answer_when_asked(true);
    EXPECT_TRUE(connect(allowed));
    EXPECT_TRUE(answered.get());
    EXPECT_EQ(network.request({{"method", "pairing.list"}}).at("peers").size(), 2);
}
TEST_F(CompositorFixture, APhoneWithoutHevcStillGetsAPicture) {
    const auto identity_directory = directory / "codec-identities";
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code ignored;
            for (const auto *name : {"identity.dpapi", "paired.json", "paired.lock"})
                std::filesystem::remove(directory / name, ignored);
            std::filesystem::remove(directory, ignored);
        }
    } cleanup{identity_directory};
    state->request({{"method", "settings.set"}, {"key", "video.codec"}, {"value", "hevc"}});
    NetworkConfig config;
    config.addresses = {Endpoint::parse("127.0.0.1", 0)};
    config.control_port = config.media_port = 0;
    config.width = 128;
    config.height = 64;
    config.software_only = true;
    config.advertise = false;
    config.identity_directory = identity_directory;
    NetworkServer network(*state, *gpu, *compositor, config);
    PairingPolicy policy;
    policy.allow_new = true;
    policy.approve = [](const Key &, StopToken) { return true; };
    Identity phone;
    auto stream = TcpStream::connect(
        Endpoint::parse("127.0.0.1", network.endpoints()[0].at("control_port").get<uint16_t>()));
    const auto material = authenticate_session(*stream, PeerRole::phone, phone, policy);
    ControlChannel channel(*stream, material);
    channel.send({{"type", "hello"},
                  {"protocol", wire::session_protocol},
                  {"capabilities",
                   {{"model", "H.264 only phone"},
                    {"codecs", nlohmann::json::array({{{"mime", "video/avc"},
                        {"sizes", nlohmann::json::array({{{"width", 128}, {"height", 64}, {"fps", 60}}})}}})}}}});
    std::optional<nlohmann::json> ready;
    const auto deadline = monotonic_ns() + 5'000'000'000;
    while (!ready && monotonic_ns() < deadline)
        ready = channel.receive(std::chrono::milliseconds(100));
    ASSERT_TRUE(ready);
    ASSERT_EQ(ready->at("type"), "ready");
    EXPECT_EQ(ready->at("stream").at("codec"), "h264");
}
TEST_F(CompositorFixture, ChangingALensMeasurementTakesEffect) {
    const auto before = state->viewer_geometry()->screen[0];
    const auto changed = state->request({{"method", "settings.set"},
                                         {"key", "lens.screen_to_lens_m"},
                                         {"value", 0.05}});
    EXPECT_EQ(changed.at("values").at("lens").at("profile"), "manual");
    EXPECT_NE(state->viewer_geometry()->screen[0].up, before.up);
}
TEST_F(CompositorFixture, ManualResolutionIsUsedOnlyWhenThePhoneCanDecodeIt) {
    const auto identity_directory = directory / "resolution-identities";
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code ignored;
            for (const auto *name : {"identity.dpapi", "paired.json", "paired.lock"})
                std::filesystem::remove(directory / name, ignored);
            std::filesystem::remove(directory, ignored);
        }
    } cleanup{identity_directory};
    state->request({{"method", "settings.set"}, {"key", "video.resolution"}, {"value", "1280x640"}});
    NetworkConfig config;
    config.addresses = {Endpoint::parse("127.0.0.1", 0)};
    config.control_port = config.media_port = 0;
    config.width = 128;
    config.height = 64;
    config.software_only = true;
    config.advertise = false;
    config.identity_directory = identity_directory;
    NetworkServer network(*state, *gpu, *compositor, config);
    const auto endpoint =
        Endpoint::parse("127.0.0.1", network.endpoints()[0].at("control_port").get<uint16_t>());
    PairingPolicy policy;
    policy.allow_new = true;
    policy.approve = [](const Key &, StopToken) { return true; };
    const auto stream_width = [&](nlohmann::json sizes) {
        Identity phone;
        auto stream = TcpStream::connect(endpoint);
        const auto material = authenticate_session(*stream, PeerRole::phone, phone, policy);
        ControlChannel channel(*stream, material);
        channel.send({{"type", "hello"},
                      {"protocol", wire::session_protocol},
                      {"capabilities",
                       {{"model", "Resolution test phone"},
                        {"codecs", nlohmann::json::array({{{"mime", "video/avc"}, {"sizes", sizes}}})}}}});
        std::optional<nlohmann::json> ready;
        const auto deadline = monotonic_ns() + 5'000'000'000;
        while (!ready && monotonic_ns() < deadline)
            ready = channel.receive(std::chrono::milliseconds(100));
        return ready && ready->at("type") == "ready" ? ready->at("stream").at("width").get<uint32_t>() : 0U;
    };
    const nlohmann::json full{{{"width", 128}, {"height", 64}, {"fps", 60}}};
    auto both = full;
    both.push_back({{"width", 1280}, {"height", 640}, {"fps", 60}});
    EXPECT_EQ(stream_width(both), 1280U);
    EXPECT_EQ(stream_width(full), 128U);
}
TEST_F(CompositorFixture, OnlyAnAcceptedPhoneCanReplaceTheLiveSession) {
    const auto identity_directory = directory / "replacement-identities";
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code ignored;
            for (const auto *name : {"identity.dpapi", "paired.json", "paired.lock"})
                std::filesystem::remove(directory / name, ignored);
            std::filesystem::remove(directory, ignored);
        }
    } cleanup{identity_directory};
    NetworkConfig config;
    config.addresses = {Endpoint::parse("127.0.0.1", 0)};
    config.control_port = config.media_port = 0;
    config.width = 128;
    config.height = 64;
    config.software_only = true;
    config.advertise = false;
    config.identity_directory = identity_directory;
    NetworkServer network(*state, *gpu, *compositor, config);
    const auto endpoint =
        Endpoint::parse("127.0.0.1", network.endpoints()[0].at("control_port").get<uint16_t>());
    PairingPolicy policy;
    policy.allow_new = true;
    policy.approve = [](const Key &, StopToken) { return true; };
    const auto hello = [](const char *mime) {
        return nlohmann::json{
            {"type", "hello"},
            {"protocol", wire::session_protocol},
            {"capabilities",
             {{"model", "Replacement test phone"},
              {"codecs", nlohmann::json::array(
                             {{{"mime", mime},
                               {"sizes", nlohmann::json::array(
                                             {{{"width", 128}, {"height", 64}, {"fps", 60}}})}}})}}}};
    };
    const auto answers = [](ControlChannel &channel) {
        try {
            channel.send({{"type", "ping"}});
            const auto deadline = monotonic_ns() + 2'000'000'000;
            while (monotonic_ns() < deadline)
                if (const auto reply = channel.receive(std::chrono::milliseconds(50));
                    reply && reply->value("type", "") == "pong")
                    return true;
        } catch (const std::exception &) {
        }
        return false;
    };
    const auto settle = [] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); };

    Identity first_phone;
    auto first_stream = TcpStream::connect(endpoint);
    const auto first_material =
        authenticate_session(*first_stream, PeerRole::phone, first_phone, policy);
    ControlChannel first(*first_stream, first_material);
    first.send(hello("video/avc"));
    const auto ready = first.receive(std::chrono::milliseconds(1000));
    ASSERT_TRUE(ready);
    ASSERT_EQ(ready->at("type"), "ready");
    ASSERT_TRUE(answers(first));

    // A connection that says nothing, such as a port scan, changes nothing.
    auto silent = TcpStream::connect(endpoint);
    settle();
    EXPECT_TRUE(answers(first));

    // Neither does one that sends rubbish and leaves.
    {
        auto rubbish = TcpStream::connect(endpoint);
        const std::array<uint8_t, 4> bytes{1, 2, 3, 4};
        (void)rubbish->send_record(bytes, std::chrono::milliseconds(500));
        settle();
    }
    EXPECT_TRUE(answers(first));

    // A real handshake is not enough: the PC must also be able to serve the phone.
    {
        Identity unsupported_phone;
        auto stream = TcpStream::connect(endpoint);
        const auto material =
            authenticate_session(*stream, PeerRole::phone, unsupported_phone, policy);
        ControlChannel channel(*stream, material);
        channel.send(hello("video/none"));
        const auto refused = channel.receive(std::chrono::milliseconds(1000));
        ASSERT_TRUE(refused);
        EXPECT_EQ(refused->at("type"), "disconnect");
    }
    settle();
    EXPECT_TRUE(answers(first));

    // An accepted phone does replace the session, even with a stray still open.
    Identity second_phone;
    auto second_stream = TcpStream::connect(endpoint);
    const auto second_material =
        authenticate_session(*second_stream, PeerRole::phone, second_phone, policy);
    ControlChannel second(*second_stream, second_material);
    second.send(hello("video/avc"));
    std::optional<nlohmann::json> replaced;
    const auto replace_deadline = monotonic_ns() + 5'000'000'000;
    while (!replaced && monotonic_ns() < replace_deadline)
        replaced = second.receive(std::chrono::milliseconds(100));
    ASSERT_TRUE(replaced);
    EXPECT_EQ(replaced->at("type"), "ready");
    EXPECT_TRUE(answers(second));
    EXPECT_FALSE(answers(first));
    const auto status = network.request({{"method", "network.status"}});
    EXPECT_TRUE(status.at("control_active").get<bool>());
    EXPECT_FALSE(status.at("session").is_null());
}
TEST_F(CompositorFixture, AuthenticatedDriverSessionPoseToStereoEncodedUdpAndDecode) {
    check_session(false);
}
TEST_F(CompositorFixture, AuthenticatedUsbTcpSessionRejectsUnboundSocketAndDeliversStereo) {
    check_session(true);
}
TEST_F(CompositorFixture, FormatChangesResolutionAndFpsWithoutLosingUdpTracking) {
    check_format_session(false,0);
}
TEST_F(CompositorFixture, FormatChangesResolutionAndFpsWithoutReplacingTcpSession) {
    check_format_session(true,0);
}
TEST_F(CompositorFixture, RejectedFormatRestoresPreviousEncoderAndDisablesRetryLoop) {
    check_format_session(false,1);
}
TEST_F(CompositorFixture, MissingFormatAcknowledgmentRollsBackWithTrackingAlive) {
    check_format_session(false,2);
}
TEST_F(CompositorFixture, MismatchedPhoneAppIsToldToUpdate) {
    const auto identity_directory = directory / "identities";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            for (const auto *name : {"identity.dpapi", "paired.json", "paired.lock"})
                std::filesystem::remove(path / name, ignored);
            std::filesystem::remove(path, ignored);
        }
    } cleanup{identity_directory};
    Identity phone;
    win::IdentityStore identities(identity_directory);
    identities.trust(phone.public_key(), "Version test");
    NetworkConfig config;
    config.addresses = {Endpoint::parse("127.0.0.1", 0)};
    config.control_port = config.media_port = 0;
    config.width = 128;
    config.height = 64;
    config.software_only = true;
    config.advertise = false;
    config.identity_directory = identity_directory;
    NetworkServer network(*state, *gpu, *compositor, config);
    auto stream = TcpStream::connect(Endpoint::parse(
        "127.0.0.1", network.endpoints().at(0).at("control_port").get<uint16_t>()));
    PairingPolicy policy;
    policy.expected_identity = identities.identity().public_key();
    const auto material = authenticate_session(*stream, PeerRole::phone, phone, policy);
    ControlChannel channel(*stream, material);
    channel.send({{"type", "hello"}, {"protocol", wire::session_protocol - 1},
                  {"capabilities", {{"model", "Old app"}, {"codecs", nlohmann::json::array()}}}});
    const auto reply = channel.receive(std::chrono::milliseconds(1000));
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->at("type"), "disconnect");
    EXPECT_NE(reply->at("reason").get<std::string>().find("Install on phone"), std::string::npos);
}
TEST_F(CompositorFixture, UsbTetherIsWiredWithoutPacingOrStandingFec) {
    const auto identity_directory = directory / "identities";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            for (const auto *name : {"identity.dpapi", "paired.json", "paired.lock"})
                std::filesystem::remove(path / name, ignored);
            std::filesystem::remove(path, ignored);
        }
    } cleanup{identity_directory};
    Identity phone;
    win::IdentityStore identities(identity_directory);
    identities.trust(phone.public_key(), "Link test");
    const auto check = [&](const char *link, bool wired, unsigned floor, unsigned spread) {
        NetworkConfig config;
        config.addresses = {Endpoint::parse("127.0.0.1", 0)};
        config.control_port = config.media_port = 0;
        config.width = 128;
        config.height = 64;
        config.software_only = true;
        config.advertise = false;
        config.identity_directory = identity_directory;
        NetworkServer network(*state, *gpu, *compositor, config);
        auto stream = TcpStream::connect(Endpoint::parse(
            "127.0.0.1", network.endpoints().at(0).at("control_port").get<uint16_t>()));
        PairingPolicy policy;
        policy.expected_identity = identities.identity().public_key();
        const auto material = authenticate_session(*stream, PeerRole::phone, phone, policy);
        ControlChannel channel(*stream, material);
        nlohmann::json hello{
            {"type", "hello"}, {"protocol", wire::session_protocol}, {"transport", "udp"},
            {"capabilities",
             {{"model", "Link fixture"},
              {"codecs", nlohmann::json::array(
                             {{{"mime", "video/avc"},
                               {"sizes", nlohmann::json::array(
                                             {{{"width", 128}, {"height", 64}, {"fps", 60}}})}}})}}}};
        if (link)
            hello["link"] = link;
        channel.send(hello);
        const auto ready = channel.receive(std::chrono::milliseconds(1000));
        ASSERT_TRUE(ready);
        ASSERT_EQ(ready->at("type"), "ready");
        const auto session = network.request({{"method", "network.status"}}).at("session");
        ASSERT_FALSE(session.is_null());
        EXPECT_EQ(session.at("wired_link"), wired);
        EXPECT_EQ(session.at("send_spread_pct"), spread);
        EXPECT_EQ(session.at("adaptation").at("fec").at("floor_pct"), floor);
        EXPECT_EQ(session.at("adaptation").at("fec").at("requested_ratio_pct"), floor);
    };
    check("usb", true, 0, 0);
    check("network", false, 10, 60);
    check(nullptr, false, 10, 60); // The field is optional.
    state->request({{"method", "settings.set"}, {"key", "fec.adaptive"}, {"value", false}});
    check("usb", true, 10, 0); // A fixed ratio is an explicit choice, even on a cable.
}
void CompositorFixture::check_format_session(bool tcp,int failure) {
    const auto identity_directory=directory/"identities";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            for(const auto *name:{"identity.dpapi","paired.json","paired.lock"}) std::filesystem::remove(path/name,ignored);
            std::filesystem::remove(path,ignored);
        }
    } cleanup{identity_directory};
    Identity phone;
    win::IdentityStore identities(identity_directory);identities.trust(phone.public_key(),"Format test");
    NetworkConfig config;config.addresses={Endpoint::parse("127.0.0.1",0)};
    config.control_port=config.media_port=0;config.width=128;config.height=64;
    config.software_only=true;config.advertise=false;config.identity_directory=identity_directory;
    NetworkServer network(*state,*gpu,*compositor,config);
    const auto endpoints=network.endpoints().at(0);
    auto stream=TcpStream::connect(Endpoint::parse("127.0.0.1",endpoints.at("control_port").get<uint16_t>()));
    PairingPolicy policy;policy.expected_identity=identities.identity().public_key();
    const auto material=authenticate_session(*stream,PeerRole::phone,phone,policy);
    ControlChannel channel(*stream,material);
    const auto sizes=nlohmann::json::array({stream_format_json({128,64,60}),stream_format_json({96,48,60}),
        stream_format_json({96,48,45}),stream_format_json({96,48,30})});
    channel.send({{"type","hello"},{"protocol", wire::session_protocol},{"format_change",1},{"transport",tcp?"tcp":"udp"},
        {"capabilities",{{"model","Format fixture"},{"codecs",nlohmann::json::array({
            {{"mime","video/avc"},{"sizes",sizes}}})}}}});
    const auto ready=channel.receive(std::chrono::milliseconds(1000));
    ASSERT_TRUE(ready);ASSERT_EQ(ready->at("type"),"ready");ASSERT_EQ(ready->at("format_change"),1);
    const auto peer=Endpoint::parse("127.0.0.1",ready->at("stream").at(tcp?"tcp_port":"udp_port").get<uint16_t>());
    std::unique_ptr<ITransport> transport;
    if(tcp) transport=std::make_unique<TcpTransport>(TcpStream::connect(peer));
    std::atomic<bool> receiving{true},capture{};
    std::mutex capture_mutex;wire::Bytes access_unit;
    auto media=std::make_unique<PhoneLink>(peer,material,[&](FrameView frame) {
        if(!receiving.load()) return false;
        if(capture.load()&&(frame.header.flags&1)) {
            std::lock_guard lock(capture_mutex);
            if(access_unit.empty()) access_unit.assign(frame.bytes.begin(),frame.bytes.end());
        }
        return true;
    },std::move(transport));
    const auto left=texture({240,240,240,255}), right=texture({16,16,16,255});
    const auto preferences=state->request({{"method","settings.get"}}).at("values");
    const auto start=monotonic_ns();uint64_t next_stats{},sequence{},previous_encoded{};
    uint32_t pose_sequence{};
    unsigned prepared{},committed{},restored{};bool complete=false;
    std::optional<StreamFormat> awaiting;
    uint64_t request_id{};
    while(monotonic_ns()-start<22'000'000'000 && !complete) {
        layer(left.rSharedTextureHandles[0],right.rSharedTextureHandles[0]);present();
        wire::PosePacket pose;pose.sequence=++pose_sequence;pose.sensor_ns=monotonic_ns();
        (void)media->pose(pose);
        if(auto message=channel.receive(std::chrono::milliseconds(5))) {
            const auto type=message->value("type",std::string{});
            if(type=="ping") channel.send({{"type","pong"}});
            if(type=="stream.prepare") {
                ++prepared;request_id=message->at("request_id").get<uint64_t>();
                awaiting=parse_stream_format(message->at("format"));
                EXPECT_EQ(awaiting->width,96);EXPECT_EQ(awaiting->height,48);
                receiving.store(false);capture.store(false);
                media->video_boundary(message->at("render_boundary_ns").get<uint64_t>(),awaiting->fps);
                // An acknowledgment for another transaction must not commit this one.
                channel.send({{"type","stream.committed"},{"request_id",request_id+1}});
                if(failure==1) channel.send({{"type","stream.rejected"},{"request_id",request_id}});
                else if(!failure) channel.send({{"type","stream.prepared"},{"request_id",request_id}});
            }
            if(type=="stream.commit") {
                EXPECT_EQ(message->at("request_id"),request_id);ASSERT_TRUE(awaiting);
                ++committed;receiving.store(true);
                if(committed==2) {std::lock_guard lock(capture_mutex);access_unit.clear();capture.store(true);}
                channel.send({{"type","stream.committed"},{"request_id",request_id}});
            }
            if(type=="stream.abort") {
                ++restored;EXPECT_EQ(parse_stream_format(message->at("format")),(StreamFormat{128,64,60}));
                media->video_boundary(message->at("render_boundary_ns").get<uint64_t>(),60);
                {std::lock_guard lock(capture_mutex);access_unit.clear();}
                receiving.store(true);capture.store(true);
                channel.send({{"type","stream.restored"},{"request_id",request_id}});
            }
        }
        const auto now=monotonic_ns();
        if(now>=next_stats&&media->stats().clock_ready) {
            auto report=empty_phone_stats();report["clock_ready"]=true;report["decode_samples"]=20;
            report["decode_p50_ms"]=5;report["decode_p95_ms"]=20;
            report["accepted_frames"]=++sequence*50;report["decoder_decoded"]=sequence*50;
            report["missed_vsync"]=sequence*3;
            channel.send({{"type","phone.stats"},{"seq",sequence},{"values",report}});
            next_stats=now+200'000'000;
        }
        const auto session=network.request({{"method","network.status"}}).at("session");
        if(!session.is_null()&&session.contains("adaptation")) {
            const auto encoded=session.at("encoded_frames").get<uint64_t>();
            EXPECT_GE(encoded,previous_encoded);previous_encoded=encoded;
            const auto &format=session.at("adaptation").at("format");
            if(format.at("changing").get<bool>()&&media->stats().clock_ready) {
                const auto tracking=state->head.read();
                EXPECT_GT(tracking.timestamp_ns,start);
                EXPECT_LT(monotonic_ns()-tracking.timestamp_ns,250'000'000);
            }
            const bool applied=failure ? restored&&format.at("blocked").get<bool>()&&!format.at("changing").get<bool>() :
                format.at("changes")==2&&format.at("current").at("fps")==45;
            std::lock_guard lock(capture_mutex);complete=applied&&!access_unit.empty();
        }
    }
    ASSERT_TRUE(complete) << network.request({{"method","network.status"}}).dump();
    EXPECT_GE(prepared,1U);EXPECT_EQ(restored,failure?1U:0U);EXPECT_EQ(committed,failure?0U:2U);
    EXPECT_GT(state->head.read().timestamp_ns,start);
    EXPECT_EQ(state->request({{"method","settings.get"}}).at("values"),preferences);
    const auto final_status=network.request({{"method","network.status"}});
    EXPECT_TRUE(final_status.at("control_active").get<bool>());
    EXPECT_EQ(final_status.at("session").at("encoder_resets"),0);
    const auto width=failure?128U:96U,height=failure?64U:48U;
    MfDecoder decoder(Codec::h264,width,height);
    wire::Bytes encoded;{std::lock_guard lock(capture_mutex);encoded=access_unit;}
    ASSERT_TRUE(contains_parameter_sets(encoded,Codec::h264));ASSERT_TRUE(contains_idr(encoded,Codec::h264));
    ASSERT_TRUE(decoder.submit(encoded,1));
    std::optional<DecodedFrame> decoded;const auto deadline=monotonic_ns()+2'000'000'000;
    while(!decoded&&monotonic_ns()<deadline) decoded=decoder.poll();
    ASSERT_TRUE(decoded);EXPECT_EQ(decoded->width,width);EXPECT_EQ(decoded->height,height);
    EXPECT_GT(decoded->nv12[width*10+10],180);EXPECT_LT(decoded->nv12[width*10+width-10],60);
    media.reset();channel.send({{"type","disconnect"}});
}
void CompositorFixture::check_session(bool tcp) {
    const auto identity_directory = directory / "identities";
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code ignored;
            for (const auto *name : {"identity.dpapi", "paired.json", "paired.lock"})
                std::filesystem::remove(directory / name, ignored);
            std::filesystem::remove(directory, ignored);
        }
    } cleanup{identity_directory};
    Identity phone;
    win::IdentityStore identities(identity_directory);
    identities.trust(phone.public_key(), "Test phone");
    NetworkConfig config;
    config.addresses = {Endpoint::parse("127.0.0.1", 0)};
    config.control_port = config.media_port = 0;
    config.width = 128;
    config.height = 64;
    config.software_only = true;
    config.identity_directory = identity_directory;
    NetworkServer network(*state, *gpu, *compositor, config);
    const auto endpoints = network.endpoints();
    ASSERT_EQ(endpoints.size(), 1);
    const auto endpoint = endpoints[0];
    {
        UdpTransport discovery(Endpoint::parse("127.0.0.1", 0));
        DiscoveryNonce nonce{};
        nonce[0] = 43;
        const auto query = discovery_query(nonce);
        ASSERT_TRUE(discovery.send(
            query, Endpoint::parse("127.0.0.1", endpoint.at("media_port").get<uint16_t>())));
        std::array<uint8_t, wire::max_datagram> reply{};
        Endpoint sender;
        const auto count = discovery.receive(reply, sender, std::chrono::milliseconds(1000));
        ASSERT_TRUE(count);
        const auto found = parse_discovery_reply(std::span(reply).first(*count), nonce);
        ASSERT_TRUE(found);
        EXPECT_EQ(found->identity, identities.identity().public_key());
        EXPECT_EQ(found->control_port, endpoint.at("control_port"));
    }
    auto stream = TcpStream::connect(
        Endpoint::parse("127.0.0.1", endpoint.at("control_port").get<uint16_t>()));
    PairingPolicy policy;
    policy.expected_identity = identities.identity().public_key();
    const auto material = authenticate_session(*stream, PeerRole::phone, phone, policy);
    ControlChannel channel(*stream, material);
    channel.send(
        {{"type", "hello"},
         {"transport", tcp ? "tcp" : "udp"},
         {"thermal_guard", 1},
         {"protocol", wire::session_protocol},
         {"capabilities",
          {{"model", "Synthetic PC loopback"}, {"thermal_status", 0},
           {"codecs", nlohmann::json::array(
                          {{{"mime", "video/avc"},
                            {"sizes", nlohmann::json::array(
                                          {{{"width", 128}, {"height", 64}, {"fps", 60}}})}}})}}}});
    const auto ready = channel.receive(std::chrono::milliseconds(1000));
    ASSERT_TRUE(ready);
    ASSERT_EQ(ready->at("type"), "ready");
    const auto remote = Endpoint::parse(
        "127.0.0.1", ready->at("stream").at(tcp ? "tcp_port" : "udp_port").get<uint16_t>());
    std::unique_ptr<ITransport> media_storage;
    if (tcp) {
        // An unbound socket cannot claim the authenticated session with an
        // oversized record; it must be drained before the real phone can attach.
        auto rogue = TcpStream::connect(remote);
        ASSERT_TRUE(rogue->send_record(wire::Bytes(wire::max_datagram + 1, 9),
                                       std::chrono::milliseconds(100)));
        media_storage = std::make_unique<TcpTransport>(TcpStream::connect(remote));
    } else
        media_storage = std::make_unique<UdpTransport>(Endpoint::parse("127.0.0.1", 0));
    auto &media = *media_storage;
    PacketCipher cipher(material.id, material.media);
    std::array<uint8_t, 8> initial_ping{};
    wire::Writer initial_writer(initial_ping);
    initial_writer.u64(monotonic_ns());
    const auto initial_packet = cipher.seal(wire::Type::ping, 0, monotonic_ns(), initial_ping);
    ASSERT_TRUE(initial_packet && media.send(*initial_packet, remote));
    ASSERT_EQ(ready->at("viewer_calibration"), 1);
    ASSERT_EQ(ready->at("phone_stats"), 1);
    ASSERT_EQ(ready->at("phone_input"), 1);
    RawInput controls; controls.active=true;
    const auto send_controls=[&](uint32_t sequence,uint32_t buttons,uint64_t timestamp,bool corrupt=false,uint16_t device=0) {
        controls.sequence=sequence; controls.buttons=buttons; controls.timestamp_ns=timestamp;
        std::array<uint8_t,input_packet_size> payload{};
        if (!encode_input(controls,payload)) return false;
        auto packet=cipher.seal(wire::Type::input,device,monotonic_ns(),payload);
        if (!packet) return false;
        if (corrupt) packet->back()^=1;
        return media.send(*packet,remote);
    };
    const auto input_deadline=monotonic_ns()+1'000'000'000;
    uint32_t input_sequence{};
    while (!state->phone_input.read(monotonic_ns()).active&&monotonic_ns()<input_deadline) {
        ASSERT_TRUE(send_controls(++input_sequence,0,monotonic_ns()));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(state->phone_input.read(monotonic_ns()).active);
    ASSERT_TRUE(send_controls(++input_sequence,1U<<13,monotonic_ns()));
    while (state->phone_input.read(monotonic_ns()).hands[1].trigger!=1&&monotonic_ns()<input_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(state->phone_input.read(monotonic_ns()).hands[1].trigger,1);
    ASSERT_TRUE(send_controls(++input_sequence,0,monotonic_ns(),true));
    ASSERT_TRUE(send_controls(++input_sequence,0,monotonic_ns(),false,1));
    ASSERT_TRUE(send_controls(++input_sequence,0,monotonic_ns()-200'000'000));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(state->phone_input.read(monotonic_ns()).hands[1].trigger,1);
    ASSERT_TRUE(send_controls(++input_sequence,0,monotonic_ns()));
    while (state->phone_input.read(monotonic_ns()).hands[1].trigger!=0&&monotonic_ns()<input_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_EQ(state->phone_input.read(monotonic_ns()).hands[1].trigger,0);
    EXPECT_FALSE(network.request({{"method", "network.status"}})
                     .at("session")
                     .at("phone_stats")
                     .at("available")
                     .get<bool>());
    auto phone_stats = empty_phone_stats();
    phone_stats["decode_samples"] = 20;
    phone_stats["decode_p50_ms"] = 3.5;
    phone_stats["decode_p95_ms"] = 7.0;
    phone_stats["clock_ready"] = true;
    channel.send({{"type", "phone.stats"}, {"seq", 1}, {"values", phone_stats}});
    const auto wait_stats = [&](uint64_t seq, uint64_t rejected) {
        const auto until = monotonic_ns() + 2'000'000'000;
        while (monotonic_ns() < until) {
            const auto stats =
                network.request({{"method", "network.status"}}).at("session").at("phone_stats");
            if (stats.at("seq") == seq && stats.at("rejected") >= rejected)
                return stats;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return nlohmann::json(nullptr);
    };
    // Malformed reports are ignored; the checks below need this session to survive them.
    channel.send({{"type", "latency.samples"}});
    channel.send({{"type", "latency.samples"}, {"samples", 7}});
    channel.send({{"type", "settings.set"}});
    ASSERT_FALSE(wait_stats(1, 0).is_null());
    channel.send({{"type", "phone.stats"}, {"seq", 1}, {"values", phone_stats}});
    auto invalid_stats = phone_stats;
    invalid_stats["decode_p95_ms"] = -1;
    channel.send({{"type", "phone.stats"}, {"seq", 2}, {"values", invalid_stats}});
    const auto preserved_stats = wait_stats(1, 2);
    ASSERT_FALSE(preserved_stats.is_null());
    EXPECT_EQ(preserved_stats.at("values"), phone_stats);
    channel.send({{"type", "phone.stats"}, {"seq", 2}, {"values", phone_stats}});
    const auto latest_stats = wait_stats(2, 2);
    ASSERT_FALSE(latest_stats.is_null());
    EXPECT_TRUE(latest_stats.at("available").get<bool>());
    phone_stats["accepted_frames"] = 55;
    phone_stats["decoder_decoded"] = 55;
    phone_stats["reassembly_dropped_frames"] = 5;
    channel.send({{"type", "phone.stats"}, {"seq", 3}, {"values", phone_stats}});
    const auto wait_bitrate = [&](uint32_t bitrate, bool enabled) {
        const auto until = monotonic_ns() + 2'000'000'000;
        while (monotonic_ns() < until) {
            const auto q =
                network.request({{"method", "network.status"}}).at("session").at("adaptation");
            if (q.at("encoder_accepted_bitrate_bps") == bitrate && q.at("enabled") == enabled)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };
    ASSERT_TRUE(wait_bitrate(48'000'000, true));
    EXPECT_EQ(network.request({{"method", "network.status"}}).at("session").at("adaptation")
        .at("fec").at("requested_ratio_pct"), tcp ? 0 : 17); // TCP must bypass even under pressure.
    state->request({{"method", "settings.set"}, {"key", "video.adaptive"}, {"value", false}});
    ASSERT_TRUE(wait_bitrate(60'000'000, false));
    state->request({{"method", "settings.set"}, {"key", "fec.adaptive"}, {"value", false}});
    state->request({{"method", "settings.set"}, {"key", "fec.ratio_pct"}, {"value", 20}});
    const auto fec_deadline = monotonic_ns() + 2'000'000'000;
    bool fec_updated = false;
    while (!fec_updated && monotonic_ns() < fec_deadline) {
        const auto fec = network.request({{"method", "network.status"}}).at("session").at("adaptation").at("fec");
        fec_updated = !fec.at("adaptive").get<bool>() && fec.at("requested_ratio_pct") == (tcp ? 0 : 20);
        if (!fec_updated) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(fec_updated);
    phone_stats["missed_vsync"] = 100;
    channel.send({{"type", "phone.stats"}, {"seq", 4}, {"values", phone_stats}});
    ASSERT_FALSE(wait_stats(4, 2).is_null());
    EXPECT_TRUE(wait_bitrate(60'000'000, false));
    const auto original_projection = ready->at("projection").at("id").get<uint32_t>();
    const auto viewer_reply = [&](uint64_t request_id) -> std::optional<nlohmann::json> {
        const auto expires = monotonic_ns() + 2'000'000'000;
        while (monotonic_ns() < expires) {
            auto message = channel.receive(std::chrono::milliseconds(50));
            if (!message)
                continue;
            if (message->value("type", "") == "ping")
                channel.send({{"type", "pong"}});
            if (message->value("type", "") == "viewer.result" &&
                message->value("request_id", uint64_t{}) == request_id)
                return message;
        }
        return {};
    };
    auto viewer = state->settings_copy().viewer_values();
    viewer["screen_to_lens_m"] = 0.045;
    viewer["inter_lens_distance_m"] = 0.065;
    channel.send({{"type", "viewer.set"}, {"request_id", 1}, {"values", viewer}});
    const auto applied = viewer_reply(1);
    ASSERT_TRUE(applied);
    ASSERT_EQ(applied->at("type"), "viewer.result");
    ASSERT_TRUE(applied->at("ok").get<bool>());
    const auto projection_id = applied->at("projection").at("id").get<uint32_t>();
    EXPECT_GT(projection_id, original_projection);
    EXPECT_EQ(applied->at("request_id"), 1);
    const auto accepted_settings = state->settings_copy().values();
    viewer["screen_width_m"] = 0.001;
    channel.send({{"type", "viewer.set"}, {"request_id", 2}, {"values", viewer}});
    const auto rejected = viewer_reply(2);
    ASSERT_TRUE(rejected);
    EXPECT_FALSE(rejected->at("ok").get<bool>());
    EXPECT_EQ(rejected->at("request_id"), 2);
    EXPECT_EQ(state->settings_copy().values(), accepted_settings);
    EXPECT_EQ(state->viewer_geometry()->id, projection_id);
    std::array<uint8_t, 8> ping{};
    wire::Writer ping_writer(ping);
    ping_writer.u64(monotonic_ns());
    const auto ping_packet = cipher.seal(wire::Type::ping, 0, monotonic_ns(), ping);
    ASSERT_TRUE(ping_packet);
    ASSERT_TRUE(media.send(*ping_packet, remote));
    const auto deadline = monotonic_ns() + 5'000'000'000;
    bool pong = false;
    std::array<uint8_t, wire::max_datagram> bytes{};
    Endpoint source;
    wire::Header header;
    while (!pong && monotonic_ns() < deadline) {
        const auto size = media.receive(bytes, source, std::chrono::milliseconds(100));
        if (!size)
            continue;
        const auto payload = cipher.open(std::span(bytes).first(*size), header);
        pong = payload && header.type == wire::Type::pong && payload->size() == 24;
    }
    ASSERT_TRUE(pong);
    wire::PosePacket pose;
    pose.sequence = 1;
    pose.sensor_ns = monotonic_ns();
    pose.pose.orientation = Quat::exponential({0, 0.3, 0});
    std::array<uint8_t, wire::pose_size> pose_bytes{};
    ASSERT_TRUE(wire::encode_pose(pose, pose_bytes));
    const auto pose_packet = cipher.seal(wire::Type::pose, 0, monotonic_ns(), pose_bytes);
    ASSERT_TRUE(pose_packet);
    auto tampered = *pose_packet;
    tampered.back() ^= 1;
    ASSERT_TRUE(media.send(tampered, remote));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(state->head.read().timestamp_ns, 0);
    ASSERT_TRUE(media.send(*pose_packet, remote));
    while (state->head.read().timestamp_ns != pose.sensor_ns && monotonic_ns() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_EQ(state->head.read().timestamp_ns, pose.sensor_ns);
    while (!network.request({{"method", "network.status"}})
                .at("session")
                .value("encoder_ready", false) &&
           monotonic_ns() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto left = texture({255, 0, 0, 255}), right = texture({0, 0, 255, 255});
    layer(left.rSharedTextureHandles[0], right.rSharedTextureHandles[0]);
    present();
    Reassembler assembler(1 << 20);
    MfDecoder decoder(Codec::h264, 128, 64);
    bool fed = false;
    while (!fed && monotonic_ns() < deadline) {
        const auto size = media.receive(bytes, source, std::chrono::milliseconds(100));
        if (!size)
            continue;
        const auto payload = cipher.open(std::span(bytes).first(*size), header);
        if (!payload || (header.type != wire::Type::video && header.type != wire::Type::fec))
            continue;
        const auto video = wire::decode_video_header(*payload);
        ASSERT_TRUE(video);
        const auto frame = assembler.push(
            *video, std::span(*payload).subspan(wire::video_header_size), monotonic_ns());
        if (frame) {
            EXPECT_EQ(frame->header.frame_id, 1);
            EXPECT_EQ(frame->header.projection_id, projection_id);
            EXPECT_NEAR(frame->header.render_pose.orientation.w, 1, 1e-8);
            fed = decoder.submit(frame->bytes, frame->header.frame_id);
        }
    }
    ASSERT_TRUE(fed);
    std::optional<DecodedFrame> decoded;
    while (!decoded && monotonic_ns() < deadline) {
        decoded = decoder.poll();
        if (!decoded)
            std::this_thread::yield();
    }
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->frame_id, 1);
    EXPECT_NEAR(decoded->nv12[32 * 128 + 32], 63, 5);
    EXPECT_NEAR(decoded->nv12[32 * 128 + 96], 32, 5);
    EXPECT_EQ(network.request({{"method", "network.status"}}).at("session").at("encoded_frames"),
              1);
    channel.send({{"type", "phone.thermal"}, {"seq", 1}, {"status", 4}, {"sample_age_ms", 0}});
    const auto wait_cap = [&](unsigned cap) {
        const auto until = monotonic_ns() + 2'000'000'000;
        while (monotonic_ns() < until) {
            const auto guard = network.request({{"method", "network.status"}}).at("session").at("adaptation").at("guard");
            if (guard.at("input_fps_cap") == cap) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };
    ASSERT_TRUE(wait_cap(0));
    for (unsigned i = 0; i < 6; ++i) {
        layer(left.rSharedTextureHandles[0], right.rSharedTextureHandles[0]); present();
    }
    const auto paused = network.request({{"method", "network.status"}}).at("session");
    EXPECT_EQ(paused.at("encoded_frames"), 1);
    EXPECT_GE(paused.at("adaptation").at("guard").at("skipped_frames"), 6);
    // Video protection cannot suppress tracking ingress or destroy its session.
    pose.sequence = 2; pose.sensor_ns = monotonic_ns();
    ASSERT_TRUE(wire::encode_pose(pose, pose_bytes));
    const auto tracked = cipher.seal(wire::Type::pose, 0, monotonic_ns(), pose_bytes);
    ASSERT_TRUE(tracked && media.send(*tracked, remote));
    const auto tracking_until = monotonic_ns() + 1'000'000'000;
    while (state->head.read().timestamp_ns != pose.sensor_ns && monotonic_ns() < tracking_until)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_EQ(state->head.read().timestamp_ns, pose.sensor_ns);
    state->request({{"method", "settings.set"}, {"key", "power.thermal_guard"}, {"value", false}});
    ASSERT_TRUE(wait_cap(60));
    layer(left.rSharedTextureHandles[0], right.rSharedTextureHandles[0]); present();
    bool resumed_idr = false;
    const auto resumed_until = monotonic_ns() + 2'000'000'000;
    while (!resumed_idr && monotonic_ns() < resumed_until) {
        const auto size = media.receive(bytes, source, std::chrono::milliseconds(50));
        if (!size) continue;
        const auto payload = cipher.open(std::span(bytes).first(*size), header);
        if (!payload || (header.type != wire::Type::video && header.type != wire::Type::fec)) continue;
        const auto video = wire::decode_video_header(*payload);
        resumed_idr = video && video->frame_id > 1 && (video->flags & 1) != 0;
    }
    EXPECT_TRUE(resumed_idr); // Resume must restart a decodable reference chain.
    if (tcp)
        static_cast<TcpTransport &>(media).interrupt();
    else
        channel.send({{"type", "disconnect"}});
    const auto disconnected_by = monotonic_ns() + 2'000'000'000;
    while (!network.request({{"method", "network.status"}}).at("session").is_null() &&
           monotonic_ns() < disconnected_by)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_TRUE(network.request({{"method", "network.status"}}).at("session").is_null());
    EXPECT_FALSE(state->phone_input.read(monotonic_ns()).active);
}
TEST_F(CompositorFixture, PremultipliedLayerOrderAndLinearLightBlending) {
    auto red = texture({255, 0, 0, 255}), half_green = texture({0, 188, 0, 128});
    layer(red.rSharedTextureHandles[0], red.rSharedTextureHandles[0]);
    layer(half_green.rSharedTextureHandles[0], half_green.rSharedTextureHandles[0]);
    present();
    const auto image = pixels();
    const auto center = (32 * 128 + 32) * 4;
    EXPECT_NEAR(image[center + 2], 188, 2);
    EXPECT_NEAR(image[center + 1], 188, 2);
    EXPECT_EQ(image[center], 0);
    EXPECT_GE(image[center + 3], 254);
}
TEST_F(CompositorFixture, TextureBoundsSelectRightHalfAndHistoryMatchesFrame) {
    auto set = texture({255, 0, 0, 255});
    Com<ID3D11Texture2D> source;
    ASSERT_HRESULT_SUCCEEDED(gpu->device->OpenSharedResource(
        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(set.rSharedTextureHandles[0])),
        IID_PPV_ARGS(&source)));
    std::array<uint8_t, 16 * 16 * 4> data{};
    for (size_t y = 0; y < 16; ++y)
        for (size_t x = 0; x < 16; ++x) {
            const auto at = (y * 16 + x) * 4;
            data[at] = x < 8 ? 255 : 0;
            data[at + 1] = x < 8 ? 0 : 255;
            data[at + 3] = 255;
        }
    gpu->context->UpdateSubresource(source.Get(), 0, nullptr, data.data(), 16 * 4, 0);
    gpu->context->Flush();
    layer(set.rSharedTextureHandles[0], set.rSharedTextureHandles[0], {0.5F, 0, 1, 1});
    present();
    const auto image = pixels();
    const auto center = (32 * 128 + 32) * 4;
    EXPECT_EQ(image[center + 2], 0);
    EXPECT_GE(image[center + 1], 254);
    const auto history =
        state->request({{"method", "frame.pose"}, {"frame_id", metadata.frame_id}});
    EXPECT_EQ(history.at("render_ns"), metadata.render_ns);
    EXPECT_EQ(history.at("orientation").at(3), metadata.render_pose.orientation.w);
}
TEST_F(CompositorFixture, SwapSetLifetimeAndBoundedOutputLeases) {
    auto set = texture({70, 80, 90, 255}, 17);
    EXPECT_EQ(compositor->texture_sets(), 1);
    vr::SharedTextureHandle_t handles[2]{set.rSharedTextureHandles[0],
                                         set.rSharedTextureHandles[0]};
    uint32_t indices[2]{};
    compositor->GetNextSwapTextureSetIndex(handles, &indices);
    EXPECT_EQ(indices[0], 1);
    EXPECT_EQ(indices[1], 1);
    std::vector<uint8_t> leases;
    compositor->sink([&](const GpuFrame &frame) {
        leases.push_back(frame.slot);
        return true;
    });
    for (int i = 0; i < 4; ++i) {
        layer(set.rSharedTextureHandles[0], set.rSharedTextureHandles[0]);
        present();
    }
    EXPECT_EQ(leases.size(), 3);
    vr::DriverDirectMode_FrameTiming timing{};
    timing.m_nSize = sizeof(timing);
    compositor->GetFrameTiming(&timing);
    EXPECT_EQ(timing.m_nNumDroppedFrames, 1);
    for (auto slot : leases)
        compositor->release(slot);
    layer(set.rSharedTextureHandles[0], set.rSharedTextureHandles[0]);
    present();
    EXPECT_EQ(leases.size(), 4);
    compositor->release(leases.back());
    compositor->DestroyAllSwapTextureSets(17);
    EXPECT_EQ(compositor->texture_sets(), 0);
}
TEST_F(CompositorFixture, MalformedDimensionsAndPoseAreContained) {
    vr::IVRDriverDirectModeComponent::SwapTextureSet_t set{};
    vr::IVRDriverDirectModeComponent::SwapTextureSetDesc_t description{
        9000, 16, DXGI_FORMAT_R8G8B8A8_UNORM, 1};
    compositor->CreateSwapTextureSet(1, &description, &set);
    EXPECT_EQ(set.rSharedTextureHandles[0], 0);
    auto valid = texture({1, 2, 3, 255});
    vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t eyes[2]{};
    for (auto &eye : eyes) {
        eye.hTexture = valid.rSharedTextureHandles[0];
        eye.bounds = {0, 0, 1, 1};
    }
    compositor->SubmitLayer(eyes);
    present();
    EXPECT_EQ(captured, nullptr);
}
} // namespace
