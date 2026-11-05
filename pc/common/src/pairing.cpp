// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/pairing.hpp>
#include <sodium.h>
#include <algorithm>
#include <stdexcept>

namespace pocketvr {
namespace {
void sodium_ready() {
    if (sodium_init() < 0)
        throw std::runtime_error("libsodium initialization failed");
}
Key digest(std::string_view domain, std::span<const uint8_t> bytes, const Key *key = nullptr) {
    crypto_generichash_state hash;
    crypto_generichash_init(&hash, key ? key->data() : nullptr, key ? key->size() : 0, 32);
    crypto_generichash_update(&hash, reinterpret_cast<const uint8_t *>(domain.data()),
                              domain.size());
    crypto_generichash_update(&hash, bytes.data(), bytes.size());
    Key out{};
    crypto_generichash_final(&hash, out.data(), out.size());
    return out;
}
SessionKeys derive(const SessionKeys &base, std::string_view domain,
                   std::span<const uint8_t> transcript) {
    return {digest(domain, transcript, &base.transmit), digest(domain, transcript, &base.receive)};
}
} // namespace
Identity::Identity() {
    sodium_ready();
    if (crypto_sign_keypair(public_.data(), secret_.data()))
        throw std::runtime_error("Ed25519 identity generation failed");
}
Identity::Identity(const Key &seed) {
    sodium_ready();
    if (crypto_sign_seed_keypair(public_.data(), secret_.data(), seed.data()))
        throw std::runtime_error("Ed25519 identity restoration failed");
}
Identity::~Identity() {
    sodium_memzero(secret_.data(), secret_.size());
}
Key Identity::seed() const {
    Key seed{};
    crypto_sign_ed25519_sk_to_seed(seed.data(), secret_.data());
    return seed;
}
std::array<uint8_t, 64> Identity::sign(std::span<const uint8_t> message) const {
    std::array<uint8_t, 64> signature{};
    if (crypto_sign_detached(signature.data(), nullptr, message.data(), message.size(),
                             secret_.data()))
        throw std::runtime_error("Ed25519 signature failed");
    return signature;
}
PairingHandshake::PairingHandshake(PeerRole role, const Identity &identity,
                                   std::optional<Key> pinned)
    : role_(role), identity_(identity), pinned_(pinned) {
    if (role != PeerRole::pc && role != PeerRole::phone)
        throw std::invalid_argument("Pairing role");
    wire::Writer writer(local_);
    writer.u16(1);
    writer.u16(0);
    writer.u8(static_cast<uint8_t>(role));
    std::copy(identity.public_key().begin(), identity.public_key().end(), local_.begin() + 5);
    std::copy(ephemeral_.public_key.begin(), ephemeral_.public_key.end(), local_.begin() + 37);
    randombytes_buf(local_.data() + 69, 32);
}
PairingHandshake::~PairingHandshake() {
    sodium_memzero(&media_, sizeof(media_));
    sodium_memzero(&control_, sizeof(control_));
    sodium_memzero(&proofs_, sizeof(proofs_));
}
void PairingHandshake::require(bool condition) const {
    if (failed_ || !condition)
        throw std::logic_error("Pairing handshake state");
}
Key PairingHandshake::commitment() const {
    require(!revealed_);
    return digest("PocketVR hello commitment v1", local_);
}
bool PairingHandshake::receive_commitment(std::span<const uint8_t> commitment) {
    if (failed_ || committed_ || commitment.size() != peer_commit_.size())
        return fail();
    std::copy(commitment.begin(), commitment.end(), peer_commit_.begin());
    committed_ = true;
    return true;
}
const PairingHandshake::Hello &PairingHandshake::reveal() const {
    require(committed_);
    return local_;
}
bool PairingHandshake::receive_reveal(std::span<const uint8_t> hello) {
    if (failed_ || !committed_ || revealed_ || hello.size() != hello_size)
        return fail();
    const auto hash = digest("PocketVR hello commitment v1", hello);
    if (sodium_memcmp(hash.data(), peer_commit_.data(), hash.size()))
        return fail();
    wire::Reader reader(hello);
    uint16_t major{}, minor{};
    uint8_t role{};
    if (!reader.u16(major) || !reader.u16(minor) || !reader.u8(role) || major != 1 || minor != 0 ||
        role != static_cast<uint8_t>(role_ == PeerRole::pc ? PeerRole::phone : PeerRole::pc))
        return fail();
    std::copy(hello.begin(), hello.end(), peer_.begin());
    std::copy_n(peer_.begin() + 5, 32, peer_identity_.begin());
    if (sodium_memcmp(peer_identity_.data(), identity_.public_key().data(), 32) == 0 ||
        (pinned_ && sodium_memcmp(pinned_->data(), peer_identity_.data(), 32)))
        return fail();
    crypto_generichash_state state;
    crypto_generichash_init(&state, nullptr, 0, transcript_.size());
    constexpr std::string_view domain = "PocketVR mutually signed pairing transcript v1";
    crypto_generichash_update(&state, reinterpret_cast<const uint8_t *>(domain.data()),
                              domain.size());
    const auto &pc = role_ == PeerRole::pc ? local_ : peer_;
    const auto &phone = role_ == PeerRole::phone ? local_ : peer_;
    crypto_generichash_update(&state, pc.data(), pc.size());
    crypto_generichash_update(&state, phone.data(), phone.size());
    crypto_generichash_final(&state, transcript_.data(), transcript_.size());
    revealed_ = true;
    return true;
}
std::array<uint8_t, 64> PairingHandshake::signature() const {
    require(revealed_);
    return identity_.sign(transcript_);
}
bool PairingHandshake::receive_signature(std::span<const uint8_t> signature) {
    if (failed_ || !revealed_ || authenticated_ || signature.size() != 64 ||
        crypto_sign_verify_detached(signature.data(), transcript_.data(), transcript_.size(),
                                    peer_identity_.data()))
        return fail();
    Key ephemeral_peer{};
    std::copy_n(peer_.begin() + 37, 32, ephemeral_peer.begin());
    try {
        auto base =
            exchange_keys(ephemeral_, ephemeral_peer, role_ == PeerRole::phone, transcript_);
        media_ = derive(base, "PocketVR UDP v1", transcript_);
        control_ = derive(base, "PocketVR TCP records v1", transcript_);
        proofs_ = derive(base, "PocketVR approval proofs v1", transcript_);
        sodium_memzero(&base, sizeof(base));
    } catch (...) {
        return fail();
    }
    authenticated_ = true;
    return true;
}
std::array<uint8_t, 33> PairingHandshake::pin_claim(bool locally_known) {
    require(authenticated_ && !claimed_);
    std::array<uint8_t, 65> input{};
    std::copy(transcript_.begin(), transcript_.end(), input.begin());
    input.back() = locally_known ? 1 : 0;
    const auto mac = digest("PocketVR pinned identity state v1", input, &proofs_.transmit);
    std::array<uint8_t, 33> claim{};
    claim[0] = input.back();
    std::copy(mac.begin(), mac.end(), claim.begin() + 1);
    claimed_ = true;
    locally_known_ = locally_known;
    return claim;
}
std::optional<bool> PairingHandshake::receive_pin_claim(std::span<const uint8_t> claim) {
    if (failed_ || !authenticated_ || !claimed_ || peer_claimed_ || claim.size() != 33 ||
        claim[0] > 1) {
        fail();
        return {};
    }
    std::array<uint8_t, 65> input{};
    std::copy(transcript_.begin(), transcript_.end(), input.begin());
    input.back() = claim[0];
    const auto expected = digest("PocketVR pinned identity state v1", input, &proofs_.receive);
    if (sodium_memcmp(expected.data(), claim.data() + 1, expected.size())) {
        fail();
        return {};
    }
    peer_known_ = claim[0] == 1;
    peer_claimed_ = true;
    return peer_known_;
}
bool PairingHandshake::approve_new() {
    if (failed_ || !authenticated_ || !claimed_ || !peer_claimed_ || approved_)
        return fail();
    approved_ = true;
    return true;
}
bool PairingHandshake::approve_known() {
    if (failed_ || !authenticated_ || !claimed_ || !peer_claimed_ || !locally_known_ ||
        !peer_known_ || approved_ || !pinned_)
        return fail();
    approved_ = true;
    return true;
}
bool PairingHandshake::approve_known(const Key &stored_identity) {
    if (failed_ || !authenticated_ || !claimed_ || !peer_claimed_ || !locally_known_ ||
        !peer_known_ || approved_ ||
        sodium_memcmp(stored_identity.data(), peer_identity_.data(), stored_identity.size()))
        return fail();
    approved_ = true;
    return true;
}
Key PairingHandshake::confirmation() const {
    require(authenticated_ && approved_);
    return digest("PocketVR user approval v1", transcript_, &proofs_.transmit);
}
bool PairingHandshake::receive_confirmation(std::span<const uint8_t> proof) {
    if (failed_ || !authenticated_ || !claimed_ || !peer_claimed_ || peer_approved_ ||
        proof.size() != 32)
        return fail();
    const auto expected = digest("PocketVR user approval v1", transcript_, &proofs_.receive);
    if (sodium_memcmp(expected.data(), proof.data(), expected.size()))
        return fail();
    peer_approved_ = true;
    return true;
}
bool PairingHandshake::ready() const noexcept {
    return !failed_ && authenticated_ && claimed_ && peer_claimed_ && approved_ && peer_approved_;
}
SessionMaterial PairingHandshake::session() const {
    require(ready());
    const auto hash = digest("PocketVR session identifier v1", transcript_);
    uint32_t id{};
    for (unsigned i = 0; i < 4; ++i)
        id |= static_cast<uint32_t>(hash[i]) << (8 * i);
    return {id ? id : 1, peer_identity_, media_, control_};
}
const Key &PairingHandshake::peer_identity() const {
    require(authenticated_);
    return peer_identity_;
}
ControlCipher::ControlCipher(const SessionMaterial &session)
    : session_(session.id), keys_(session.control) {
    sodium_ready();
    if (!session_)
        throw std::invalid_argument("Zero control session");
}
ControlCipher::~ControlCipher() {
    sodium_memzero(&keys_, sizeof(keys_));
}
std::optional<wire::Bytes> ControlCipher::seal(std::span<const uint8_t> plaintext) {
    if (failed_ || plaintext.empty() || plaintext.size() > max_plaintext || sent_ == UINT64_MAX)
        return {};
    wire::Bytes record(16 + plaintext.size() + 16);
    wire::Writer writer(record);
    writer.u32(session_);
    writer.u64(++sent_);
    writer.u32(static_cast<uint32_t>(plaintext.size()));
    unsigned long long size{};
    if (crypto_aead_chacha20poly1305_ietf_encrypt(record.data() + 16, &size, plaintext.data(),
                                                  plaintext.size(), record.data(), 16, nullptr,
                                                  record.data(), keys_.transmit.data())) {
        failed_ = true;
        return {};
    }
    return record;
}
std::optional<wire::Bytes> ControlCipher::open(std::span<const uint8_t> record) {
    auto reject = [&]() -> std::optional<wire::Bytes> {
        failed_ = true;
        return {};
    };
    if (failed_ || record.size() <= 32 || record.size() > wire::max_control ||
        received_ == UINT64_MAX)
        return reject();
    wire::Reader reader(record.first(16));
    uint32_t session{}, length{};
    uint64_t counter{};
    if (!reader.u32(session) || !reader.u64(counter) || !reader.u32(length) ||
        session != session_ || counter != received_ + 1 || length != record.size() - 32)
        return reject();
    wire::Bytes plaintext(length);
    unsigned long long actual{};
    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            plaintext.data(), &actual, nullptr, record.data() + 16, record.size() - 16,
            record.data(), 16, record.data(), keys_.receive.data()) ||
        actual != length)
        return reject();
    received_ = counter;
    return plaintext;
}
} // namespace pocketvr
