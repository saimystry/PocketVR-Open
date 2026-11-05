// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/tcp.hpp>
#include <algorithm>
#include <stdexcept>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace pocketvr {
namespace {
#ifdef _WIN32
using Socket = SOCKET;
using Length = int;
void initialize() {
    struct Runtime {
        Runtime() {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data))
                throw std::runtime_error("Winsock startup failed");
        }
        ~Runtime() {
            WSACleanup();
        }
    };
    static Runtime runtime;
}
void close_socket(Socket s) {
    closesocket(s);
}
bool would_block() {
    const auto e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
void nonblocking(Socket s) {
    u_long one = 1;
    if (ioctlsocket(s, FIONBIO, &one))
        throw std::runtime_error("TCP nonblocking mode failed");
}
constexpr int send_flags = 0;
#else
using Socket = int;
using Length = socklen_t;
void initialize() {}
void close_socket(Socket s) {
    close(s);
}
bool would_block() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EINTR;
}
void nonblocking(Socket s) {
    const auto flags = fcntl(s, F_GETFL, 0);
    if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK))
        throw std::runtime_error("TCP nonblocking mode failed");
}
constexpr int send_flags = MSG_NOSIGNAL;
#endif
constexpr Socket invalid = static_cast<Socket>(-1);
struct Guard {
    Socket socket{invalid};
    ~Guard() {
        if (socket != invalid)
            close_socket(socket);
    }
};
sockaddr_in native(Endpoint endpoint) {
    sockaddr_in value{};
    value.sin_family = AF_INET;
    value.sin_addr.s_addr = htonl(endpoint.address);
    value.sin_port = htons(endpoint.port);
    return value;
}
Endpoint endpoint(const sockaddr_in &value) {
    return {ntohl(value.sin_addr.s_addr), ntohs(value.sin_port)};
}
Endpoint local_endpoint(Socket socket) {
    sockaddr_in address{};
    Length size = sizeof(address);
    if (getsockname(socket, reinterpret_cast<sockaddr *>(&address), &size))
        throw std::runtime_error("TCP local endpoint query failed");
    return endpoint(address);
}
void validate_timeout(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0 || timeout.count() > 1000)
        throw std::invalid_argument("TCP timeout must be 0-1000 ms");
}
bool wait(Socket socket, bool writing, std::chrono::steady_clock::time_point deadline) {
    const auto left = std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(
                                               deadline - std::chrono::steady_clock::now())
                                               .count());
#ifdef _WIN32
    fd_set descriptors, errors;
    FD_ZERO(&descriptors);
    FD_SET(socket, &descriptors);
    FD_ZERO(&errors);
    FD_SET(socket, &errors);
    timeval timeout{static_cast<long>(left / 1'000'000), static_cast<long>(left % 1'000'000)};
    const int nfds = 0;
    const auto result = select(nfds, writing ? nullptr : &descriptors,
                               writing ? &descriptors : nullptr, &errors, &timeout);
    return result > 0 && FD_ISSET(socket, &descriptors) && !FD_ISSET(socket, &errors);
