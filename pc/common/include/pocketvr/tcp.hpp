// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "transport.hpp"
#include <atomic>

namespace pocketvr {
// One reader and one writer, with interrupt() callable by the owner. Join them
// before destroying the stream. A partial send timeout closes the connection.
class TcpStream final {
  public:
    static std::unique_ptr<TcpStream>
    connect(Endpoint peer, std::chrono::milliseconds timeout = std::chrono::milliseconds(1000),
            bool allow_public = false);
    ~TcpStream();
    TcpStream(const TcpStream &) = delete;
    TcpStream &operator=(const TcpStream &) = delete;
    bool send_record(std::span<const uint8_t> payload, std::chrono::milliseconds timeout);
    // For media, where losing one record is fine. A record that cannot start within
    // `start` is skipped and the stream stays open. One already partly written must
    // finish within `finish`, or the framing is lost and the stream closes.
    bool send_droppable_record(std::span<const uint8_t> payload, std::chrono::milliseconds start,
                               std::chrono::milliseconds finish);
    void send_buffer(int bytes) noexcept;
    std::optional<wire::Bytes> receive_record(std::chrono::milliseconds timeout);
    // View remains valid until the next receive call. Used by the USB hot path.
    std::optional<std::span<const uint8_t>> receive_record_view(std::chrono::milliseconds timeout);
    void interrupt() noexcept;
    bool alive() const noexcept {
        return alive_.load();
    }
    Endpoint peer() const noexcept {
        return peer_;
    }
    Endpoint local() const;

  private:
    friend class TcpListener;
    TcpStream(uintptr_t socket, Endpoint peer);
    bool write_record(std::span<const uint8_t> payload, std::chrono::milliseconds start,
                      std::chrono::milliseconds finish, bool droppable);
    uintptr_t socket_;
    Endpoint peer_;
    std::atomic<bool> alive_{true};
    std::array<uint8_t, 4> prefix_{};
    std::array<uint8_t, wire::max_control + 4> transmit_{};
    size_t prefix_used_{}, payload_used_{};
    wire::Bytes payload_;
};
class TcpListener final {
  public:
    explicit TcpListener(Endpoint bind, bool allow_public = false);
    ~TcpListener();
    TcpListener(const TcpListener &) = delete;
    TcpListener &operator=(const TcpListener &) = delete;
    std::unique_ptr<TcpStream> accept(std::chrono::milliseconds timeout);
    Endpoint local() const;

  private:
    uintptr_t socket_;
    bool public_{};
};
// USB adb media uses the same checked UDP packet bytes inside TCP records.
class TcpTransport final : public ITransport {
  public:
    explicit TcpTransport(std::unique_ptr<TcpStream> stream);
    bool send(std::span<const uint8_t> message, const Endpoint &peer) override;
    std::optional<size_t> receive(std::span<uint8_t> buffer, Endpoint &sender,
                                  std::chrono::milliseconds timeout) override;
    Endpoint local_endpoint() const override {
        return stream_->local();
    }
    void interrupt() noexcept {
        stream_->interrupt();
    }
    bool alive() const noexcept override {
        return stream_->alive();
    }

  private:
    std::unique_ptr<TcpStream> stream_;
};
} // namespace pocketvr
