// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <windows.h>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <functional>
#include <span>
#include <string>
#include <vector>
#include <thread>
#include <pocketvr/transport.hpp>

namespace pocketvr::win {
class Handle {
  public:
    explicit Handle(HANDLE h = nullptr) : handle_(h) {}
    ~Handle() {
        reset();
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&other) noexcept : handle_(other.release()) {}
    Handle &operator=(Handle &&other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.release();
        }
        return *this;
    }
    HANDLE get() const {
        return handle_;
    }
    explicit operator bool() const {
        return handle_ && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() {
        const auto h = handle_;
        handle_ = nullptr;
        return h;
    }
    void reset(HANDLE h = nullptr) {
        if (*this)
            CloseHandle(handle_);
        handle_ = h;
    }

  private:
    HANDLE handle_;
};
std::filesystem::path appdata(bool local = false);
void initialize_logging(const std::string &component);
std::vector<uint8_t> protect(std::span<const uint8_t> bytes);
std::vector<uint8_t> unprotect(std::span<const uint8_t> bytes);
void atomic_write(const std::filesystem::path &file, std::span<const uint8_t> bytes);
std::vector<Endpoint> private_addresses();
struct PrivateInterface {
    Endpoint address;
    uint32_t index{};
};
std::vector<PrivateInterface> private_interfaces();
std::string computer_name();
constexpr wchar_t pipe_name[] = LR"(\\.\pipe\PocketVR.v1)";
class RpcServer {
  public:
    using Handler = std::function<nlohmann::json(const nlohmann::json &)>;
    explicit RpcServer(Handler handler, std::wstring name = pipe_name);
    ~RpcServer();
    RpcServer(const RpcServer &) = delete;
    RpcServer &operator=(const RpcServer &) = delete;

  private:
    Handler handler_;
    std::wstring name_;
    Handle stop_;
    std::jthread thread_;
    void run(std::stop_token token);
};
nlohmann::json rpc(const nlohmann::json &request, const std::wstring &name = pipe_name,
                   std::stop_token cancellation = {});
} // namespace pocketvr::win
