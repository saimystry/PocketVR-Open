// SPDX-License-Identifier: Apache-2.0
#include "windows.hpp"
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <pocketvr/protocol.hpp>
#include <shlobj.h>
#include <sddl.h>
#include <wincrypt.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <fstream>
#include <mutex>
#include <algorithm>
#include <atomic>

namespace pocketvr::win {
std::string computer_name() {
    std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1> wide{};
    DWORD count = static_cast<DWORD>(wide.size());
    if (!GetComputerNameW(wide.data(), &count))
        return "PocketVR PC";
    const auto size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(count), nullptr,
                                          0, nullptr, nullptr);
    if (size <= 0)
        return "PocketVR PC";
    std::string name(static_cast<size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(count), name.data(), size,
                             nullptr, nullptr))
        return "PocketVR PC";
    return name;
}
std::vector<PrivateInterface> private_interfaces() {
    (void)Endpoint::parse("127.0.0.1", 0); // Initialize Winsock before enumerating addresses.
    std::vector<PrivateInterface> result;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> bytes(size);
    constexpr ULONG flags =
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    auto status = GetAdaptersAddresses(
        AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(bytes.data()), &size);
    if (status == ERROR_BUFFER_OVERFLOW && size <= 1024 * 1024) {
        bytes.resize(size);
        status = GetAdaptersAddresses(
            AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES *>(bytes.data()), &size);
    }
    if (status == ERROR_NO_DATA)
        return result;
    if (status != NO_ERROR)
        throw std::runtime_error("Private network interface enumeration failed");
    for (auto *adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(bytes.data()); adapter;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp)
            continue;
        for (auto *address = adapter->FirstUnicastAddress; address; address = address->Next) {
            if (!address->Address.lpSockaddr || address->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(address->Address.lpSockaddr);
            Endpoint endpoint{ntohl(ipv4->sin_addr.s_addr), 0};
            if (endpoint.private_or_local() &&
                std::none_of(result.begin(), result.end(),
                             [&](const auto &value) { return value.address == endpoint; })) {
                result.push_back({endpoint, adapter->IfIndex});
                if (result.size() >= 16)
                    return result;
            }
        }
    }
    return result;
}
std::vector<Endpoint> private_addresses() {
    std::vector<Endpoint> result{Endpoint::parse("127.0.0.1", 0)};
    for (const auto &nic : private_interfaces())
        if (std::find(result.begin(), result.end(), nic.address) == result.end())
            result.push_back(nic.address);
    return result;
}
std::filesystem::path appdata(bool local) {
    PWSTR raw = nullptr;
    const auto result = SHGetKnownFolderPath(
        local ? FOLDERID_LocalAppData : FOLDERID_RoamingAppData, 0, nullptr, &raw);
    if (FAILED(result))
        throw std::runtime_error("Cannot locate application data folder");
    std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    path /= "PocketVR";
    std::filesystem::create_directories(path);
    return path;
}
void initialize_logging(const std::string &component) {
    const auto path = appdata(true) / "logs";
    std::filesystem::create_directories(path);
    auto logger = spdlog::rotating_logger_mt(
        "pocketvr-" + component, (path / (component + ".log")).string(), 2 * 1024 * 1024, 3);
    logger->set_pattern("%Y-%m-%dT%H:%M:%S.%e [%l] [%t] %v");
    spdlog::set_default_logger(logger);
    spdlog::flush_on(spdlog::level::warn);
}
std::vector<uint8_t> protect(std::span<const uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > 65536)
        throw std::invalid_argument("Protected data size");
    DATA_BLOB input{static_cast<DWORD>(bytes.size()), const_cast<BYTE *>(bytes.data())}, output{};
    if (!CryptProtectData(&input, L"PocketVR paired device", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("DPAPI encryption failed");
    std::vector<uint8_t> result(output.pbData, output.pbData + output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}
std::vector<uint8_t> unprotect(std::span<const uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > 65536)
        throw std::invalid_argument("Protected data size");
    DATA_BLOB input{static_cast<DWORD>(bytes.size()), const_cast<BYTE *>(bytes.data())}, output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &output))
        throw std::runtime_error("DPAPI decryption failed");
    std::vector<uint8_t> result(output.pbData, output.pbData + output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}
