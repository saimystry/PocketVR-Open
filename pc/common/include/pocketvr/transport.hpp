// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "protocol.hpp"
#include <chrono>
#include <memory>
#include <string>

namespace pocketvr {
struct Endpoint {
    uint32_t address{};
    uint16_t port{}; // IPv4 address in host order.
    static Endpoint parse(const std::string &address, uint16_t port);
    std::string ip() const;
    bool private_or_local() const;
    bool operator==(const Endpoint &) const = default;
};
class ITransport {
  public:
    virtual ~ITransport() = default;
    virtual bool send(std::span<const uint8_t> message, const Endpoint &peer) = 0;
    virtual std::optional<size_t> receive(std::span<uint8_t> buffer, Endpoint &sender,
                                          std::chrono::milliseconds timeout) = 0;
    virtual Endpoint local_endpoint() const = 0;
    virtual bool alive() const noexcept {
        return true;
    }
};
class UdpTransport final : public ITransport {
  public:
    explicit UdpTransport(Endpoint local, bool allow_public = false);
    ~UdpTransport() override;
    UdpTransport(const UdpTransport &) = delete;
    UdpTransport &operator=(const UdpTransport &) = delete;
    bool send(std::span<const uint8_t> message, const Endpoint &peer) override;
    std::optional<size_t> receive(std::span<uint8_t> buffer, Endpoint &sender,
                                  std::chrono::milliseconds timeout) override;
    Endpoint local_endpoint() const override;
    void broadcast_enabled(bool enabled);
    int receive_buffer_bytes() const noexcept { return receive_buffer_; }
    int send_buffer_bytes() const noexcept { return send_buffer_; }
    bool buffer_tuning_applied() const noexcept { return buffer_tuning_applied_; }
    // Blocks until one of up to 16 sockets has data or the timeout passes. Lets a
    // receiver sleep without adding delay to the next packet.
    static bool wait_any(std::span<UdpTransport *const> sockets, std::chrono::milliseconds timeout);

  private:
    uintptr_t socket_;
    bool public_;
    int receive_buffer_{}, send_buffer_{};
    bool buffer_tuning_applied_{};
};
uint64_t monotonic_ns();
} // namespace pocketvr
