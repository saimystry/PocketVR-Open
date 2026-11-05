// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "pairing.hpp"
#include "tcp.hpp"
#include <functional>
#include <nlohmann/json.hpp>
#include "worker.hpp"

namespace pocketvr {
struct PairingPolicy {
    bool allow_new{};
    std::optional<Key> expected_identity;
    // Return the actual stored pin, not a host/address trust boolean. Invoked
    // again before approval to fail closed if the record was revoked meanwhile.
    std::function<std::optional<Key>(const Key &)> lookup_identity;
    // Tells the phone that the PC is deciding whether to accept it.
    std::function<void(const Key &)> waiting;
    // Decides whether to trust a new identity. May block while the owner answers.
    std::function<bool(const Key &, StopToken)> approve;
};
SessionMaterial authenticate_session(TcpStream &stream, PeerRole role, const Identity &identity,
                                     const PairingPolicy &policy, StopToken stop = {});
// Single control-thread owner; no caller may send or receive concurrently.
class ControlChannel final {
  public:
    ControlChannel(TcpStream &stream, const SessionMaterial &material)
        : stream_(stream), cipher_(material) {}
    void send(const nlohmann::json &message);
    std::optional<nlohmann::json> receive(std::chrono::milliseconds timeout);

  private:
    TcpStream &stream_;
    ControlCipher cipher_;
};
} // namespace pocketvr
