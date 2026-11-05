// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "crypto.hpp"
#include <string>

namespace pocketvr {
class Identity final {
  public:
    Identity();
    explicit Identity(const Key &seed);
    ~Identity();
    Identity(const Identity &) = delete;
    Identity &operator=(const Identity &) = delete;
    const Key &public_key() const {
        return public_;
    }
    Key seed() const;
    std::array<uint8_t, 64> sign(std::span<const uint8_t> message) const;

  private:
    Key public_{};
    std::array<uint8_t, 64> secret_{};
};
enum class PeerRole : uint8_t { pc = 1, phone = 2 };
struct SessionMaterial {
    uint32_t id{};
    Key peer_identity{};
    SessionKeys media{}, control{};
};
// Commit both hellos before revealing either. Each side must approve the other:
// new identities through approve_new(), remembered ones through approve_known().
// One instance is single-use and confined to the control-session thread.
class PairingHandshake final {
  public:
    static constexpr size_t hello_size = 101;
    using Hello = std::array<uint8_t, hello_size>;
    PairingHandshake(PeerRole role, const Identity &identity, std::optional<Key> pinned = {});
    ~PairingHandshake();
    PairingHandshake(const PairingHandshake &) = delete;
    PairingHandshake &operator=(const PairingHandshake &) = delete;
    Key commitment() const;
    bool receive_commitment(std::span<const uint8_t> commitment);
    const Hello &reveal() const;
    bool receive_reveal(std::span<const uint8_t> hello);
    std::array<uint8_t, 64> signature() const;
    bool receive_signature(std::span<const uint8_t> signature);
    std::array<uint8_t, 33> pin_claim(bool locally_known);
    std::optional<bool> receive_pin_claim(std::span<const uint8_t> claim);
    bool approve_new();
    bool approve_known();
    bool approve_known(const Key &stored_identity);
    Key confirmation() const;
    bool receive_confirmation(std::span<const uint8_t> proof);
    bool ready() const noexcept;
    SessionMaterial session() const;
    const Key &peer_identity() const;
    bool failed() const noexcept {
        return failed_;
    }

  private:
    PeerRole role_;
    const Identity &identity_;
    std::optional<Key> pinned_;
    KeyPair ephemeral_;
    Hello local_{}, peer_{};
    Key peer_commit_{}, peer_identity_{};
    std::array<uint8_t, 64> transcript_{};
    SessionKeys media_{}, control_{}, proofs_{};
    bool committed_{}, revealed_{}, authenticated_{}, approved_{}, peer_approved_{}, failed_{};
    bool claimed_{}, peer_claimed_{}, locally_known_{}, peer_known_{};
    bool fail() noexcept {
        failed_ = true;
        return false;
    }
    void require(bool condition) const;
};
// Reliable control records: strict consecutive counters, authenticated lengths,
// and keys separate from UDP. Any bad record permanently closes this direction.
class ControlCipher final {
  public:
    static constexpr size_t max_plaintext = wire::max_control - 32;
    explicit ControlCipher(const SessionMaterial &session);
    ~ControlCipher();
    ControlCipher(const ControlCipher &) = delete;
    ControlCipher &operator=(const ControlCipher &) = delete;
    std::optional<wire::Bytes> seal(std::span<const uint8_t> plaintext);
    std::optional<wire::Bytes> open(std::span<const uint8_t> record);
    bool failed() const noexcept {
        return failed_;
    }

  private:
    uint32_t session_{};
    SessionKeys keys_{};
    uint64_t sent_{}, received_{};
    bool failed_{};
};
} // namespace pocketvr
