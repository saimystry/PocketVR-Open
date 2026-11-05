# Pairing and control authentication v1

Each handshake message is a TCP record (u32 little-endian body length), followed by a one-byte kind and its fixed-size body. Every handshake object is single-use; invalid/repeated messages fail permanently.

1. COMMIT (kind 1, body 32): BLAKE2b-256 of domain `PocketVR hello commitment v1` followed by the sender's hello. Both commitments must be sent before either hello is revealed.
2. REVEAL (kind 2, body 101): major u16=1, minor u16=0, role u8 (PC=1, phone=2), Ed25519 identity public key (32), fresh X25519 public key (32), fresh random nonce (32). Verify the commitment, opposite role, exact version and any expected identity pin. Reject reflection.
3. SIGNATURE (kind 3, body 64): Ed25519 signature of the BLAKE2b-512 transcript: domain `PocketVR mutually signed pairing transcript v1`, PC hello, phone hello, in that order. Verify before displaying a code or deriving keys.
4. PIN CLAIM (kind 5, body 33): a one-byte locally-known flag followed by keyed BLAKE2b-256 of domain `PocketVR pinned identity state v1`, transcript, and flag, under the directional approval key. Exchange and verify both claims before approval. Skip user confirmation only when both sides have a matching stored identity pin. If either side has lost or forgotten its pin, require the complete first-use flow again.
5. APPROVAL (kind 4, body 32): keyed BLAKE2b-256 of domain `PocketVR user approval v1` and the transcript, under the directional approval key. Require local approval and a valid peer proof before exporting session keys.

X25519 uses libsodium's directional key exchange, further bound to the transcript. Derive independent keyed BLAKE2b-256 subkeys with labels `PocketVR UDP v1`, `PocketVR TCP records v1`, and `PocketVR approval proofs v1`, each followed by the transcript. See the [libsodium key exchange documentation](https://doc.libsodium.org/key_exchange) and [signature documentation](https://doc.libsodium.org/public-key_cryptography/public-key_signatures).

There is no pairing code. A new phone is accepted by the PC under the rules in `spec.md`, and each side then proves its approval with a keyed confirmation over the transcript.

PairingGate provides one concurrent new-peer attempt and at most five attempts per explicit 120-second window. The production listener applies it globally and starts with new pairing closed. A gate is serialized by its owner. Forgetting a connected identity interrupts its session; pin updates and revocation are serialized. Tests check implementation behavior; no independent protocol/security audit has been performed.

After authentication, the body is session_id u32, consecutive directional counter u64, plaintext_length u32, ciphertext, and a 16-byte ChaCha20-Poly1305-IETF tag. The first 16 bytes are associated data; the first 12 are the nonce. JSON plaintext is 1–65504 bytes. Receive counters must be exactly the next value because TCP is ordered. Any authentication, size, session or counter error permanently closes the cipher. The nonzero u32 session id comes from BLAKE2b-256(domain `PocketVR session identifier v1`, transcript), with zero mapped to one. Independent fresh keys make a session-id collision insufficient to reuse a nonce/key pair.

PC seeds and paired records are DPAPI protected under the user's roaming PocketVR directory (identity.dpapi, paired.json). A share-denied lock serializes setup/driver updates. Android seeds and PC pins are wrapped with an Android Keystore AES-GCM key; app backup is disabled. Corrupt/missing identity storage fails closed and is never silently replaced.
