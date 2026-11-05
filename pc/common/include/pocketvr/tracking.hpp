// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "math.hpp"
#include <atomic>
#include <cstdint>
#include <optional>
#include <mutex>
#include <type_traits>

namespace pocketvr {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // Intentional cache-line padding isolates the SPSC cursors.
#endif
template <class T, size_t Capacity> class SpscQueue {
    static_assert(Capacity >= 2 && std::is_trivially_copyable_v<T>);

  public:
    bool push(const T &value) {
        const auto write = write_.load(std::memory_order_relaxed), next = (write + 1) % Capacity;
        if (next == read_.load(std::memory_order_acquire))
            return false;
        data_[write] = value;
        write_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T &value) {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire))
            return false;
        value = data_[read];
        read_.store((read + 1) % Capacity, std::memory_order_release);
        return true;
    }

  private:
    std::array<T, Capacity> data_{};
    alignas(64) std::atomic<size_t> read_{};
    alignas(64) std::atomic<size_t> write_{};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
struct SensorSample {
    uint64_t timestamp_ns{};
    Vec3 gyro{}, accel{0, 9.80665, 0};
    std::optional<Vec3> magnetometer;
};
// Runtime-only bias in the phone's fixed sensor axes, independent of display
// rotation. Explicit recalibration invalidates it; no value is persisted to disk.
class GyroBiasCache {
    mutable std::mutex mutex_;
    std::optional<Vec3> value_;

  public:
    std::optional<Vec3> get() const {
        std::lock_guard lock(mutex_);
        return value_;
    }
    void clear() {
        std::lock_guard lock(mutex_);
        value_.reset();
    }
    void set(Vec3 value) {
        if (!value.finite() || value.norm() >= 0.08)
            throw std::invalid_argument("Invalid calibrated gyro bias");
        std::lock_guard lock(mutex_);
        value_ = value;
    }
};
class SensorFusion {
  public:
    // Explicit hold-still calibration; freeze bias afterwards so slow deliberate
    // yaw cannot be mistaken for a sensor offset during normal tracking.
    void begin_bias_calibration();
    void restore_bias(Vec3 bias);
    bool calibrating() const {
        return calibrating_;
    }
    bool update(const SensorSample &sample);
    Quat orientation() const {
        return (recenter_ * orientation_).unit();
    }
    Vec3 angular_velocity() const {
        return omega_;
    }
    Vec3 gyro_bias() const {
        return bias_;
    }
    bool magnetic_disturbance() const {
        return disturbed_;
    }
    void magnetometer_enabled(bool enabled) {
        use_mag_ = enabled;
    }
    void recenter();
    void slow_autocenter(double seconds);
    void reset();

  private:
    Quat orientation_{}, recenter_{};
    Vec3 bias_{}, omega_{};
    uint64_t timestamp_{};
    bool use_mag_{}, disturbed_{};
    double field_norm_{};
    bool calibrating_{};
    // Temperature moves the gyro's zero point during a session. Quiet two-second
    // windows nudge the bias; movement of any kind restarts the window.
    double quiet_seconds_{}, quiet_square_{};
    Vec3 quiet_sum_{};
    void refine_bias(const SensorSample &sample, double dt);
    double calibration_seconds_{}, calibration_square_{};
    Vec3 calibration_sum_{}, calibration_accel_{}, calibration_filtered_accel_{};
};
// Android sensor axes remain fixed to the device's natural orientation.
// rotation is Android Surface.ROTATION_0/90/180/270 (0..3).
Vec3 sensor_to_display(Vec3 value, unsigned rotation);
Quat game_rotation_to_display(Quat android_rotation, unsigned rotation);
struct PoseSnapshot {
    Pose pose{};
    uint64_t timestamp_ns{};
    uint8_t tracking{};
    Vec3 linear_velocity{};
    uint8_t space{}; // 0 tracking space, 1 already aligned OpenVR standing space.
};
class PoseState {
  public:
    void publish(PoseSnapshot sample);
    PoseSnapshot read() const noexcept;

  private:
    inline static std::atomic<uint64_t> identities_{};
    const uint64_t identity_{identities_.fetch_add(1) + 1};
    mutable std::mutex writer_;
    std::atomic<uint64_t> sequence_{}, timestamp_{};
    std::array<std::atomic<double>, 13> values_{};
    std::atomic<uint8_t> tracking_{};
    std::atomic<uint8_t> space_{};
};
struct PoseRecord {
    uint64_t frame_id{}, timestamp_ns{};
    Pose pose{};
};
// All calls require external serialization; State owns history_mutex_.
class PoseHistory {
  public:
    void put(PoseRecord value) {
        records_[next_] = value;
        next_ = (next_ + 1) % records_.size();
        count_ = std::min(count_ + 1, records_.size());
    }
    std::optional<PoseRecord> frame(uint64_t id) const {
        for (size_t i = 0; i < count_; ++i) {
            const auto &p = records_[(next_ + records_.size() - 1 - i) % records_.size()];
            if (p.frame_id == id)
                return p;
        }
        return {};
    }

  private:
    std::array<PoseRecord, 512> records_{};
    size_t next_{}, count_{};
};
} // namespace pocketvr
