// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/pairing.hpp>
#include <gtest/gtest.h>

using namespace pocketvr;
namespace {
void authenticate(PairingHandshake &pc, PairingHandshake &phone, bool known = false) {
    const auto a = pc.commitment(), b = phone.commitment();
    ASSERT_TRUE(pc.receive_commitment(b));
    ASSERT_TRUE(phone.receive_commitment(a));
    const auto ar = pc.reveal(), br = phone.reveal();
    ASSERT_TRUE(pc.receive_reveal(br));
    ASSERT_TRUE(phone.receive_reveal(ar));
    ASSERT_TRUE(pc.receive_signature(phone.signature()));
    ASSERT_TRUE(phone.receive_signature(pc.signature()));
    const auto pc_claim = pc.pin_claim(known), phone_claim = phone.pin_claim(known);
    ASSERT_EQ(pc.receive_pin_claim(phone_claim), known);
    ASSERT_EQ(phone.receive_pin_claim(pc_claim), known);
}
void approve(PairingHandshake &pc, PairingHandshake &phone) {
    ASSERT_TRUE(pc.approve_new());
    ASSERT_TRUE(phone.approve_new());
    ASSERT_TRUE(pc.receive_confirmation(phone.confirmation()));
    ASSERT_TRUE(phone.receive_confirmation(pc.confirmation()));
}
} // namespace
TEST(Pairing, MutualApprovalFreshKeysAndIdentityRestoration) {
    Identity pc_key, phone_key;
    Identity restored(pc_key.seed());
    EXPECT_EQ(restored.public_key(), pc_key.public_key());
    PairingHandshake pc(PeerRole::pc, pc_key), phone(PeerRole::phone, phone_key);
    EXPECT_THROW(pc.reveal(), std::logic_error);
    EXPECT_THROW(pc.session(), std::logic_error);
    authenticate(pc, phone);
    EXPECT_FALSE(pc.ready());
    EXPECT_THROW(pc.session(), std::logic_error);
    approve(pc, phone);
    ASSERT_TRUE(pc.ready());
    ASSERT_TRUE(phone.ready());
    const auto a = pc.session(), b = phone.session();
    EXPECT_EQ(a.id, b.id);
    EXPECT_EQ(a.peer_identity, phone_key.public_key());
    EXPECT_EQ(a.media.transmit, b.media.receive);
    EXPECT_EQ(a.control.receive, b.control.transmit);
    EXPECT_NE(a.media.transmit, a.control.transmit);
    PairingHandshake pc2(PeerRole::pc, restored, phone_key.public_key()),
        phone2(PeerRole::phone, phone_key, restored.public_key());
    authenticate(pc2, phone2, true);
    ASSERT_TRUE(pc2.approve_known());
    ASSERT_TRUE(phone2.approve_known());
    ASSERT_TRUE(pc2.receive_confirmation(phone2.confirmation()));
    ASSERT_TRUE(phone2.receive_confirmation(pc2.confirmation()));
    EXPECT_NE(pc2.session().media.transmit, a.media.transmit);
}
TEST(Pairing, TamperWrongRoleChangedIdentityAndWrongCodeFailClosed) {
    Identity pc_key, phone_key, stranger;
    PairingHandshake pc(PeerRole::pc, pc_key), phone(PeerRole::phone, phone_key);
    ASSERT_TRUE(pc.receive_commitment(phone.commitment()));
    ASSERT_TRUE(phone.receive_commitment(pc.commitment()));
    auto changed = phone.reveal();
    changed[69] ^= 1;
    EXPECT_FALSE(pc.receive_reveal(changed));
    EXPECT_TRUE(pc.failed());
    EXPECT_FALSE(pc.receive_reveal(phone.reveal()));
    PairingHandshake reflected(PeerRole::pc, pc_key);
    ASSERT_TRUE(reflected.receive_commitment(reflected.commitment()));
    EXPECT_FALSE(reflected.receive_reveal(reflected.reveal()));
    PairingHandshake pinned(PeerRole::pc, pc_key, stranger.public_key()),
        peer(PeerRole::phone, phone_key);
    ASSERT_TRUE(pinned.receive_commitment(peer.commitment()));
    ASSERT_TRUE(peer.receive_commitment(pinned.commitment()));
    EXPECT_FALSE(pinned.receive_reveal(peer.reveal()));
    PairingHandshake a(PeerRole::pc, pc_key), b(PeerRole::phone, phone_key);
    authenticate(a, b);
    EXPECT_THROW(a.session(), std::logic_error); // No session before both sides approve.
    EXPECT_TRUE(a.approve_new());
    EXPECT_FALSE(a.approve_new()); // Approval is single-use.
    EXPECT_THROW(a.session(), std::logic_error);
}
TEST(Pairing, InvalidSignatureAndCrossSessionProofAreRejected) {
    Identity pc_key, phone_key;
    PairingHandshake pc(PeerRole::pc, pc_key), phone(PeerRole::phone, phone_key);
    const auto a = pc.commitment(), b = phone.commitment();
    ASSERT_TRUE(pc.receive_commitment(b));
    ASSERT_TRUE(phone.receive_commitment(a));
    const auto ar = pc.reveal(), br = phone.reveal();
    ASSERT_TRUE(pc.receive_reveal(br));
    ASSERT_TRUE(phone.receive_reveal(ar));
    auto signature = phone.signature();
    signature[0] ^= 1;
    EXPECT_FALSE(pc.receive_signature(signature));
    EXPECT_THROW((void)pc.peer_identity(), std::logic_error);
    PairingHandshake first(PeerRole::pc, pc_key), second(PeerRole::phone, phone_key);
    authenticate(first, second);
    ASSERT_TRUE(first.approve_new());
    const auto proof = first.confirmation();
    PairingHandshake newer(PeerRole::pc, pc_key), newer_phone(PeerRole::phone, phone_key);
    authenticate(newer, newer_phone);
    EXPECT_FALSE(newer_phone.receive_confirmation(proof));
}
TEST(ControlSecurity, AuthenticatedLengthCounterAndStrictReplayFailure) {
    Identity pc_key, phone_key;
    PairingHandshake pc(PeerRole::pc, pc_key), phone(PeerRole::phone, phone_key);
    authenticate(pc, phone);
    approve(pc, phone);
    ControlCipher sender(pc.session()), receiver(phone.session());
    wire::Bytes message(60'000, 42);
    const auto first = sender.seal(message);
    ASSERT_TRUE(first);
    EXPECT_EQ(receiver.open(*first), message);
    const auto second = sender.seal(message);
    ASSERT_TRUE(second);
    EXPECT_EQ(receiver.open(*second), message);
    EXPECT_FALSE(receiver.open(*first));
    EXPECT_TRUE(receiver.failed());
    EXPECT_FALSE(receiver.open(*second));
    EXPECT_FALSE(sender.seal(wire::Bytes(ControlCipher::max_plaintext + 1)));
    ControlCipher tamper_receiver(phone.session());
    auto tampered = *first;
    tampered[16] ^= 1;
    EXPECT_FALSE(tamper_receiver.open(tampered));
    ControlCipher length_receiver(phone.session());
    tampered = *first;
    tampered[12] ^= 1;
    EXPECT_FALSE(length_receiver.open(tampered));
    ControlCipher skip_receiver(phone.session());
    EXPECT_FALSE(skip_receiver.open(*second));
}
