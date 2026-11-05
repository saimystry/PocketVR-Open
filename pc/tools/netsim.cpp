// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/transport.hpp>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <iostream>
#include <queue>
#include <random>

using namespace pocketvr;
namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) {
    stopped = 1;
}
} // namespace
struct Queued {
    uint64_t due{};
    Endpoint destination;
    wire::Bytes bytes;
    bool operator<(const Queued &other) const {
        return due > other.due;
    }
};
int main(int argc, char **argv) {
    try {
        Endpoint listen = Endpoint::parse("127.0.0.1", 9945),
                 target = Endpoint::parse("127.0.0.1", 9944);
        double delay_ms = 0, jitter_ms = 0, loss = 0, mbps = 0, good_bad = 0, bad_good = 1,
               bad_loss = 1;
        uint32_t seed = 1;
        uint64_t duration = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout
                    << "Usage: pocketvr_netsim [--listen-port 9945] [--target-ip 127.0.0.1] "
                       "[--target-port 9944] [--delay-ms N] [--jitter-ms N] [--loss 0..1] [--mbps "
                       "N] [--good-bad 0..1] [--bad-good 0..1] [--bad-loss 0..1] [--seed N] "
                       "[--duration-ms N]\nBinds loopback only; simulates both directions. Delay "
                       "queue is bounded to 8 MiB/8192 packets and 200 ms bandwidth backlog.\n";
                return 0;
            }
            if (i + 1 >= argc)
                throw std::invalid_argument("Missing argument value");
            const std::string value = argv[++i];
            if (arg == "--target-ip")
                target = Endpoint::parse(value, target.port);
            else if (arg == "--listen-port" || arg == "--target-port") {
                const auto port = std::stoul(value);
                if (port == 0 || port > 65535)
                    throw std::invalid_argument("Port range");
                if (arg == "--listen-port")
                    listen.port = static_cast<uint16_t>(port);
                else
                    target.port = static_cast<uint16_t>(port);
            } else if (arg == "--delay-ms")
                delay_ms = std::stod(value);
            else if (arg == "--jitter-ms")
                jitter_ms = std::stod(value);
            else if (arg == "--loss")
                loss = std::stod(value);
            else if (arg == "--mbps")
                mbps = std::stod(value);
            else if (arg == "--good-bad")
                good_bad = std::stod(value);
            else if (arg == "--bad-good")
                bad_good = std::stod(value);
            else if (arg == "--bad-loss")
                bad_loss = std::stod(value);
            else if (arg == "--seed")
                seed = static_cast<uint32_t>(std::stoul(value));
            else if (arg == "--duration-ms")
                duration = std::stoull(value);
            else
                throw std::invalid_argument("Unknown option");
        }
        for (double v : {delay_ms, jitter_ms, loss, mbps, good_bad, bad_good, bad_loss})
            if (!std::isfinite(v) || v < 0)
                throw std::invalid_argument("Invalid impairment value");
        if (delay_ms > 1000 || jitter_ms > 1000 || loss > 1 || good_bad > 1 || bad_good > 1 ||
            bad_loss > 1 || mbps > 10000 || (mbps > 0 && mbps < 0.01) || !target.private_or_local())
            throw std::invalid_argument("Impairment range");
        UdpTransport socket(listen);
        std::optional<Endpoint> client;
        std::priority_queue<Queued> queue;
        size_t queued_bytes = 0;
        std::mt19937 random(seed);
        std::uniform_real_distribution<double> chance(0, 1);
        std::uniform_real_distribution<double> jitter(-jitter_ms, jitter_ms);
        bool bad = false;
        uint64_t received = 0, dropped = 0, sent = 0;
        std::array<uint64_t, 2> next_send{};
        const auto start = monotonic_ns();
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        std::array<uint8_t, wire::max_datagram> buffer{};
        std::cout << "{\"listening\":" << listen.port << "}" << std::endl;
        while (!stopped && (duration == 0 || (monotonic_ns() - start) / 1'000'000 < duration)) {
            Endpoint from;
            const auto count = socket.receive(buffer, from, std::chrono::milliseconds(1));
            const auto now = monotonic_ns();
            if (count) {
                ++received;
                const bool reverse = from == target;
                if (!reverse) {
                    if (client && *client != from) {
                        ++dropped;
                        continue;
                    }
                    client = from;
                }
                if (reverse && !client) {
                    ++dropped;
                    continue;
                }
                const auto destination = reverse ? *client : target;
                if (bad) {
                    if (chance(random) < bad_good)
                        bad = false;
                } else if (chance(random) < good_bad)
                    bad = true;
                if (chance(random) < (bad ? bad_loss : loss) || queue.size() >= 8192 ||
                    queued_bytes + *count > 8 * 1024 * 1024) {
                    ++dropped;
                    continue;
                }
                auto due =
                    now + static_cast<uint64_t>(std::max(0.0, delay_ms + jitter(random)) * 1e6);
                const size_t direction = reverse ? 1 : 0;
                if (mbps > 0) {
                    const auto baseline = due;
                    due = std::max(due, next_send[direction]);
                    const auto interval =
                        static_cast<uint64_t>(static_cast<double>(*count) * 8000 / mbps);
                    if (due - baseline > 200'000'000) {
                        ++dropped;
                        continue;
                    }
                    next_send[direction] = due + interval;
                }
                queued_bytes += *count;
                queue.push({due,
                            destination,
                            {buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(*count)}});
            }
            while (!queue.empty() && queue.top().due <= now) {
                const auto &item = queue.top();
                if (socket.send(item.bytes, item.destination))
                    ++sent;
                else
                    ++dropped;
                queued_bytes -= item.bytes.size();
                queue.pop();
            }
        }
        std::cout << "{\"received\":" << received << ",\"sent\":" << sent
                  << ",\"dropped\":" << dropped << ",\"pending\":" << queue.size() << "}\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
