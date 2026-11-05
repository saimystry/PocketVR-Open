// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/discovery.hpp>
#include <algorithm>
#include <stdexcept>

namespace pocketvr {
namespace {
bool name_valid(const std::string &name) {
    if (name.empty() || name.size() > 96)
        return false;
    for (size_t i = 0; i < name.size();) {
        const auto lead = static_cast<uint8_t>(name[i++]);
        if (lead < 0x80) {
            if (lead < 32 || lead == 127)
                return false;
            continue;
        }
        const unsigned count = lead >= 0xc2 && lead <= 0xdf   ? 1
                               : lead >= 0xe0 && lead <= 0xef ? 2
                               : lead >= 0xf0 && lead <= 0xf4 ? 3
                                                              : 0;
        if (!count || name.size() - i < count)
            return false;
        uint32_t code = lead & ((1U << (6 - count)) - 1);
        for (unsigned n = 0; n < count; ++n) {
            const auto byte = static_cast<uint8_t>(name[i++]);
            if ((byte & 0xc0) != 0x80)
                return false;
            code = (code << 6) | (byte & 0x3f);
        }
        constexpr uint32_t minimum[]{0, 0x80, 0x800, 0x10000};
        if (code < minimum[count] || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            return false;
    }
    return true;
}
} // namespace
std::array<uint8_t, discovery_query_size> discovery_query(const DiscoveryNonce &nonce) {
    std::array<uint8_t, discovery_query_size> bytes{};
    wire::Writer writer(bytes);
    writer.u32(discovery_magic);
    writer.u8(1);
    writer.u8(0);
    writer.u16(0);
    writer.bytes(nonce);
    return bytes;
}
std::optional<DiscoveryNonce> parse_discovery_query(std::span<const uint8_t> bytes) {
    if (bytes.size() != discovery_query_size)
        return {};
    wire::Reader reader(bytes);
    uint32_t magic{};
    uint8_t version{}, kind{};
    uint16_t reserved{};
    DiscoveryNonce nonce{};
    if (!reader.u32(magic) || !reader.u8(version) || !reader.u8(kind) || !reader.u16(reserved) ||
        !reader.bytes(nonce) || magic != discovery_magic || version != 1 || kind || reserved)
        return {};
    return nonce;
}
wire::Bytes discovery_reply(const DiscoveryNonce &nonce, const DiscoveredServer &server) {
    if (!server.control_port || !server.media_port || !name_valid(server.name))
        throw std::invalid_argument("Invalid discovery reply");
    wire::Bytes bytes(60 + server.name.size());
    wire::Writer writer(bytes);
    writer.u32(discovery_magic);
    writer.u8(1);
    writer.u8(1);
    writer.u16(server.control_port);
    writer.bytes(nonce);
    writer.u16(server.media_port);
    writer.u16(static_cast<uint16_t>(server.name.size()));
    writer.bytes(
        std::span(reinterpret_cast<const uint8_t *>(server.name.data()), server.name.size()));
    writer.bytes(server.identity);
    return bytes;
}
std::optional<DiscoveredServer> parse_discovery_reply(std::span<const uint8_t> bytes,
                                                      const DiscoveryNonce &expected) {
    if (bytes.size() < 61 || bytes.size() > 156)
        return {};
    wire::Reader reader(bytes);
    uint32_t magic{};
    uint8_t version{}, kind{};
    uint16_t length{};
    DiscoveryNonce nonce{};
    DiscoveredServer server;
    if (!reader.u32(magic) || !reader.u8(version) || !reader.u8(kind) ||
        !reader.u16(server.control_port) || !reader.bytes(nonce) ||
        !reader.u16(server.media_port) || !reader.u16(length) || magic != discovery_magic ||
        version != 1 || kind != 1 || !server.control_port || !server.media_port ||
        nonce != expected || !length || length > 96 ||
        reader.remaining() != static_cast<size_t>(length) + 32)
        return {};
    server.name.resize(length);
    if (!reader.bytes(std::span(reinterpret_cast<uint8_t *>(server.name.data()), length)) ||
        !reader.bytes(server.identity) || !name_valid(server.name))
        return {};
    return server;
}
} // namespace pocketvr
