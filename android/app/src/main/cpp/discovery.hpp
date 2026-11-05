// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pocketvr/discovery.hpp>
#include <pocketvr/transport.hpp>
#include <pocketvr/worker.hpp>
#include <nlohmann/json.hpp>
#include <mutex>

namespace pocketvr::android {
class DiscoveryScanner final {
  public:
    ~DiscoveryScanner() {
        stop();
    }
    void start();
    void stop();
    nlohmann::json results() const;

  private:
    struct Candidate {
        Endpoint endpoint;
        DiscoveredServer server;
        uint64_t seen{};
    };
    mutable std::mutex mutex_;
    std::vector<Candidate> candidates_;
    Worker worker_;
    void run(StopToken stop) noexcept;
};
} // namespace pocketvr::android
