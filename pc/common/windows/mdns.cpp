// SPDX-License-Identifier: Apache-2.0
#include "mdns.hpp"
#include "identity_store.hpp"
#include <winsock2.h>
#include <windns.h>
#include <spdlog/spdlog.h>
#include <atomic>
#include <bit>

namespace pocketvr::win {
namespace {
// Process-scoped OS module: asynchronous DNS callbacks can outlive an advertiser.
HMODULE dns_module() {
    static const auto module = LoadLibraryExW(L"dnsapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        throw std::runtime_error("Windows DNS service API unavailable");
    return module;
}
template <class F> F function(const char *name) {
    const auto address = GetProcAddress(dns_module(), name);
    if (!address)
        throw std::runtime_error("Windows DNS service API unavailable");
    return std::bit_cast<F>(address);
}
} // namespace
class MdnsAdvertiser::State : public std::enable_shared_from_this<State> {
    using Register = decltype(&DnsServiceRegister);
    using Free = decltype(&DnsServiceFreeInstance);
    struct Callback {
        std::shared_ptr<State> state;
        bool removing{};
    };
    Register register_{function<Register>("DnsServiceRegister")};
    Register remove_{function<Register>("DnsServiceDeRegister")};
    decltype(&DnsServiceRegisterCancel) cancel_{
        function<decltype(cancel_)>("DnsServiceRegisterCancel")};
    Free free_{function<Free>("DnsServiceFreeInstance")};
    PDNS_SERVICE_INSTANCE original_{}, registered_instance_{};
    DNS_SERVICE_REGISTER_REQUEST request_{}, remove_request_{};
    DNS_SERVICE_CANCEL cancellation_{};
    std::atomic<bool> completed_{}, stopping_{}, removing_{};
    static void WINAPI complete(DWORD status, void *opaque,
                                PDNS_SERVICE_INSTANCE instance) noexcept {
        std::unique_ptr<Callback> callback(static_cast<Callback *>(opaque));
        const auto state = callback->state;
        try {
            if (callback->removing) {
                if (instance && instance != state->original_ &&
                    instance != state->registered_instance_)
                    state->free_(instance);
                state->registered.store(false);
            } else {
                state->registered_instance_ = instance;
                state->registered.store(status == ERROR_SUCCESS);
                state->completed_.store(true);
                if (status != ERROR_SUCCESS)
                    spdlog::debug("mDNS registration ended with Windows status {}", status);
                if (state->stopping_.load() && state->registered.load())
                    state->remove();
            }
        } catch (...) { /* Never unwind into an OS callback. */
        }
    }
    void remove() {
        if (removing_.exchange(true))
            return;
        auto callback = std::make_unique<Callback>(Callback{shared_from_this(), true});
        remove_request_ = request_;
        remove_request_.pServiceInstance = registered_instance_ ? registered_instance_ : original_;
        remove_request_.pQueryContext = callback.get();
        const auto status = remove_(&remove_request_, nullptr);
        if (status == DNS_REQUEST_PENDING)
            (void)callback.release();
        else
            registered.store(false);
    }

  public:
    std::atomic<bool> registered{};
    State(PrivateInterface nic, uint16_t control_port, uint16_t media_port, const Key &identity) {
        if (!nic.index || !nic.address.private_or_local() || !control_port || !media_port)
            throw std::invalid_argument(
                "mDNS advertisement requires a private interface and ports");
        std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1> computer{};
        DWORD count = static_cast<DWORD>(computer.size());
        if (!GetComputerNameW(computer.data(), &count))
            throw std::runtime_error("Computer name unavailable");
        std::wstring name(computer.data(), count);
        for (auto &c : name)
            if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                  (c >= L'0' && c <= L'9') || c == L'-'))
                c = L'_';
        const auto host = name + L".local";
        const auto service = L"PocketVR-" + name + L"._pocketvr._tcp.local";
        const auto identity_text = key_hex(identity);
        const std::wstring identity_wide(identity_text.begin(), identity_text.end());
        const auto port = std::to_wstring(media_port);
        PCWSTR keys[]{L"version", L"identity", L"media_port"};
        PCWSTR values[]{L"1", identity_wide.c_str(), port.c_str()};
        auto address = htonl(nic.address.address);
        const auto construct =
            function<decltype(&DnsServiceConstructInstance)>("DnsServiceConstructInstance");
        original_ = construct(service.c_str(), host.c_str(), &address, nullptr, control_port, 0, 0,
                              3, keys, values);
        if (!original_)
            throw std::runtime_error("mDNS service construction failed");
        request_.Version = DNS_QUERY_REQUEST_VERSION1;
        request_.InterfaceIndex = nic.index;
        request_.pServiceInstance = original_;
        request_.pRegisterCompletionCallback = complete;
        request_.unicastEnabled = FALSE;
    }
    ~State() {
        if (registered_instance_ && registered_instance_ != original_)
            free_(registered_instance_);
        if (original_)
            free_(original_);
    }
    void start() {
        // DNS completion can run after SteamVR releases its driver reference.
        // Keep the containing module resident until vrserver exits, avoiding a
        // callback into unloaded code without blocking SteamVR cleanup.
        HMODULE containing{};
        const auto address = std::bit_cast<uintptr_t>(&complete);
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(address), &containing))
            throw std::runtime_error("Cannot protect DNS callback lifetime");
        auto callback = std::make_unique<Callback>(Callback{shared_from_this(), false});
        request_.pQueryContext = callback.get();
        const auto status = register_(&request_, &cancellation_);
        if (status == DNS_REQUEST_PENDING)
            (void)callback.release();
        else {
            completed_.store(true);
            spdlog::debug("mDNS registration unavailable: {}", status);
        }
    }
    void stop() noexcept {
        stopping_.store(true);
        try {
            if (registered.load())
                remove();
            else if (!completed_.load())
                (void)cancel_(&cancellation_);
        } catch (...) { /* Registration is also removed automatically at process exit. */
        }
    }
};
MdnsAdvertiser::MdnsAdvertiser(PrivateInterface nic, uint16_t control_port, uint16_t media_port,
                               const Key &identity)
    : state_(std::make_shared<State>(nic, control_port, media_port, identity)) {
    state_->start();
}
MdnsAdvertiser::~MdnsAdvertiser() {
    state_->stop();
}
bool MdnsAdvertiser::advertised() const noexcept {
    return state_->registered.load();
}
} // namespace pocketvr::win