void atomic_write(const std::filesystem::path &file, std::span<const uint8_t> bytes) {
    std::filesystem::create_directories(file.parent_path());
    auto temporary = file;
    static std::atomic<uint64_t> next_file{};
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                 std::to_wstring(++next_file) + L".tmp";
    Handle handle(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle)
        throw std::runtime_error("Cannot write settings temporary file");
    DWORD written{};
    if (bytes.size() > 64 * 1024 * 1024 ||
        !WriteFile(handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                   nullptr) ||
        written != bytes.size() || !FlushFileBuffers(handle.get()))
        throw std::runtime_error("Cannot persist settings");
    handle.reset();
    if (!MoveFileExW(temporary.c_str(), file.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace settings file");
}
static bool transfer(HANDLE pipe, HANDLE stop, void *buffer, DWORD bytes, bool write) {
    DWORD offset = 0;
    while (offset < bytes) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event)
            return false;
        OVERLAPPED io{};
        io.hEvent = event.get();
        DWORD done{};
        const auto pointer = static_cast<uint8_t *>(buffer) + offset;
        const BOOL result = write ? WriteFile(pipe, pointer, bytes - offset, &done, &io)
                                  : ReadFile(pipe, pointer, bytes - offset, &done, &io);
        if (!result) {
            if (GetLastError() != ERROR_IO_PENDING)
                return false;
            HANDLE events[]{stop, event.get()};
            const auto waited = WaitForMultipleObjects(2, events, FALSE, 8000);
            if (waited != WAIT_OBJECT_0 + 1) {
                CancelIoEx(pipe, &io);
                GetOverlappedResult(pipe, &io, &done, TRUE);
                return false;
            }
            if (!GetOverlappedResult(pipe, &io, &done, FALSE))
                return false;
        }
        if (!done)
            return false;
        offset += done;
    }
    return true;
}
static PSECURITY_DESCRIPTOR owner_security() {
    HANDLE token_raw{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_raw))
        throw std::runtime_error("Cannot query current user");
    Handle token(token_raw);
    DWORD length{};
    if (GetTokenInformation(token.get(), TokenUser, nullptr, 0, &length) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || !length)
        throw std::runtime_error("Cannot size user SID");
    std::vector<uint8_t> data(length);
    if (!GetTokenInformation(token.get(), TokenUser, data.data(), length, &length))
        throw std::runtime_error("Cannot query user SID");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid, &sid))
        throw std::runtime_error("Cannot format user SID");
    std::wstring sddl = L"D:P(A;;GA;;;";
    sddl += sid;
    sddl += L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr))
        throw std::runtime_error("Cannot create pipe ACL");
    return descriptor;
}
RpcServer::RpcServer(Handler handler, std::wstring name)
    : handler_(std::move(handler)), name_(std::move(name)),
      stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    if (!stop_)
        throw std::runtime_error("RPC stop event failed");
    thread_ = std::jthread([this](std::stop_token token) { run(token); });
}
RpcServer::~RpcServer() {
    SetEvent(stop_.get());
    thread_.request_stop();
    if (thread_.joinable())
        thread_.join();
}
void RpcServer::run(std::stop_token token) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    try {
        descriptor = owner_security();
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        Handle pipe(CreateNamedPipeW(
            name_.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
            wire::max_control + 4, wire::max_control + 4, 0, &security));
        if (!pipe)
            throw std::runtime_error("Cannot create owner-restricted RPC pipe");
        while (!token.stop_requested()) {
            Handle connected(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            OVERLAPPED io{};
            io.hEvent = connected.get();
            const auto connect = ConnectNamedPipe(pipe.get(), &io);
            const auto error = connect ? ERROR_SUCCESS : GetLastError();
            if (error == ERROR_IO_PENDING) {
                HANDLE events[]{stop_.get(), connected.get()};
                if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
                    CancelIoEx(pipe.get(), &io);
                    DWORD ignored{};
                    GetOverlappedResult(pipe.get(), &io, &ignored, TRUE);
                    break;
                }
            } else if (error != ERROR_PIPE_CONNECTED && error != ERROR_SUCCESS)
                continue;
            while (!token.stop_requested()) {
                std::array<uint8_t, 4> prefix{};
                if (!transfer(pipe.get(), stop_.get(), prefix.data(), 4, false))
                    break;
                wire::Reader reader(prefix);
                uint32_t size{};
                reader.u32(size);
                if (!size || size > wire::max_control)
                    break;
                wire::Bytes bytes(size);
                if (!transfer(pipe.get(), stop_.get(), bytes.data(), size, false))
                    break;
                nlohmann::json response;
                try {
                    const auto request = nlohmann::json::parse(bytes.begin(), bytes.end(),
                        [](int depth, nlohmann::json::parse_event_t, nlohmann::json &) {
                            if (depth > 32)
                                throw std::invalid_argument("RPC JSON nesting exceeds 32 levels");
                            return true;
                        });
                    if (!request.is_object())
                        throw std::invalid_argument("Request must be an object");
                    response = handler_(request);
                } catch (const std::exception &e) {
                    response = {{"ok", false}, {"error", e.what()}};
                }
                const auto text = response.dump();
                const auto output = wire::ControlFramer::frame(
                    std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
                if (!transfer(pipe.get(), stop_.get(), const_cast<uint8_t *>(output.data()),
                              static_cast<DWORD>(output.size()), true))
                    break;
            }
            DisconnectNamedPipe(pipe.get());
        }
    } catch (const std::exception &e) {
        try {
            spdlog::error("Local RPC server stopped: {}", e.what());
        } catch (...) {
        }
    }
    if (descriptor)
        LocalFree(descriptor);
}
nlohmann::json rpc(const nlohmann::json &request, const std::wstring &name, std::stop_token cancellation) {
    const auto text = request.dump();
    auto output = wire::ControlFramer::frame(
        std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
    Handle pipe;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    do {
        if (cancellation.stop_requested()) throw std::runtime_error("RPC cancelled");
        pipe = Handle(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (pipe)
            break;
        const auto error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND)
            throw std::runtime_error("Cannot connect to PocketVR driver");
        if (error == ERROR_PIPE_BUSY)
            WaitNamedPipeW(name.c_str(), 20);
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    if (!pipe)
        throw std::runtime_error("PocketVR driver is not running or is busy");
    Handle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop) throw std::runtime_error("RPC cancellation event failed");
    std::stop_callback cancelled(cancellation, [&] { SetEvent(stop.get()); });
    if (cancellation.stop_requested()) throw std::runtime_error("RPC cancelled");
    if (!transfer(pipe.get(), stop.get(), output.data(), static_cast<DWORD>(output.size()), true))
        throw std::runtime_error("RPC send failed");
    std::array<uint8_t, 4> prefix{};
    if (!transfer(pipe.get(), stop.get(), prefix.data(), 4, false))
        throw std::runtime_error("RPC response timed out");
    wire::Reader reader(prefix);
    uint32_t size{};
    reader.u32(size);
    if (!size || size > wire::max_control)
        throw std::runtime_error("RPC response length");
    wire::Bytes bytes(size);
    if (!transfer(pipe.get(), stop.get(), bytes.data(), size, false))
        throw std::runtime_error("RPC response truncated");
    return nlohmann::json::parse(bytes);
}
} // namespace pocketvr::win
