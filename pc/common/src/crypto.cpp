// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/crypto.hpp>
#include <sodium.h>
#include <stdexcept>

namespace pocketvr {
static void initialize() {
    static const int result = sodium_init();
    if (result < 0)
        throw std::runtime_error("libsodium initialization failed");
}
KeyPair::KeyPair() {
    initialize();
    if (crypto_kx_keypair(public_key.data(), secret_key.data()) != 0)
        throw std::runtime_error("X25519 key generation failed");
}
KeyPair::~KeyPair() {
    sodium_memzero(secret_key.data(), secret_key.size());
}
SessionKeys exchange_keys(const KeyPair &local, const Key &peer, bool client,
                          std::span<const uint8_t> transcript) {
    initialize();
    if (transcript.size() < 32 || transcript.size() > 4096)
        throw std::invalid_argument("Session transcript size");
    SessionKeys base{}, result{};
    const int status =
        client ? crypto_kx_client_session_keys(base.receive.data(), base.transmit.data(),
                                               local.public_key.data(), local.secret_key.data(),
                                               peer.data())
               : crypto_kx_server_session_keys(base.receive.data(), base.transmit.data(),
                                               local.public_key.data(), local.secret_key.data(),
                                               peer.data());
    if (status != 0)
        throw std::invalid_argument("Invalid peer X25519 key");
    crypto_generichash(result.transmit.data(), result.transmit.size(), transcript.data(),
                       transcript.size(), base.transmit.data(), base.transmit.size());
    crypto_generichash(result.receive.data(), result.receive.size(), transcript.data(),
                       transcript.size(), base.receive.data(), base.receive.size());
    sodium_memzero(&base, sizeof(base));
    return result;
}
bool ReplayWindow::acceptable(uint64_t counter) const {
    if (counter == 0)
        return false;
    if (counter > highest_)
        return true;
    const auto offset = highest_ - counter;
    return offset < seen_.size() && !seen_.test(static_cast<size_t>(offset));
}
bool ReplayWindow::accept(uint64_t counter) {
    if (!acceptable(counter))
        return false;
    if (counter > highest_) {
        const auto delta = counter - highest_;
        if (delta >= seen_.size())
            seen_.reset();
        else
            seen_ <<= static_cast<size_t>(delta);
        highest_ = counter;
    }
    seen_.set(static_cast<size_t>(highest_ - counter));
    return true;
}
PacketCipher::PacketCipher(uint32_t session, SessionKeys keys) : session_(session), keys_(keys) {
    initialize();
    if (session == 0)
        throw std::invalid_argument("Zero session identifier");
    sodium_memzero(&keys, sizeof(keys));
}
PacketCipher::~PacketCipher() {
    sodium_memzero(&keys_, sizeof(keys_));
}
static std::array<uint8_t, 12> nonce(uint32_t session, uint64_t counter) {
    std::array<uint8_t, 12> n{};
    wire::Writer w(n);
    w.u32(session);
    w.u64(counter);
    return n;
}
std::optional<wire::Bytes> PacketCipher::seal(wire::Type type, uint16_t device, uint64_t send_ns,
                                              std::span<const uint8_t> payload) {
    if (payload.size() > wire::max_payload)
        return {};
    wire::Bytes out(wire::header_size + payload.size() + wire::tag_size);
    if (!seal_into(type, device, send_ns, payload, out))
        return {};
    return out;
}
std::optional<size_t> PacketCipher::seal_into(wire::Type type, uint16_t device, uint64_t send_ns,
                                              std::span<const uint8_t> payload,
                                              std::span<uint8_t> out) {
    if (payload.size() > wire::max_payload ||
        out.size() < wire::header_size + payload.size() + wire::tag_size ||
        counter_ == std::numeric_limits<uint64_t>::max())
        return {};
    wire::Header header{type, 1, session_, device, ++counter_, send_ns};
    if (!wire::encode_header(header, out))
        return {};
    const auto n = nonce(session_, counter_);
    unsigned long long size{};
    if (crypto_aead_chacha20poly1305_ietf_encrypt(
            out.data() + wire::header_size, &size, payload.data(), payload.size(), out.data(),
            wire::header_size, nullptr, n.data(), keys_.transmit.data()) != 0)
        return {};
    return static_cast<size_t>(size) + wire::header_size;
}
std::optional<wire::Bytes> PacketCipher::open(std::span<const uint8_t> packet,
                                              wire::Header &header) {
    if (packet.size() < wire::header_size + wire::tag_size)
        return {};
    if (packet.size() > wire::max_datagram)
        return {};
    wire::Bytes plain(packet.size() - wire::header_size - wire::tag_size);
    if (!open_into(packet, header, plain))
        return {};
    return plain;
}
std::optional<size_t> PacketCipher::open_into(std::span<const uint8_t> packet, wire::Header &header,
                                              std::span<uint8_t> plain) {
    if (packet.size() < wire::header_size + wire::tag_size || packet.size() > wire::max_datagram ||
        plain.size() < packet.size() - wire::header_size - wire::tag_size)
        return {};
    const auto decoded = wire::decode_header(packet);
    if (!decoded || decoded->session != session_ || !replay_.acceptable(decoded->counter))
        return {};
    unsigned long long size{};
    const auto n = nonce(session_, decoded->counter);
    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            plain.data(), &size, nullptr, packet.data() + wire::header_size,
            packet.size() - wire::header_size, packet.data(), wire::header_size, n.data(),
            keys_.receive.data()) != 0)
        return {};
    if (!replay_.accept(decoded->counter))
        return {};
    header = *decoded;
    return static_cast<size_t>(size);
}
} // namespace pocketvr
