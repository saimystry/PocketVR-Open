// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pocketvr/latency_log.hpp>
#include <utility>
#include <vector>
#include "sensors.hpp"
#include <pocketvr/settings.hpp>
#include <pocketvr/surface_frames.hpp>
#include <android/native_window.h>
#include <array>
#include <mutex>
#include <android/surface_texture.h>
#include <jni.h>

namespace pocketvr::android {
class Renderer final {
  public:
    // Single-buffer mode draws straight to the screen. It skips the compositor queue
    // but can tear, and falls back to normal drawing where the phone lacks support.
    Renderer(JNIEnv *env, jobject texture, std::shared_ptr<ANativeWindow> display,
             bool single_buffer = false);
    ~Renderer();
    std::shared_ptr<ANativeWindow> decoder_window() const {
        return decoder_window_;
    }
    uint64_t begin_stream() {
        std::lock_guard lock(arrival_mutex_);
        arrivals_seen_ = 0;
        return session_.fetch_add(1) + 1;
    }
    bool frame(wire::VideoHeader header, uint64_t timestamp_ns, uint64_t session,
               uint64_t received_ns = 0, uint64_t queued_ns = 0) {
        arrived();
        return frames_.publish({header, timestamp_ns, session, received_ns, queued_ns});
    }
    // Timings of frames shown since the last call, in this phone's clock.
    std::vector<PhoneFrameTiming> take_latency() {
        std::lock_guard lock(latency_mutex_);
        return std::exchange(latency_, {});
    }
    void configure(const Settings &settings, uint32_t projection_id);
    void display_hz(double hz) {
        if (!std::isfinite(hz) || hz < 20 || hz > 240) throw std::invalid_argument("Display refresh rate");
        display_hz_.store(hz);
    }
    void tracking(std::weak_ptr<Sensors> sensors);
    void cooling(bool paused) { cooling_.store(paused); }
    void menu(std::vector<std::string> lines) {
        std::lock_guard lock(menu_mutex_);
        menu_ = std::move(lines);
        menu_revision_.fetch_add(1);
    }
    void grid(bool enabled) {
        grid_.store(enabled);
    }
    // 0 headset lenses, 1 fit the view to each half screen, 2 one undistorted view.
    void view_mode(int mode) {
        if (mode < 0 || mode > 2)
            throw std::invalid_argument("Invalid view mode");
        view_mode_.store(mode);
    }
    // Size of the picture in each eye; 1 is the plain lens-corrected size.
    void view_size(double size) {
        if (!std::isfinite(size) || size < 0.7 || size > 1.6)
            throw std::invalid_argument("Invalid eye view size");
        view_size_.store(size);
    }
    void stats(int mode) {
        if (mode < -1 || mode > 2)
            throw std::invalid_argument("Invalid stats overlay mode");
        stats_override_.store(mode);
    }
    nlohmann::json status() const;
#ifdef POCKETVR_NATIVE_TESTS
    void test_pose(PoseSnapshot pose) {
        test_pose_.publish(pose);
    }
    void test_calibration(bool value) {
        test_calibrating_.store(value);
    }
#endif

  private:
    struct Config {
        ViewerGeometry geometry{};
        Lens lens{};
        float strength{0.3F}, size{0.85F}, softness{0.25F};
        bool reprojection{true}, chromatic{}, dynamic{};
        int edge{}, stats{};
        uint64_t revision{};
    };
    JavaVM *vm_{};
    jobject texture_ref_{};
    ASurfaceTexture *texture_{};
    std::shared_ptr<ANativeWindow> display_, decoder_window_;
    SurfaceFrames frames_;
    mutable std::mutex config_mutex_, error_mutex_;
    Config config_;
    std::weak_ptr<Sensors> sensors_;
    std::string error_;
    std::atomic<bool> stop_{}, running_{}, grid_{}, single_buffer_active_{};
    bool single_buffer_{};
    std::atomic<bool> cooling_{};
    std::atomic<uint64_t> session_{}, draws_{}, latched_{}, repeated_{}, missed_{}, frame_id_{};
    std::atomic<double> render_ms_{}, view_size_{1};
    std::atomic<double> display_fps_{}, video_fps_{};
    std::atomic<double> display_hz_{60};
    std::atomic<int> stats_override_{-1}, stats_mode_{}, view_mode_{};
    Worker worker_;
#ifdef POCKETVR_NATIVE_TESTS
    PoseState test_pose_;
    std::atomic<bool> test_calibrating_{};
#endif
    void run(StopToken stop) noexcept;
    // Records how long before the next display refresh a decoded frame was ready.
    void arrived();
    mutable std::mutex arrival_mutex_;
    std::mutex latency_mutex_, menu_mutex_;
    std::vector<std::string> menu_;
    std::atomic<uint64_t> menu_revision_{};
    std::vector<PhoneFrameTiming> latency_;
    std::array<float, 64> arrivals_{};
    uint64_t arrivals_seen_{};
    std::atomic<uint64_t> vsync_ns_{};
};
} // namespace pocketvr::android
