// SPDX-License-Identifier: Apache-2.0
#include "discovery.hpp"
#include <sodium.h>
#include <algorithm>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string_view>

namespace pocketvr::android {
namespace {
bool usb_interface(std::string_view name) {
    const auto prefix = name.starts_with("rndis") ? 5U :
        (name.starts_with("usb") || name.starts_with("ncm")) ? 3U : 0U;
    return prefix && name.size() > prefix &&
        std::all_of(name.begin() + prefix, name.end(), [](char c) { return c >= '0' && c <= '9'; });
}
bool usb_peer(uint32_t peer) {
    ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) return false;
    bool found = false;
    for (auto *nic = interfaces; nic; nic = nic->ifa_next) {
        if (!nic->ifa_name || !usb_interface(nic->ifa_name) || !(nic->ifa_flags & IFF_UP) ||
            !nic->ifa_addr || !nic->ifa_netmask || nic->ifa_addr->sa_family != AF_INET) continue;
        const auto local = ntohl(reinterpret_cast<const sockaddr_in *>(nic->ifa_addr)->sin_addr.s_addr);
        const auto mask = ntohl(reinterpret_cast<const sockaddr_in *>(nic->ifa_netmask)->sin_addr.s_addr);
        if (mask && peer != local && (peer & mask) == (local & mask)) found = true;
    }
    freeifaddrs(interfaces);
    return found;
}
}
void DiscoveryScanner::start() {
    stop();
    {
        std::lock_guard lock(mutex_);
        candidates_.clear();
        candidates_.reserve(16);
    }
    worker_ = Worker([this](StopToken stop) { run(stop); });
}
void DiscoveryScanner::stop() {
    worker_.request_stop();
    if (worker_.joinable())
        worker_.join();
}
void DiscoveryScanner::run(StopToken stop) noexcept {
    try {
        UdpTransport socket({0, 0}, true);
        socket.broadcast_enabled(true);
        DiscoveryNonce nonce{};
        randombytes_buf(nonce.data(), nonce.size());
        uint64_t next_query{}, next_probe{}, next_nonce = monotonic_ns() + 5'000'000'000;
        std::array<uint8_t, wire::max_datagram> bytes{};
        while (!stop.stop_requested()) {
            const auto now = monotonic_ns();
            if (now >= next_nonce) {
                randombytes_buf(nonce.data(), nonce.size());
                next_nonce = now + 5'000'000'000;
            }
            if (now >= next_query) {
                const auto query = discovery_query(nonce);
                (void)socket.send(query, {0xffffffff, 9944});
                // Limited broadcast follows the default route, which is often Wi-Fi.
                // Also query each local subnet, including the phone's tether NIC.
                ifaddrs *interfaces = nullptr;
                if (getifaddrs(&interfaces) == 0) {
                    unsigned sent = 0;
                    unsigned probes = 0;
                    for (auto *nic = interfaces; nic && sent < 16; nic = nic->ifa_next) {
                        if (!(nic->ifa_flags & IFF_UP) || !(nic->ifa_flags & IFF_BROADCAST) ||
                            !nic->ifa_broadaddr || nic->ifa_broadaddr->sa_family != AF_INET)
                            continue;
                        const auto *ip = reinterpret_cast<const sockaddr_in *>(nic->ifa_broadaddr);
                        (void)socket.send(query, {ntohl(ip->sin_addr.s_addr), 9944});
                        ++sent;
                        // Windows listeners bound to a unicast address need not
                        // receive broadcasts. Probe only the small local USB
                        // tether subnet; never sweep Wi-Fi, VPNs or large ranges.
                        const std::string_view name(nic->ifa_name ? nic->ifa_name : "");
                        if (!usb_interface(name) ||
                            !nic->ifa_addr || !nic->ifa_netmask || nic->ifa_addr->sa_family != AF_INET)
                            continue;
                        const auto local = ntohl(reinterpret_cast<const sockaddr_in *>(nic->ifa_addr)->sin_addr.s_addr);
                        const auto mask = ntohl(reinterpret_cast<const sockaddr_in *>(nic->ifa_netmask)->sin_addr.s_addr);
                        const auto hosts = ~mask;
                        if (hosts < 3 || hosts > 255 || (hosts & (hosts + 1)) != 0)
                            continue;
                        bool known = false;
                        {
                            std::lock_guard lock(mutex_);
                            for (const auto &candidate : candidates_) {
                                if (now - candidate.seen < 10'000'000'000 &&
                                    (candidate.endpoint.address & mask) == (local & mask)) {
                                    (void)socket.send(query, {candidate.endpoint.address, 9944});
                                    known = true;
                                }
                            }
                        }
                        if (known || now < next_probe)
                            continue;
                        for (uint32_t host = 1; host < hosts && probes < 254; ++host) {
                            Endpoint target{(local & mask) | host, 9944};
                            if (target.address != local && target.private_or_local()) {
                                (void)socket.send(query, target);
                                ++probes;
                            }
                        }
                    }
                    if (probes) next_probe = now + 5'000'000'000;
                    freeifaddrs(interfaces);
                }
                next_query = now + 1'000'000'000;
                std::lock_guard lock(mutex_);
                std::erase_if(candidates_,
                              [&](const auto &value) { return now - value.seen > 10'000'000'000; });
            }
            Endpoint sender;
            const auto size = socket.receive(bytes, sender, std::chrono::milliseconds(50));
            if (!size || !sender.private_or_local())
                continue;
            const auto server = parse_discovery_reply(std::span(bytes).first(*size), nonce);
            if (!server)
                continue;
            sender.port = server->control_port;
            std::lock_guard lock(mutex_);
            const auto found =
                std::find_if(candidates_.begin(), candidates_.end(),
                             [&](const auto &candidate) { return candidate.endpoint == sender; });
            if (found != candidates_.end())
                *found = {sender, *server, now};
            else if (candidates_.size() < 16)
                candidates_.push_back({sender, *server, now});
        }
    } catch (...) { /* Manual IP and Android NSD remain available. */
    }
}
nlohmann::json DiscoveryScanner::results() const {
    std::lock_guard lock(mutex_);
    auto values = nlohmann::json::array();
    const auto now = monotonic_ns();
    for (const auto &candidate : candidates_) {
        if (now - candidate.seen > 10'000'000'000)
            continue;
        std::array<char, 65> identity{};
        sodium_bin2hex(identity.data(), identity.size(), candidate.server.identity.data(),
                      candidate.server.identity.size());
        values.push_back({{"usb_tether", usb_peer(candidate.endpoint.address)},
                          {"identity", identity.data()}, {"ip", candidate.endpoint.ip()},
                          {"port", candidate.endpoint.port},
                          {"name", candidate.server.name}});
    }
    return values;
}
} // namespace pocketvr::android
