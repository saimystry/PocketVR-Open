// SPDX-License-Identifier: Apache-2.0
#include "sensors.hpp"
#include <android/sensor.h>
#include <android/looper.h>
#include <sys/resource.h>
#include <unistd.h>
#include <cerrno>

namespace pocketvr::android {
Sensors::Sensors(std::shared_ptr<PhoneLink> link, std::shared_ptr<GyroBiasCache> calibration)
    : link_(std::move(link)), calibration_(std::move(calibration)) {
    if (!calibration_)
        throw std::invalid_argument("Missing gyro calibration cache");
    worker_ = Worker([this](StopToken stop) { run(stop); });
}
Sensors::~Sensors() {
    worker_.request_stop();
    if (worker_.joinable())
        worker_.join();
}
void Sensors::rotation(unsigned value) {
    if (value > 3)
        throw std::invalid_argument("Invalid display rotation");
    rotation_.store(value);
}
void Sensors::configure(const nlohmann::json &settings) {
    const auto &tracking = settings.at("tracking");
    const auto rate = tracking.at("rate_hz").get<unsigned>();
    if (rate < 100 || rate > 500)
        throw std::invalid_argument("Invalid IMU rate");
    const auto mode = tracking.at("fusion").get<std::string>();
    if (mode != "gyro_accel" && mode != "gyro_accel_mag" && mode != "android_game_rotation")
        throw std::invalid_argument("Unknown fusion mode");
    const auto yaw = tracking.at("yaw_correction").get<std::string>();
    if (yaw != "off" && yaw != "magnetometer" && yaw != "slow_autocenter")
        throw std::invalid_argument("Unknown yaw correction");
    rate_.store(rate);
    mode_.store(mode == "android_game_rotation" ? 2U : mode == "gyro_accel_mag" ? 1U : 0U);
    yaw_.store(yaw == "magnetometer" ? 1U : yaw == "slow_autocenter" ? 2U : 0U);
}
nlohmann::json Sensors::status() const {
    std::lock_guard lock(error_mutex_);
    return {{"running", running_.load()},           {"calibrating", calibrating_.load()},
            {"calibration_reused", reused_.load()}, {"samples", samples_.load()},
            {"rejected", rejected_.load()},         {"queue_drops", queue_drops_.load()},
            {"rate_hz", measured_rate_.load()},     {"error", error_}};
}
void Sensors::run(StopToken stop) noexcept {
    ASensorManager *manager{};
    ASensorEventQueue *queue{};
    ALooper *looper{};
    try {
        (void)setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), -8);
        manager = ASensorManager_getInstanceForPackage("com.pocketvr");
        if (!manager)
            throw std::runtime_error("Android sensor manager unavailable");
        looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
        if (!looper)
            throw std::runtime_error("Sensor looper unavailable");
        ALooper_acquire(looper);
        queue = ASensorManager_createEventQueue(manager, looper, 1, nullptr, nullptr);
        if (!queue)
            throw std::runtime_error("Sensor event queue unavailable");
        const auto gyro = ASensorManager_getDefaultSensor(manager, ASENSOR_TYPE_GYROSCOPE);
        const auto accel = ASensorManager_getDefaultSensor(manager, ASENSOR_TYPE_ACCELEROMETER);
        const auto mag = ASensorManager_getDefaultSensor(manager, ASENSOR_TYPE_MAGNETIC_FIELD);
        const auto game =
            ASensorManager_getDefaultSensor(manager, ASENSOR_TYPE_GAME_ROTATION_VECTOR);
        if (!gyro || !accel)
            throw std::runtime_error("A gyroscope and accelerometer are required");
        auto enable = [&](const ASensor *sensor, unsigned hz) {
            const auto delay = std::max<int32_t>(ASensor_getMinDelay(sensor),
                                                 static_cast<int32_t>(1'000'000 / hz));
            if (ASensorEventQueue_registerSensor(queue, sensor, delay, 0) < 0)
                throw std::runtime_error("Cannot enable requested sensor without batching");
        };
        enable(accel, 200);
        unsigned current_rate{}, current_mode = 99, current_yaw = 99, current_rotation = 99;
        SensorFusion fusion;
        Vec3 acceleration{}, magnetic{};
        uint64_t accel_ns{}, mag_ns{}, game_ns{}, last_send{}, last_gyro{}, rate_start{},
            rate_samples{};
        Quat game_orientation{}, game_center{};
        uint32_t sequence{};
        bool first_pose = true;
        double still_seconds{};
        running_.store(true);
        while (!stop.stop_requested()) {
            const auto rotation = rotation_.load(), mode = mode_.load(), yaw = yaw_.load();
            const auto calibrate = calibrate_.exchange(false);
            if (current_rotation != rotation || current_mode != mode || calibrate) {
                fusion.reset();
                const auto cached = calibrate ? std::optional<Vec3>{} : calibration_->get();
                if (cached)
                    fusion.restore_bias(sensor_to_display(*cached, rotation));
                else
                    fusion.begin_bias_calibration();
                reused_.store(cached.has_value());
                current_rotation = rotation;
                current_mode = mode;
                current_yaw = 99;
                accel_ns = mag_ns = game_ns = last_send = 0;
                first_pose = true;
                game_center = {};
                still_seconds = 0;
                calibrating_.store(fusion.calibrating());
                if (game) {
                    ASensorEventQueue_disableSensor(queue, game);
                    if (mode == 2)
                        enable(game, rate_.load());
                }
                if (mode == 2 && !game)
                    throw std::runtime_error("Game rotation sensor is unavailable");
            }
            if (current_rate != rate_.load()) {
                current_rate = rate_.load();
                enable(gyro, current_rate);
                if (mode == 2 && game)
                    enable(game, current_rate);
            }
            if (current_yaw != yaw) {
                current_yaw = yaw;
                if (mag) {
                    ASensorEventQueue_disableSensor(queue, mag);
                    if (mode == 1 || yaw == 1)
                        enable(mag, 50);
                }
            }
            fusion.magnetometer_enabled(mode == 1 || yaw == 1);
            (void)ALooper_pollOnce(5, nullptr, nullptr, nullptr);
            ASensorEvent events[32];
            ssize_t count{};
            while (!stop.stop_requested() &&
                   (count = ASensorEventQueue_getEvents(queue, events, 32)) > 0) {
                for (ssize_t i = 0; i < count; ++i) {
                    const auto &event = events[i];
                    if (event.timestamp <= 0 || (event.type != ASENSOR_TYPE_ACCELEROMETER &&
                                                 event.type != ASENSOR_TYPE_MAGNETIC_FIELD &&
                                                 event.type != ASENSOR_TYPE_GAME_ROTATION_VECTOR &&
                                                 event.type != ASENSOR_TYPE_GYROSCOPE))
                        continue;
                    const auto timestamp = static_cast<uint64_t>(event.timestamp);
                    const auto now = monotonic_ns();
                    if (timestamp > now || now - timestamp > 50'000'000) {
                        rejected_.fetch_add(1);
                        continue;
                    }
                    const Vec3 raw{event.data[0], event.data[1], event.data[2]};
                    if (!raw.finite()) {
                        rejected_.fetch_add(1);
                        continue;
                    }
                    const auto value = sensor_to_display(raw, rotation);
                    if (event.type == ASENSOR_TYPE_ACCELEROMETER) {
                        acceleration = value;
                        accel_ns = timestamp;
                    } else if (event.type == ASENSOR_TYPE_MAGNETIC_FIELD) {
                        magnetic = value;
                        mag_ns = timestamp;
                    } else if (event.type == ASENSOR_TYPE_GAME_ROTATION_VECTOR) {
                        const Quat android_q{event.data[0], event.data[1], event.data[2],
                                             event.data[3]};
                        if (android_q.finite() && std::abs(android_q.norm2() - 1) < 0.02) {
                            game_orientation = game_rotation_to_display(android_q, rotation);
                            game_ns = timestamp;
                        }
                    } else if (event.type == ASENSOR_TYPE_GYROSCOPE) {
                        if (!accel_ns || accel_ns > timestamp || timestamp - accel_ns > 100'000'000)
                            continue;
                        SensorSample sample{timestamp, value, acceleration, {}};
                        if (mag_ns && mag_ns <= timestamp && timestamp - mag_ns < 200'000'000)
                            sample.magnetometer = magnetic;
                        const bool was_calibrating = fusion.calibrating();
                        if (!fusion.update(sample)) {
                            rejected_.fetch_add(1);
                            continue;
                        }
                        samples_.fetch_add(1);
                        calibrating_.store(fusion.calibrating());
                        if (was_calibrating && !fusion.calibrating())
                            calibration_->set(
                                sensor_to_display(fusion.gyro_bias(), (4 - rotation) % 4));
                        if (fusion.calibrating())
                            continue;
                        if (first_pose || recenter_.exchange(false)) {
                            fusion.recenter();
                            const auto forward = game_orientation.rotate({0, 0, -1});
                            game_center =
                                Quat::exponential({0, -std::atan2(-forward.x, -forward.z), 0});
                            first_pose = false;
                        }
                        auto orientation = fusion.orientation();
                        if (mode == 2) {
                            if (!game_ns || game_ns > timestamp ||
                                timestamp - game_ns > 100'000'000)
                                continue;
                            orientation = (game_center * game_orientation).unit();
                        }
                        const auto omega = fusion.angular_velocity();
                        const auto linear =
                            acceleration - orientation.inverse().rotate({0, 9.80665, 0});
                        const auto dt = last_gyro && timestamp > last_gyro
                                            ? static_cast<double>(timestamp - last_gyro) * 1e-9
                                            : 0;
                        last_gyro = timestamp;
                        if (yaw == 2 && omega.norm() < 0.003 && linear.norm() < 0.1)
                            still_seconds += dt;
                        else
                            still_seconds = 0;
                        if (yaw == 2 && still_seconds > 10 && mode != 2) {
                            // Recenter only after prolonged stillness; never learn slow-turn gyro
                            // bias.
                            fusion.slow_autocenter(dt);
                        }
                        wire::PosePacket pose;
                        pose.sequence = ++sequence;
                        pose.sensor_ns = timestamp;
                        pose.pose.orientation = orientation;
                        pose.pose.angular_velocity = omega;
                        pose.linear_accel = linear;
                        latest_.publish({pose.pose, timestamp, 1});
                        if (!last_send || timestamp - last_send >= 950'000'000 / current_rate) {
                            if (!link_->pose(pose))
                                queue_drops_.fetch_add(1);
                            last_send = timestamp;
                        }
                        if (!rate_start)
                            rate_start = timestamp;
                        ++rate_samples;
                        if (timestamp - rate_start >= 1'000'000'000) {
                            measured_rate_.store(static_cast<double>(rate_samples) * 1e9 /
                                                 static_cast<double>(timestamp - rate_start));
                            rate_start = timestamp;
                            rate_samples = 0;
                        }
                    }
                }
            }
            if (count < 0 && count != -EAGAIN)
                throw std::runtime_error("Android sensor queue failed");
        }
    } catch (const std::exception &error) {
        std::lock_guard lock(error_mutex_);
        error_ = error.what();
    } catch (...) {
        std::lock_guard lock(error_mutex_);
        error_ = "Sensor thread failed";
    }
    if (queue)
        ASensorManager_destroyEventQueue(manager, queue);
    if (looper)
        ALooper_release(looper);
    running_.store(false);
}
} // namespace pocketvr::android
