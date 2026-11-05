// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/transport.hpp>
#include <limits>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
using SocketLength = int;
static void close_socket(Socket s) {
    closesocket(s);
}
static void initialize_sockets() {
    struct Runtime {
        Runtime() {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
                throw std::runtime_error("WSAStartup failed");
        }
        ~Runtime() {
            WSACleanup();
        }
    };
    static Runtime runtime;
}
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <time.h>
using Socket = int;
using SocketLength = socklen_t;
static void close_socket(Socket s) {
    close(s);
}
static void initialize_sockets() {}
#endif

namespace pocketvr {
static sockaddr_in native(Endpoint e) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(e.address);
    address.sin_port = htons(e.port);
    return address;
}
static Endpoint endpoint(const sockaddr_in &a) {
    return {ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)};
}
Endpoint Endpoint::parse(const std::string &address, uint16_t port) {
    initialize_sockets();
    in_addr parsed{};
    if (inet_pton(AF_INET, address.c_str(), &parsed) != 1)
        throw std::invalid_argument("Expected an IPv4 literal address; IPv6 is not supported in v1");
    return {ntohl(parsed.s_addr), port};
}
std::string Endpoint::ip() const {
    initialize_sockets();
    in_addr a{};
    a.s_addr = htonl(address);
    std::array<char, INET_ADDRSTRLEN> text{};
    if (!inet_ntop(AF_INET, &a, text.data(), static_cast<SocketLength>(text.size())))
        throw std::runtime_error("inet_ntop failed");
    return text.data();
}
bool Endpoint::private_or_local() const {
    return (address & 0xff000000) == 0x0a000000 || (address & 0xfff00000) == 0xac100000 ||
           (address & 0xffff0000) == 0xc0a80000 || (address & 0xffff0000) == 0xa9fe0000 ||
           (address & 0xff000000) == 0x7f000000;
}
UdpTransport::UdpTransport(Endpoint local, bool allow_public)
    : socket_(~uintptr_t{}), public_(allow_public) {
    if (!allow_public && !local.private_or_local())
        throw std::invalid_argument("Bind requires a specific private/link-local/loopback address");
    initialize_sockets();
    const auto s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == static_cast<Socket>(-1))
        throw std::runtime_error("UDP socket creation failed");
    socket_ = static_cast<uintptr_t>(s);
#ifdef _WIN32
    u_long nonblocking = 1;
    const bool mode_failed = ioctlsocket(s, FIONBIO, &nonblocking) != 0;
#else
    const auto flags = fcntl(s, F_GETFL, 0);
    const bool mode_failed = flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) != 0;
#endif
    if (mode_failed) {
        close_socket(s);
        socket_ = ~uintptr_t{};
        throw std::runtime_error("UDP nonblocking mode failed");
    }
#ifdef __ANDROID__
    // Expedited forwarding. Wi-Fi queues it as voice, so head poses are not held behind
    // other traffic. Best effort: networks may ignore or clear it.
    const int priority = 0xb8;
    (void)setsockopt(s, IPPROTO_IP, IP_TOS, &priority, sizeof(priority));
