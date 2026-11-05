// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <openvr_driver.h>
#include "state.hpp"
#include "d3d.hpp"

namespace pocketvr::driver {
class Compositor;
class Hmd final : public vr::ITrackedDeviceServerDriver, public vr::IVRDisplayComponent {
  public:
    Hmd(State &state, Gpu &gpu);
    ~Hmd();
    Compositor &compositor() {
        return *compositor_;
    }
    vr::EVRInitError Activate(uint32_t index) override;
    void Deactivate() override;
    void EnterStandby() override;
    void *GetComponent(const char *version) override;
    void DebugRequest(const char *request, char *response, uint32_t capacity) override;
    vr::DriverPose_t GetPose() override;
    void GetWindowBounds(int32_t *x, int32_t *y, uint32_t *width, uint32_t *height) override;
    bool IsDisplayOnDesktop() override {
        return false;
    }
    bool IsDisplayRealDisplay() override {
        return false;
    }
    void GetRecommendedRenderTargetSize(uint32_t *width, uint32_t *height) override;
    void GetEyeOutputViewport(vr::EVREye eye, uint32_t *x, uint32_t *y, uint32_t *width,
                              uint32_t *height) override;
    void GetProjectionRaw(vr::EVREye eye, float *left, float *right, float *top,
                          float *bottom) override;
    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye eye, float u, float v) override;
    bool ComputeInverseDistortion(vr::HmdVector2_t *result, vr::EVREye eye, uint32_t channel,
                                  float u, float v) override;
    void update_properties();
    uint32_t index() const {
        return index_.load();
    }

  private:
    State &state_;
    Gpu &gpu_;
    std::unique_ptr<Compositor> compositor_;
    std::atomic<uint32_t> index_{vr::k_unTrackedDeviceIndexInvalid};
};
} // namespace pocketvr::driver
