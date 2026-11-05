// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "windows.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace pocketvr::dashboard {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
struct Snapshot {
    Json data = Json::object();
    Clock::time_point received{};
    uint64_t revision{};
    std::string error, result, logs;
    bool online(Clock::time_point now = Clock::now()) const {
        return error.empty() && revision && now >= received && now - received < std::chrono::seconds(3);
    }
};
class Client {
  public:
    explicit Client(std::wstring pipe = win::pipe_name);
    ~Client();
    Snapshot snapshot() const;
    bool command(Json request);
    bool read_logs();
    bool export_bundle();
  private:
    enum class Kind { rpc, logs, bundle };
    struct Job { Kind kind; Json request; Clock::time_point created; };
    std::wstring pipe_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    Snapshot snapshot_;
    std::deque<Job> jobs_;
    std::jthread worker_;
    bool enqueue(Kind kind, Json request = {});
    void run(std::stop_token stop);
};
struct History {
    std::vector<float> bitrate, decode, fps, drops;
    uint64_t revision{}, bytes{}, frames{}, dropped{};
    Clock::time_point previous{};
    void update(const Snapshot &snapshot);
};
} // namespace pocketvr::dashboard
