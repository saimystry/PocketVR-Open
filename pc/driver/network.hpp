// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "compositor.hpp"
#include <memory>

namespace pocketvr::driver {
struct NetworkConfig {
    std::vector<Endpoint> addresses;
    uint16_t control_port{9943}, media_port{9944};
    uint32_t width{2160}, height{1080};
    bool software_only{};
    bool advertise{true};
    // A phone reaching the PC over adb has already been approved by its owner.
    bool trust_loopback{true};
    std::filesystem::path identity_directory;
};
class NetworkServer final {
  public:
    NetworkServer(State &state, Gpu &gpu, Compositor &compositor, NetworkConfig config = {});
    ~NetworkServer();
    NetworkServer(const NetworkServer &) = delete;
    NetworkServer &operator=(const NetworkServer &) = delete;
    nlohmann::json request(const nlohmann::json &request);
    nlohmann::json endpoints() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace pocketvr::driver
