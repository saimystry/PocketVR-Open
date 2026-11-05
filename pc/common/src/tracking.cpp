// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/tracking.hpp>

namespace pocketvr {
void PoseState::publish(PoseSnapshot s) {
    if (!s.pose.position.finite() || !s.pose.orientation.finite() ||
        std::abs(s.pose.orientation.norm2() - 1) > 0.01 || !s.pose.angular_velocity.finite() ||
        s.pose.angular_velocity.norm() > 100 || s.tracking > 2 || !s.linear_velocity.finite() ||
        s.linear_velocity.norm() > 100 || s.space > 1)
        throw std::invalid_argument("Invalid tracking pose");
    s.pose.orientation = s.pose.orientation.unit();
    std::lock_guard lock(writer_);
    sequence_.fetch_add(1);
    const std::array<double, 13> values{s.pose.position.x,         s.pose.position.y,
                                        s.pose.position.z,         s.pose.orientation.x,
                                        s.pose.orientation.y,      s.pose.orientation.z,
                                        s.pose.orientation.w,      s.pose.angular_velocity.x,
                                        s.pose.angular_velocity.y, s.pose.angular_velocity.z,
                                        s.linear_velocity.x, s.linear_velocity.y, s.linear_velocity.z};
    for (size_t i = 0; i < values.size(); ++i)
        values_[i].store(values[i]);
    timestamp_.store(s.timestamp_ns);
    tracking_.store(s.tracking);
    space_.store(s.space);
    sequence_.fetch_add(1);
}
PoseSnapshot PoseState::read() const noexcept {
    // A reader may alternate among HMD/local/test states. Preserve a bounded
    // cache per instance rather than wiping the fallback on every switch.
    // Generation tags also prevent address reuse from resurrecting old poses.
    struct Cached { const PoseState *owner{}; uint64_t identity{}; PoseSnapshot pose{}; };
    thread_local std::array<Cached, 8> recent{};
    thread_local size_t victim{};
    auto found = std::find_if(recent.begin(), recent.end(), [&](const auto &entry) {
        return entry.owner == this && entry.identity == identity_;
    });
    if (found == recent.end()) {
        found = recent.begin() + static_cast<ptrdiff_t>(victim);
        victim = (victim + 1) % recent.size();
        *found = {this, identity_, {}};
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto before = sequence_.load();
        if (before & 1)
            continue;
        std::array<double, 13> v{};
        for (size_t i = 0; i < v.size(); ++i)
            v[i] = values_[i].load();
        PoseSnapshot s{{{v[0], v[1], v[2]}, {v[3], v[4], v[5], v[6]}, {v[7], v[8], v[9]}},
                       timestamp_.load(),
                       tracking_.load(), {v[10], v[11], v[12]}, space_.load()};
        if (sequence_.load() == before) {
            if (!s.timestamp_ns)
                s = {};
            found->pose = s;
            return s;
        }
    }
    return found->pose;
}

Vec3 sensor_to_display(Vec3 value, unsigned rotation) {
    if (!value.finite() || rotation > 3)
        throw std::invalid_argument("Invalid sensor coordinate mapping");
    switch (rotation) {
    case 0:
        return value;
    case 1:
        return {-value.y, value.x, value.z};
    case 2:
        return {-value.x, -value.y, value.z};
    default:
        return {value.y, -value.x, value.z};
    }
}
Quat game_rotation_to_display(Quat android_rotation, unsigned rotation) {
    if (rotation > 3)
        throw std::invalid_argument("Invalid display rotation");
    // Sensor vectors use display_from_sensor; an orientation instead maps the
    // display basis into the world and therefore needs its inverse on the right.
    return (Quat::exponential({-pi / 2, 0, 0}) * android_rotation.unit() *
            Quat::exponential({0, 0, -static_cast<double>(rotation) * pi / 2})).unit();
}
void SensorFusion::begin_bias_calibration() {
    calibrating_ = true;
    calibration_seconds_ = calibration_square_ = 0;
    calibration_sum_ = calibration_accel_ = {};
}
void SensorFusion::restore_bias(Vec3 bias) {
    if (!bias.finite() || bias.norm() >= 0.08)
        throw std::invalid_argument("Invalid calibrated gyro bias");
    bias_ = bias;
    calibrating_ = false;
}
bool SensorFusion::update(const SensorSample &s) {
    if (!s.gyro.finite() || !s.accel.finite() || s.gyro.norm() > 100 || s.accel.norm() > 1000 ||
        s.timestamp_ns == 0)
        return false;
    if (timestamp_ == 0) {
        timestamp_ = s.timestamp_ns;
        if (s.accel.norm() > 1) {
            const auto up = s.accel.unit();
            const auto axis = up.cross({0, 1, 0});
            orientation_ = up.y < -0.999999 ? Quat::exponential({pi, 0, 0})
                                            : Quat{axis.x, axis.y, axis.z, 1 + up.y}.unit();
        }
        return true;
    }
    if (s.timestamp_ns <= timestamp_)
        return false;
    const auto dt = static_cast<double>(s.timestamp_ns - timestamp_) * 1e-9;
    timestamp_ = s.timestamp_ns;
    if (dt > 0.1) {
        if (calibrating_)
            begin_bias_calibration();
        return false;
    }
    const auto accel_norm = s.accel.norm();
    if (calibrating_) {
        // The accelerometer's sample noise must not repeatedly cancel a quiet
        // gyro window. Filter only the stationarity test, not fusion samples or
        // gyro statistics. Sustained translation/tilt and rotation still reject.
        if (calibration_filtered_accel_.norm() == 0) calibration_filtered_accel_ = s.accel;
        const auto alpha = 1 - std::exp(-dt / 0.15);
        calibration_filtered_accel_ = calibration_filtered_accel_ +
            (s.accel - calibration_filtered_accel_) * alpha;
        if (accel_norm < 7 || accel_norm > 12 ||
            std::abs(calibration_filtered_accel_.norm() - 9.80665) >= 0.3 || s.gyro.norm() >= 0.08 ||
            (calibration_seconds_ > 0 && (calibration_filtered_accel_ - calibration_accel_).norm() > 0.2)) {
            begin_bias_calibration();
        } else {
            if (calibration_seconds_ == 0)
                calibration_accel_ = calibration_filtered_accel_;
            calibration_seconds_ += dt;
            calibration_sum_ = calibration_sum_ + s.gyro * dt;
            calibration_square_ += s.gyro.dot(s.gyro) * dt;
            if (calibration_seconds_ >= 2) {
                const auto mean = calibration_sum_ * (1 / calibration_seconds_);
                const auto variance = calibration_square_ / calibration_seconds_ - mean.dot(mean);
                if (variance > 0.000009)
                    begin_bias_calibration();
                else {
                    bias_ = mean;
                    calibrating_ = false;
                }
            }
        }
    }
    Vec3 correction{};
    const auto expected_up = orientation_.inverse().rotate({0, 1, 0});
    if (accel_norm > 1) {
        const auto weight = std::clamp(1 - std::abs(accel_norm - 9.80665) / 2.0, 0.0, 1.0);
        correction = (s.accel * (1 / accel_norm)).cross(expected_up) * (2.0 * weight);
    }
    disturbed_ = false;
    if (use_mag_ && s.magnetometer) {
        const auto field = s.magnetometer->norm();
        disturbed_ = !s.magnetometer->finite() || field < 15 || field > 90 ||
                     (field_norm_ > 0 && std::abs(field - field_norm_) / field_norm_ > 0.25);
        if (!disturbed_) {
            if (field_norm_ == 0)
                field_norm_ = field;
            else
                field_norm_ += (field - field_norm_) * (1 - std::exp(-dt / 30));
            const auto measured = *s.magnetometer - expected_up * s.magnetometer->dot(expected_up);
            const auto north = orientation_.inverse().rotate({0, 0, -1});
            const auto projected = north - expected_up * north.dot(expected_up);
            if (measured.norm() > 1 && projected.norm() > 0.1)
                correction =
                    correction +
                    expected_up * (measured.unit().cross(projected.unit()).dot(expected_up) * 0.2);
        }
    }
    if (!calibrating_)
        refine_bias(s, dt);
    omega_ = s.gyro - bias_;
    orientation_ = (orientation_ * Quat::exponential((omega_ + correction) * dt)).unit();
    return true;
}
void SensorFusion::refine_bias(const SensorSample &s, double dt) {
    const auto residual = s.gyro - bias_;
    if (residual.norm() > 0.05 || std::abs(s.accel.norm() - 9.80665) > 0.3) {
        quiet_seconds_ = quiet_square_ = 0;
        quiet_sum_ = {};
        return;
    }
    quiet_seconds_ += dt;
    quiet_sum_ = quiet_sum_ + s.gyro * dt;
    quiet_square_ += s.gyro.dot(s.gyro) * dt;
    if (quiet_seconds_ < 2)
        return;
    const auto mean = quiet_sum_ * (1 / quiet_seconds_);
    const auto variance = quiet_square_ / quiet_seconds_ - mean.dot(mean);
    const auto next = bias_ + (mean - bias_) * 0.1;
    // Gyro alone cannot tell a very slow deliberate turn from drift. Judging the whole
    // window's mean ignores head tremor, and this limit keeps slow turns out.
    if ((mean - bias_).norm() < 0.004 && variance <= 0.0001 && next.norm() < 0.08)
        bias_ = next;
    quiet_seconds_ = quiet_square_ = 0;
    quiet_sum_ = {};
}
void SensorFusion::recenter() {
    const auto forward = orientation_.rotate({0, 0, -1});
    const auto yaw = std::atan2(-forward.x, -forward.z);
    recenter_ = Quat::exponential({0, -yaw, 0});
}
void SensorFusion::slow_autocenter(double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 0.1)
        return;
    const auto forward = orientation().rotate({0, 0, -1});
    const auto yaw = std::atan2(-forward.x, -forward.z);
    recenter_ =
        (Quat::exponential({0, std::clamp(-yaw, -0.002 * seconds, 0.002 * seconds), 0}) * recenter_)
            .unit();
}
void SensorFusion::reset() {
    *this = SensorFusion{};
}
} // namespace pocketvr
