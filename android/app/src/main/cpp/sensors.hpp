// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pocketvr/phone_link.hpp>
#include <nlohmann/json.hpp>

namespace pocketvr::android {
class Sensors final {
  public:
    Sensors(std::shared_ptr<PhoneLink> link, std::shared_ptr<GyroBiasCache> calibration);
    ~Sensors();
    void configure(const nlohmann::json &settings);
    void rotation(unsigned value);
    void recenter() {
        recenter_.store(true);
    }
    void calibrate() {
        calibration_->clear();
        calibrate_.store(true);
    }
    bool calibrating() const {
        return calibrating_.load();
    }
    PoseSnapshot pose() const {
        return latest_.read();
    }
    nlohmann::json status() const;

  private:
    std::shared_ptr<PhoneLink> link_;
    std::shared_ptr<GyroBiasCache> calibration_;
    PoseState latest_;
    std::atomic<unsigned> rotation_{}, rate_{400}, mode_{}, yaw_{};
    std::atomic<bool> recenter_{}, calibrate_{}, running_{}, calibrating_{true}, reused_{};
    std::atomic<uint64_t> samples_{}, rejected_{}, queue_drops_{};
    std::atomic<double> measured_rate_{};
    mutable std::mutex error_mutex_;
    std::string error_;
    Worker worker_;
    void run(StopToken stop) noexcept;
};
} // namespace pocketvr::android
