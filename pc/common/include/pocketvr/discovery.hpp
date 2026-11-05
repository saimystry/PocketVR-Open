// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "crypto.hpp"

namespace pocketvr {
using DiscoveryNonce = std::array<uint8_t, 16>;
constexpr uint32_t discovery_magic = 0x31445650; // PVD1
constexpr size_t discovery_query_size = 24;
struct DiscoveredServer {
    uint16_t control_port{}, media_port{};
    std::string name;
    Key identity{}; // Untrusted hint; only pairing/identity signatures establish trust.
};
std::array<uint8_t, discovery_query_size> discovery_query(const DiscoveryNonce &nonce);
std::optional<DiscoveryNonce> parse_discovery_query(std::span<const uint8_t> bytes);
wire::Bytes discovery_reply(const DiscoveryNonce &nonce, const DiscoveredServer &server);
std::optional<DiscoveredServer> parse_discovery_reply(std::span<const uint8_t> bytes,
                                                      const DiscoveryNonce &expected);
} // namespace pocketvr