#else
    pollfd descriptor{socket, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
    const auto result = poll(&descriptor, 1, static_cast<int>((left + 999) / 1000));
    return result > 0 && (descriptor.revents & (writing ? POLLOUT : POLLIN)) &&
           !(descriptor.revents & (POLLERR | POLLNVAL));
#endif
}
void configure(Socket socket) {
    nonblocking(socket);
    int one = 1, bytes = 32 * 1024;
    if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one),
                   sizeof(one)) ||
        setsockopt(socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&bytes),
                   sizeof(bytes)))
        throw std::runtime_error("TCP low-latency options failed");
}
} // namespace
TcpStream::TcpStream(uintptr_t socket, Endpoint peer) : socket_(socket), peer_(peer) {
    payload_.reserve(wire::max_control);
}
TcpStream::~TcpStream() {
    interrupt();
    close_socket(static_cast<Socket>(socket_));
}
std::unique_ptr<TcpStream> TcpStream::connect(Endpoint peer, std::chrono::milliseconds timeout,
                                              bool allow_public) {
    validate_timeout(timeout);
    if (!peer.port || (!allow_public && !peer.private_or_local()))
        throw std::invalid_argument("TCP requires private RFC1918, link-local or loopback IPv4; public/CGNAT addresses are not supported");
    initialize();
    Guard socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (socket.socket == invalid)
        throw std::runtime_error("TCP socket creation failed");
    configure(socket.socket);
    const auto address = native(peer);
    if (::connect(socket.socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address))) {
        if (!would_block() ||
            !wait(socket.socket, true, std::chrono::steady_clock::now() + timeout))
            throw std::runtime_error("TCP connection failed or timed out");
        int error{};
        Length size = sizeof(error);
        if (getsockopt(socket.socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error),
                       &size) ||
            error)
            throw std::runtime_error("TCP connection refused");
    }
    auto stream =
        std::unique_ptr<TcpStream>(new TcpStream(static_cast<uintptr_t>(socket.socket), peer));
    socket.socket = invalid;
    return stream;
}
void TcpStream::interrupt() noexcept {
    if (alive_.exchange(false)) {
#ifdef _WIN32
        shutdown(static_cast<Socket>(socket_), SD_BOTH);
#else
        shutdown(static_cast<Socket>(socket_), SHUT_RDWR);
#endif
    }
}
Endpoint TcpStream::local() const {
    return local_endpoint(static_cast<Socket>(socket_));
}
bool TcpStream::send_record(std::span<const uint8_t> payload, std::chrono::milliseconds timeout) {
    return write_record(payload, timeout, timeout, false);
}
bool TcpStream::send_droppable_record(std::span<const uint8_t> payload,
                                      std::chrono::milliseconds start,
                                      std::chrono::milliseconds finish) {
    return write_record(payload, start, finish, true);
}
void TcpStream::send_buffer(int bytes) noexcept {
    (void)setsockopt(static_cast<Socket>(socket_), SOL_SOCKET, SO_SNDBUF,
                     reinterpret_cast<const char *>(&bytes), sizeof(bytes));
}
bool TcpStream::write_record(std::span<const uint8_t> payload, std::chrono::milliseconds start,
                             std::chrono::milliseconds finish, bool droppable) {
    validate_timeout(start);
    validate_timeout(finish);
    if (!alive() || payload.empty() || payload.size() > wire::max_control)
        return false;
    wire::Writer prefix(transmit_);
    prefix.u32(static_cast<uint32_t>(payload.size()));
    std::copy(payload.begin(), payload.end(), transmit_.begin() + 4);
    const auto record = std::span(transmit_).first(payload.size() + 4);
    auto deadline = std::chrono::steady_clock::now() + start;
    size_t written{};
    const auto socket = static_cast<Socket>(socket_);
    while (alive() && written < record.size()) {
        const auto n = ::send(socket, reinterpret_cast<const char *>(record.data() + written),
                              static_cast<int>(record.size() - written), send_flags);
        if (n > 0) {
            if (!written)
                deadline = std::chrono::steady_clock::now() + finish;
            written += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && would_block()) {
            if (wait(socket, true, deadline))
                continue;
            if (droppable && !written)
                return false; // Nothing reached the stream, so framing is intact.
        }
        interrupt();
        return false;
    }
    return written == record.size();
}
std::optional<wire::Bytes> TcpStream::receive_record(std::chrono::milliseconds timeout) {
    const auto view = receive_record_view(timeout);
    if (!view)
        return {};
    return wire::Bytes(view->begin(), view->end());
}
std::optional<std::span<const uint8_t>>
TcpStream::receive_record_view(std::chrono::milliseconds timeout) {
    validate_timeout(timeout);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto socket = static_cast<Socket>(socket_);
    while (alive()) {
        const bool prefix = prefix_used_ < prefix_.size();
        uint8_t *data = prefix ? prefix_.data() + prefix_used_ : payload_.data() + payload_used_;
        const size_t left =
            prefix ? prefix_.size() - prefix_used_ : payload_.size() - payload_used_;
        const auto n = ::recv(socket, reinterpret_cast<char *>(data), static_cast<int>(left), 0);
        if (n > 0) {
            if (prefix) {
                prefix_used_ += static_cast<size_t>(n);
                if (prefix_used_ == prefix_.size()) {
                    wire::Reader reader(prefix_);
                    uint32_t size{};
                    reader.u32(size);
                    if (!size || size > wire::max_control) {
                        interrupt();
                        return {};
                    }
                    payload_.resize(size);
                }
            } else {
                payload_used_ += static_cast<size_t>(n);
                if (payload_used_ == payload_.size()) {
                    prefix_used_ = payload_used_ = 0;
                    return std::span<const uint8_t>(payload_);
                }
            }
            continue;
        }
        if (n < 0 && would_block()) {
            if (wait(socket, false, deadline))
                continue;
            return {};
        }
        interrupt();
    }
    return {};
}
TcpListener::TcpListener(Endpoint bind, bool allow_public)
    : socket_(~uintptr_t{}), public_(allow_public) {
    if (!allow_public && !bind.private_or_local())
        throw std::invalid_argument("TCP bind must be a specific private/local address");
    initialize();
    Guard socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (socket.socket == invalid)
        throw std::runtime_error("TCP listener creation failed");
    nonblocking(socket.socket);
#ifdef _WIN32
    int exclusive = 1;
    if (setsockopt(socket.socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)))
        throw std::runtime_error("TCP exclusive bind setup failed");
