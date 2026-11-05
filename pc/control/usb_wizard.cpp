// SPDX-License-Identifier: Apache-2.0
#include "usb.hpp"
#include "windows.hpp"
#include <commdlg.h>
#include <dbt.h>
#include <shellapi.h>
#include <fstream>
#include <mutex>
#include <thread>
#include <iostream>
#include <algorithm>
#include <array>
#include <spdlog/spdlog.h>

using namespace pocketvr;
using namespace pocketvr::win;
namespace {
constexpr wchar_t maintenance_mutex[] = L"Local\\PocketVR.UsbReconnect.v1";
constexpr wchar_t maintenance_stop[] = L"Local\\PocketVR.UsbReconnect.Stop.v1";
std::wstring wide(const std::string &value) {
    if (value.empty()) return {};
    const auto n = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (n <= 0) throw std::runtime_error("Cannot display USB status");
    std::wstring result(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), n);
    return result;
}
std::string utf8(const std::wstring &value) {
    if (value.empty()) return {};
    const auto n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) throw std::runtime_error("Cannot store adb path");
    std::string result(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), n, nullptr, nullptr);
    return result;
}
std::wstring text(HWND control) {
    std::wstring value(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    const auto length = GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
    value.resize(static_cast<size_t>(length)); return value;
}
uint16_t port(const std::wstring &text) {
    if (text.empty() || text.size() > 5 || text.find_first_not_of(L"0123456789") != std::wstring::npos)
        throw std::invalid_argument("Enter a TCP port in 1..65535");
    const auto value = std::stoul(text);
    if (!value || value > 65535) throw std::invalid_argument("Enter a TCP port in 1..65535");
    return static_cast<uint16_t>(value);
}
struct Options { std::filesystem::path adb; uint16_t control{9943}, media{9944}; bool probe{}, maintain{}, once{}, stop_maintain{}; };
Options options() {
    int count{}; auto **args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) throw std::runtime_error("Cannot read USB helper options");
    struct Args { wchar_t **p; ~Args() { LocalFree(p); } } owned{args};
    Options result;
    for (int i = 1; i < count; ++i) {
        const std::wstring option = args[i];
        if (option == L"--probe") { result.probe = true; continue; }
        if (option == L"--maintain") { result.maintain = true; continue; }
        if (option == L"--once") { result.once = true; continue; }
        if (option == L"--stop-maintain") { result.stop_maintain = true; continue; }
        if (++i >= count) throw std::invalid_argument("USB helper option needs a value");
        if (option == L"--adb") result.adb = args[i];
        else if (option == L"--control-port") result.control = port(args[i]);
        else if (option == L"--media-port") result.media = port(args[i]);
        else throw std::invalid_argument("Unknown USB helper option");
    }
    if (result.control == result.media) throw std::invalid_argument("Control and media ports must differ");
    if ((result.probe && result.maintain) || (result.once && !result.maintain))
        throw std::invalid_argument("Use --once only with --maintain; --probe stays read-only");
    return result;
}
void stop_maintenance() {
    Handle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, maintenance_stop));
    if (!event) return;
    if (!SetEvent(event.get())) throw std::runtime_error("Cannot stop automatic USB setup");
    Handle worker(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, maintenance_mutex));
    if (!worker) return;
    const auto waited = WaitForSingleObject(worker.get(), 15000);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED)
        throw std::runtime_error("Automatic USB setup is still stopping; retry removal");
    ReleaseMutex(worker.get());
}
std::wstring guidance(const AdbDevice &phone) {
    if (!phone.cable_candidate()) return L"Wireless/emulator entry: connect the physical phone using a data cable.";
    if (phone.state == "unauthorized") return L"Unlock the phone and approve Allow USB debugging, then Refresh.";
    if (phone.state == "offline") return L"Phone is offline. Reconnect its data cable, unlock it, then Refresh.";
    if (phone.state == "no") return L"adb has no permission to access this phone. Check its USB driver and debugging authorization.";
    if (phone.state != "device") return L"Phone is not in normal authorized Android mode. Boot Android, then Refresh.";
    return L"Phone authorized. Set up USB, then select USB (adb reverse) in the phone app.";
}
int maintain(const Options &options) {
    Handle single;
    Handle stop_event;
    if (!options.once) {
        single.reset(CreateMutexW(nullptr, TRUE, maintenance_mutex));
        if (!single) return 1;
        if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
        stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, maintenance_stop));
        if (!stop_event) { ReleaseMutex(single.get()); return 1; }
    }
    struct Unlock { Handle &handle; ~Unlock() { if (handle) ReleaseMutex(handle.get()); } } unlock{single};
    initialize_logging("usb-autoconnect");
    UsbReconnect reconnect;
    std::string previous;
    unsigned unavailable{};
    for (;;) {
        if (stop_event && WaitForSingleObject(stop_event.get(), 0) == WAIT_OBJECT_0) return 0;
        bool ready = false;
        std::string message;
        try {
            const auto network = rpc({{"method", "network.status"}});
            if (!network.value("ok", false)) throw std::runtime_error("PocketVR driver unavailable");
            uint16_t control{}, media{};
            for (const auto &endpoint : network.at("endpoints")) {
                if (endpoint.value("ip", "") != "127.0.0.1") continue;
                control = endpoint.at("control_port").get<uint16_t>();
                media = endpoint.at("tcp_media_port").get<uint16_t>();
            }
            if (!control || !media || control == media)
                throw std::runtime_error("PocketVR USB listeners are not ready");
            unavailable = 0;
            const UsbSetup client(find_adb(options.adb));
            message = reconnect.tick(client, control, media);
            ready = message == "USB cable mappings ready";
        } catch (const std::exception &error) {
            message = error.what();
            if (message.find("driver") != std::string::npos ||
                message.find("listeners") != std::string::npos) ++unavailable;
        }
        if (message != previous) {
            spdlog::warn("{}", message);
            previous = message;
        }
        if (options.once) {
            const auto output = nlohmann::json{{"ok", ready}, {"state", message}}.dump() + "\n";
            DWORD written{};
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output.data(), static_cast<DWORD>(output.size()), &written, nullptr);
            return ready ? 0 : 1;
        }
        if (unavailable >= 10) return 0;
        if (WaitForSingleObject(stop_event.get(), 2000) == WAIT_OBJECT_0) return 0;
    }
}
int probe(const Options &options) {
    try {
        const auto adb = find_adb(options.adb);
        UsbSetup client(adb);
        auto devices = nlohmann::json::array();
        for (const auto &device : client.devices()) {
            nlohmann::json entry{{"serial", device.serial}, {"state", device.state},
                {"model", device.model}, {"cable_candidate", device.cable_candidate()}};
            if (device.authorized()) entry["reverse"] = client.mappings(device.serial);
            devices.push_back(std::move(entry));
        }
        const auto output = nlohmann::json{{"ok", true}, {"devices", devices}}.dump(2) + "\n";
        DWORD written{}; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output.data(), static_cast<DWORD>(output.size()), &written, nullptr);
        return 0;
    } catch (const std::exception &error) {
        const auto output = nlohmann::json{{"ok", false}, {"error", error.what()}}.dump() + "\n";
        DWORD written{}; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output.data(), static_cast<DWORD>(output.size()), &written, nullptr);
        return 1;
    }
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        const auto settings = options();
        if (settings.stop_maintain) { stop_maintenance(); return 0; }
        if (settings.probe) return probe(settings);
        return maintain(settings);
    } catch (const std::exception &) {
        return 1;
    }
}
