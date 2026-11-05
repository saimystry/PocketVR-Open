// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/session.hpp>
#include <stdexcept>
#include <algorithm>

namespace pocketvr {
namespace {
void send(TcpStream &stream, uint8_t kind, std::span<const uint8_t> bytes) {
    std::array<uint8_t, PairingHandshake::hello_size + 1> message{};
    if (bytes.size() > message.size() - 1)
        throw std::logic_error("Handshake message size");
    message[0] = kind;
    std::copy(bytes.begin(), bytes.end(), message.begin() + 1);
    if (!stream.send_record(std::span(message).first(bytes.size() + 1),
                            std::chrono::milliseconds(1000)))
        throw std::runtime_error("Handshake send failed");
}
wire::Bytes receive(TcpStream &stream, uint8_t kind, size_t size, StopToken stop,
                    std::chrono::seconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!stop.stop_requested() && stream.alive() &&
           std::chrono::steady_clock::now() < deadline) {
        auto message = stream.receive_record(std::chrono::milliseconds(100));
        if (!message)
            continue;
        if (message->size() != size + 1 || (*message)[0] != kind)
            throw std::runtime_error("Unexpected handshake message");
        return {message->begin() + 1, message->end()};
    }
    throw std::runtime_error("Handshake cancelled, disconnected or timed out");
}
} // namespace
SessionMaterial authenticate_session(TcpStream &stream, PeerRole role, const Identity &identity,
                                     const PairingPolicy &policy, StopToken stop) {
    try {
        PairingHandshake handshake(role, identity, policy.expected_identity);
        send(stream, 1, handshake.commitment());
        if (!handshake.receive_commitment(receive(stream, 1, 32, stop)))
            throw std::runtime_error("Handshake commitment rejected");
        send(stream, 2, handshake.reveal());
        if (!handshake.receive_reveal(receive(stream, 2, PairingHandshake::hello_size, stop)))
            throw std::runtime_error("Handshake identity or reveal rejected");
        send(stream, 3, handshake.signature());
        if (!handshake.receive_signature(receive(stream, 3, 64, stop)))
            throw std::runtime_error("Handshake signature rejected");
        const auto stored_identity = policy.expected_identity ? policy.expected_identity :
            policy.lookup_identity ? policy.lookup_identity(handshake.peer_identity()) : std::nullopt;
        if (stored_identity && *stored_identity != handshake.peer_identity())
            throw std::runtime_error("Stored identity does not match the authenticated peer");
        const bool locally_known = stored_identity.has_value();
        send(stream, 5, handshake.pin_claim(locally_known));
        const auto peer_known = handshake.receive_pin_claim(receive(stream, 5, 33, stop));
        if (!peer_known.has_value())
            throw std::runtime_error("Peer identity-pin state authentication failed");
        const bool known = locally_known && *peer_known;
        if (!known && (!policy.allow_new || !policy.approve))
            throw std::runtime_error("Unknown peer");
        auto approve = [&] {
            if (stop.stop_requested())
                throw std::runtime_error("Pairing cancelled");
            if (known) {
                const auto current = policy.expected_identity ? policy.expected_identity :
                    policy.lookup_identity ? policy.lookup_identity(handshake.peer_identity()) : std::nullopt;
                if (!current || current != stored_identity || !handshake.approve_known(*current))
                    throw std::runtime_error("Pinned identity approval failed");
            } else {
                if (!policy.approve(handshake.peer_identity(), stop) || !handshake.approve_new())
                    throw std::runtime_error("New device was not approved");
            }
        };
        if (role == PeerRole::pc) {
            approve();
            send(stream, 4, handshake.confirmation());
            if (!handshake.receive_confirmation(
                    receive(stream, 4, 32, stop, std::chrono::seconds(120))))
                throw std::runtime_error("Phone confirmation rejected");
        } else {
            if (!known && policy.waiting)
                policy.waiting(handshake.peer_identity());
            if (!handshake.receive_confirmation(
                    receive(stream, 4, 32, stop, std::chrono::seconds(120))))
                throw std::runtime_error("PC confirmation rejected");
            approve();
            send(stream, 4, handshake.confirmation());
        }
        return handshake.session();
    } catch (...) {
        stream.interrupt();
        throw;
    }
}
void ControlChannel::send(const nlohmann::json &message) {
    if (!message.is_object())
        throw std::invalid_argument("Control message must be an object");
    const auto text = message.dump();
    const auto record =
        cipher_.seal(std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
    if (!record || !stream_.send_record(*record, std::chrono::milliseconds(1000))) {
        stream_.interrupt();
        throw std::runtime_error("Encrypted control send failed");
    }
}
std::optional<nlohmann::json> ControlChannel::receive(std::chrono::milliseconds timeout) {
    const auto record = stream_.receive_record(timeout);
    if (!record)
        return {};
    try {
        const auto plain = cipher_.open(*record);
        if (!plain)
            throw std::runtime_error("Control authentication failed");
        auto message = nlohmann::json::parse(
            plain->begin(), plain->end(),
            [](int depth, nlohmann::json::parse_event_t, nlohmann::json &) {
                if (depth > 32)
                    throw std::invalid_argument("Control JSON nesting exceeds 32 levels");
                return true;
            });
        if (!message.is_object())
            throw std::invalid_argument("Control message must be an object");
        return message;
    } catch (...) {
        stream_.interrupt();
        throw;
    }
}
} // namespace pocketvr
