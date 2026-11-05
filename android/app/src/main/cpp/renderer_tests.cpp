// SPDX-License-Identifier: Apache-2.0
#include "renderer.hpp"
#include "decoder.hpp"
#include <android/native_window_jni.h>
#include <pocketvr/transport.hpp>

namespace {
using namespace pocketvr;
struct Harness {
    std::shared_ptr<android::Renderer> renderer;
    std::unique_ptr<android::Decoder> decoder;
    uint64_t id{};
    ~Harness() {
        decoder.reset();
        renderer.reset();
    }
};
template <class F, class T> T checked(JNIEnv *env, F action, T fallback) {
    try {
        return action();
    } catch (const std::exception &e) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), e.what());
    } catch (...) {
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), "Renderer test failed");
    }
    return fallback;
}
std::string text(JNIEnv *env, jstring value) {
    const char *raw = env->GetStringUTFChars(value, nullptr);
    if (!raw)
        throw std::runtime_error("Missing test argument");
    const std::string result(raw);
    env->ReleaseStringUTFChars(value, raw);
    return result;
}
Harness &harness(jlong value) {
    if (!value)
        throw std::invalid_argument("Missing renderer test handle");
    return *reinterpret_cast<Harness *>(static_cast<uintptr_t>(value));
}
} // namespace
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeTests_rendererCalibration(
    JNIEnv *env, jclass, jlong handle, jboolean enabled) {
    (void)checked(
        env,
        [&] {
            harness(handle).renderer->test_calibration(enabled != 0);
            return true;
        },
        false);
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeTests_rendererCooling(
    JNIEnv *env, jclass, jlong handle, jboolean enabled) {
    (void)checked(env, [&] { harness(handle).renderer->cooling(enabled != 0); return true; }, false);
}
extern "C" JNIEXPORT jlong JNICALL Java_com_pocketvr_NativeTests_rendererOpen(JNIEnv *env, jclass,
                                                                              jobject surface,
                                                                              jobject texture,
                                                                              jstring codec) {
    return checked(
        env,
        [&]() -> jlong {
            auto result = std::make_unique<Harness>();
            auto window = std::shared_ptr<ANativeWindow>(ANativeWindow_fromSurface(env, surface),
                                                         ANativeWindow_release);
            result->renderer = std::make_shared<android::Renderer>(env, texture, std::move(window));
            Settings settings;
            auto viewer = settings.viewer_values();
            viewer["screen_width_m"] = 0.128;
            viewer["screen_height_m"] = 0.064;
            viewer["inter_lens_distance_m"] = 0.064;
            viewer["screen_to_lens_m"] = 0.032;
            viewer["k1"] = viewer["k2"] = 0;
            settings.set_viewer(viewer);
            settings.set("display.vignette_strength", 0);
            settings.set("reprojection.enabled", true);
            result->renderer->configure(settings, 1);
            const auto session = result->renderer->begin_stream();
            android::DecoderConfig config;
            config.name = text(env, codec);
            config.mime = "video/avc";
            config.width = 256;
            config.height = 128;
            config.fps = 60;
            config.present = [renderer = result->renderer, session](const auto &header,
                                                                    uint64_t time, uint64_t, uint64_t) {
                return renderer->frame(header, time, session);
            };
            result->decoder = std::make_unique<android::Decoder>(result->renderer->decoder_window(),
                                                                 std::move(config));
            return static_cast<jlong>(reinterpret_cast<uintptr_t>(result.release()));
        },
        jlong{});
}
extern "C" JNIEXPORT jboolean JNICALL Java_com_pocketvr_NativeTests_rendererFeed(JNIEnv *env,
                                                                                 jclass,
                                                                                 jlong handle,
                                                                                 jbyteArray bytes) {
    return checked(
        env,
        [&]() -> jboolean {
            auto &test = harness(handle);
            const auto length = env->GetArrayLength(bytes);
            if (length <= 0 || length > 8 * 1024 * 1024)
                throw std::invalid_argument("Invalid test access unit");
            std::vector<uint8_t> data(static_cast<size_t>(length));
            env->GetByteArrayRegion(bytes, 0, length, reinterpret_cast<jbyte *>(data.data()));
            wire::VideoHeader header;
            header.frame_id = ++test.id;
            header.render_ns = monotonic_ns();
            header.projection_id = 1;
            header.frame_bytes = static_cast<uint32_t>(length);
            header.flags = 1;
            return test.decoder->enqueue(FrameView{header, data}) ? JNI_TRUE : JNI_FALSE;
        },
        static_cast<jboolean>(JNI_FALSE));
}
extern "C" JNIEXPORT jstring JNICALL Java_com_pocketvr_NativeTests_rendererStatus(JNIEnv *env,
                                                                                  jclass,
                                                                                  jlong handle) {
    return checked(
        env,
        [&] {
            auto &test = harness(handle);
            const nlohmann::json status{{"renderer", test.renderer->status()},
                                        {"decoder", test.decoder->status()}};
            return env->NewStringUTF(status.dump().c_str());
        },
        static_cast<jstring>(nullptr));
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeTests_rendererClose(JNIEnv *env, jclass,
                                                                              jlong handle) {
    (void)checked(
        env,
        [&] {
            delete &harness(handle);
            return true;
        },
        false);
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeTests_rendererPose(JNIEnv *env, jclass,
                                                                             jlong handle,
                                                                             jdouble yaw) {
    (void)checked(
        env,
        [&] {
            if (!std::isfinite(yaw) || std::abs(yaw) > pi)
                throw std::invalid_argument("Invalid synthetic yaw");
            harness(handle).renderer->test_pose(
                {{{}, Quat::exponential({0, yaw, 0}), {}}, monotonic_ns(), 1});
            return true;
        },
        false);
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeTests_rendererGrid(JNIEnv *env, jclass,
                                                                             jlong handle,
                                                                             jboolean enabled) {
    (void)checked(
        env,
        [&] {
            harness(handle).renderer->grid(enabled != 0);
            return true;
        },
        false);
}
extern "C" JNIEXPORT void JNICALL Java_com_pocketvr_NativeTests_rendererStats(JNIEnv *env, jclass,
                                                                              jlong handle,
                                                                              jint mode) {
    (void)checked(
        env,
        [&] {
            harness(handle).renderer->stats(mode);
            return true;
        },
        false);
}