#endif
    const auto address = native(local);
    int bytes = 2 * 1024 * 1024;
    const auto receive_tuned = setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&bytes), sizeof(bytes)) == 0;
    bytes = 256 * 1024;
    const auto send_tuned = setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&bytes), sizeof(bytes)) == 0;
    buffer_tuning_applied_ = receive_tuned && send_tuned;
    SocketLength receive_size = sizeof(receive_buffer_), send_size = sizeof(send_buffer_);
    if (getsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char *>(&receive_buffer_), &receive_size) ||
        getsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char *>(&send_buffer_), &send_size) ||
        receive_buffer_ <= 0 || send_buffer_ <= 0) {
        close_socket(s);
        socket_ = ~uintptr_t{};
        throw std::runtime_error("Cannot query effective UDP socket buffers");
    }
    if (bind(s, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        close_socket(s);
        socket_ = ~uintptr_t{};
        throw std::runtime_error("UDP bind failed");
    }
}
UdpTransport::~UdpTransport() {
    if (socket_ != ~uintptr_t{})
        close_socket(static_cast<Socket>(socket_));
}
void UdpTransport::broadcast_enabled(bool enabled) {
    int value = enabled ? 1 : 0;
    if (setsockopt(static_cast<Socket>(socket_), SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char *>(&value), sizeof(value)))
        throw std::runtime_error("UDP broadcast option failed");
}
// After a failed send: true once a full send buffer has room again. Waits at most
// 2 ms, because a packet this late is close to useless.
static bool room_to_send(Socket socket) {
#ifdef _WIN32
    if (WSAGetLastError() != WSAEWOULDBLOCK)
        return false;
    WSAPOLLFD descriptor{};
    descriptor.fd = socket;
    descriptor.events = POLLWRNORM;
    return WSAPoll(&descriptor, 1, 2) > 0;
#else
    if (errno != EAGAIN)
        return false;
    pollfd descriptor{socket, POLLOUT, 0};
    return poll(&descriptor, 1, 2) > 0;
#endif
}
bool UdpTransport::send(std::span<const uint8_t> message, const Endpoint &peer) {
    if (message.empty() || message.size() > wire::max_datagram ||
        (!public_ && !peer.private_or_local()) || peer.port == 0)
        return false;
    const auto destination = native(peer);
    const auto send_once = [&] {
        return sendto(static_cast<Socket>(socket_), reinterpret_cast<const char *>(message.data()),
                      static_cast<int>(message.size()), 0,
                      reinterpret_cast<const sockaddr *>(&destination), sizeof(destination));
    };
    auto result = send_once();
    if (result < 0 && room_to_send(static_cast<Socket>(socket_)))
        result = send_once();
    return result == static_cast<decltype(result)>(message.size());
}
std::optional<size_t> UdpTransport::receive(std::span<uint8_t> buffer, Endpoint &sender,
                                            std::chrono::milliseconds timeout) {
    if (buffer.empty() || buffer.size() > 65536 || timeout.count() < 0 || timeout.count() > 1000)
        return {};
    const auto s = static_cast<Socket>(socket_);
#ifdef _WIN32
    fd_set read{};
    FD_ZERO(&read);
    FD_SET(s, &read);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    const int selected = select(0, &read, nullptr, nullptr, &tv);
#else
    pollfd descriptor{s, POLLIN, 0};
    const int selected = poll(&descriptor, 1, static_cast<int>(timeout.count()));
#endif
    if (selected <= 0)
        return {};
    sockaddr_in source{};
    SocketLength length = sizeof(source);
    const auto result =
        recvfrom(s, reinterpret_cast<char *>(buffer.data()), static_cast<int>(buffer.size()), 0,
                 reinterpret_cast<sockaddr *>(&source), &length);
    if (result <= 0)
        return {};
    sender = endpoint(source);
    if (!public_ && !sender.private_or_local())
        return {};
    return static_cast<size_t>(result);
}
bool UdpTransport::wait_any(std::span<UdpTransport *const> sockets,
                            std::chrono::milliseconds timeout) {
    if (sockets.size() > 16)
        throw std::invalid_argument("At most 16 sockets can be waited on");
#ifdef _WIN32
    std::array<WSAPOLLFD, 16> descriptors{};
    for (size_t i = 0; i < sockets.size(); ++i) {
        descriptors[i].fd = static_cast<Socket>(sockets[i]->socket_);
        descriptors[i].events = POLLRDNORM;
    }
    const int ready = sockets.empty() ? -1 : WSAPoll(descriptors.data(), static_cast<ULONG>(sockets.size()),
                                                     static_cast<INT>(timeout.count()));
#else
    std::array<pollfd, 16> descriptors{};
    for (size_t i = 0; i < sockets.size(); ++i) {
        descriptors[i].fd = static_cast<Socket>(sockets[i]->socket_);
        descriptors[i].events = POLLIN;
    }
    const int ready = sockets.empty() ? -1 : poll(descriptors.data(), static_cast<nfds_t>(sockets.size()),
                                                  static_cast<int>(timeout.count()));
#endif
    if (ready < 0)
        std::this_thread::sleep_for(timeout); // Never let a failed wait become a busy loop.
    return ready > 0;
}
Endpoint UdpTransport::local_endpoint() const {
    sockaddr_in address{};
    SocketLength size = sizeof(address);
    if (getsockname(static_cast<Socket>(socket_), reinterpret_cast<sockaddr *>(&address), &size) !=
        0)
        throw std::runtime_error("getsockname failed");
    return endpoint(address);
}
uint64_t monotonic_ns() {
#ifdef __ANDROID__
    timespec timestamp{};
    if (clock_gettime(CLOCK_BOOTTIME, &timestamp))
        throw std::runtime_error("Android boot clock unavailable");
    return static_cast<uint64_t>(timestamp.tv_sec) * 1'000'000'000 +
           static_cast<uint64_t>(timestamp.tv_nsec);
#else
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
#endif
}
} // namespace pocketvr