#endif
    const auto address = native(bind);
    if (::bind(socket.socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) ||
        ::listen(socket.socket, 4))
        throw std::runtime_error("TCP listener bind failed");
    socket_ = static_cast<uintptr_t>(socket.socket);
    socket.socket = invalid;
}
TcpListener::~TcpListener() {
    close_socket(static_cast<Socket>(socket_));
}
Endpoint TcpListener::local() const {
    return local_endpoint(static_cast<Socket>(socket_));
}
std::unique_ptr<TcpStream> TcpListener::accept(std::chrono::milliseconds timeout) {
    validate_timeout(timeout);
    const auto socket = static_cast<Socket>(socket_);
    if (!wait(socket, false, std::chrono::steady_clock::now() + timeout))
        return {};
    sockaddr_in address{};
    Length length = sizeof(address);
    Guard accepted{::accept(socket, reinterpret_cast<sockaddr *>(&address), &length)};
    if (accepted.socket == invalid)
        return {};
    const auto peer = endpoint(address);
    if (!public_ && !peer.private_or_local())
        return {};
    configure(accepted.socket);
    auto stream =
        std::unique_ptr<TcpStream>(new TcpStream(static_cast<uintptr_t>(accepted.socket), peer));
    accepted.socket = invalid;
    return stream;
}
TcpTransport::TcpTransport(std::unique_ptr<TcpStream> stream) : stream_(std::move(stream)) {
    if (!stream_)
        throw std::invalid_argument("TCP transport needs a connected stream");
    // Room for about one frame, so a keyframe or short USB stall does not fill the
    // socket. Kept small because anything queued here is latency.
    stream_->send_buffer(256 * 1024);
}
bool TcpTransport::send(std::span<const uint8_t> message, const Endpoint &peer) {
    return peer == stream_->peer() && message.size() <= wire::max_datagram &&
           stream_->send_droppable_record(message, std::chrono::milliseconds(10),
                                          std::chrono::milliseconds(100));
}
std::optional<size_t> TcpTransport::receive(std::span<uint8_t> buffer, Endpoint &sender,
                                            std::chrono::milliseconds timeout) {
    auto message = stream_->receive_record_view(timeout);
    if (!message)
        return {};
    if (message->size() > wire::max_datagram || message->size() > buffer.size()) {
        stream_->interrupt();
        return {};
    }
    std::copy(message->begin(), message->end(), buffer.begin());
    sender = stream_->peer();
    return message->size();
}
} // namespace pocketvr
