// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "protocol.hpp"
#include <array>
#include <bitset>
#include <limits>

namespace pocketvr {
using Key = std::array<uint8_t, 32>;
struct KeyPair {
    Key public_key{}, secret_key{};
    KeyPair();
    ~KeyPair();
    KeyPair(const KeyPair &) = delete;
    KeyPair &operator=(const KeyPair &) = delete;
};
struct SessionKeys {
    Key transmit{}, receive{};
};
SessionKeys exchange_keys(const KeyPair &local, const Key &peer, bool client,
                          std::span<const uint8_t> transcript);
class ReplayWindow {
  public:
    bool acceptable(uint64_t counter) const;
    bool accept(uint64_t counter);

  private:
    uint64_t highest_{};
    std::bitset<1024> seen_;
};
class PacketCipher {
  public:
    PacketCipher(uint32_t session, SessionKeys keys);
    ~PacketCipher();
    PacketCipher(const PacketCipher &) = delete;
    PacketCipher &operator=(const PacketCipher &) = delete;
    std::optional<wire::Bytes> seal(wire::Type type, uint16_t device, uint64_t send_ns,
                                    std::span<const uint8_t> payload);
    // Allocating convenience for tools/tests. Streaming workers use open_into.
    std::optional<wire::Bytes> open(std::span<const uint8_t> packet, wire::Header &header);
    std::optional<size_t> seal_into(wire::Type type, uint16_t device, uint64_t send_ns,
                                    std::span<const uint8_t> payload, std::span<uint8_t> output);
    std::optional<size_t> open_into(std::span<const uint8_t> packet, wire::Header &header,
                                    std::span<uint8_t> plaintext);

  private:
    uint32_t session_;
    SessionKeys keys_;
    uint64_t counter_{};
    ReplayWindow replay_;
};
} // namespace pocketvr
