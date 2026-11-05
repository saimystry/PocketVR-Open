// SPDX-License-Identifier: Apache-2.0
#include "usb.hpp"
#include "windows.hpp"
#include <algorithm>
#include <set>
#include <sstream>

namespace pocketvr::win {
namespace {
bool serial_valid(std::string_view serial) {
    return !serial.empty() && serial.size() <= 256 && serial.front() != '-' &&
        std::all_of(serial.begin(), serial.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':';
        });
}
std::wstring wide_ascii(const std::string &text) { return {text.begin(), text.end()}; }
std::wstring environment(const wchar_t *name) {
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size || size > 32768) return {};
    std::wstring text(size, L'\0');
    const auto actual = GetEnvironmentVariableW(name, text.data(), size);
    if (!actual || actual >= size) return {};
    text.resize(actual); return text;
}
bool executable(const std::filesystem::path &path) {
    std::error_code error;
    return path.is_absolute() && std::filesystem::is_regular_file(path, error);
}
}
bool AdbDevice::cable_candidate() const {
    return serial_valid(serial) && !serial.starts_with("emulator-") &&
        serial.find(':') == std::string::npos && serial.find("._tcp") == std::string::npos;
}
std::vector<AdbDevice> parse_adb_devices(std::string_view text) {
    if (text.size() > 65536) throw std::invalid_argument("adb device list too large");
    std::istringstream input{std::string(text)}; std::string line;
    std::vector<AdbDevice> devices; std::set<std::string> seen;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '*' || line.starts_with("List of devices attached")) continue;
        std::istringstream fields(line); AdbDevice device;
        if (!(fields >> device.serial >> device.state)) continue;
        if (!serial_valid(device.serial) || !seen.insert(device.serial).second)
            throw std::invalid_argument("Invalid or duplicate adb device entry");
        std::string field;
        while (fields >> field)
            if (field.starts_with("model:")) device.model = field.substr(6);
        if (device.model.size() > 128) device.model.resize(128);
        devices.push_back(std::move(device));
        if (devices.size() > 64) throw std::invalid_argument("Too many adb devices");
    }
    return devices;
}
ReverseMappings parse_adb_reverse(std::string_view text) {
    if (text.size() > 65536) throw std::invalid_argument("adb reverse list too large");
    std::istringstream input{std::string(text)}; std::string line; ReverseMappings result;
    while (std::getline(input, line)) {
        if (line.empty() || line == "\r" || line[0] == '*') continue;
        std::istringstream fields(line); std::string transport, from, to, extra;
        if (!(fields >> transport >> from >> to) || (fields >> extra) ||
            !result.emplace(from, to).second)
            throw std::invalid_argument("Cannot safely parse adb reverse mappings; no rules changed");
    }
    return result;
}
std::filesystem::path find_adb(const std::filesystem::path &configured) {
    if (!configured.empty()) {
        if (_wcsicmp(configured.filename().c_str(), L"adb.exe") != 0 || !executable(configured))
            throw std::runtime_error("Selected adb.exe was not found; browse to platform-tools/adb.exe");
        return configured;
    }
    std::vector<std::filesystem::path> candidates;
    // The installer ships its own copy, so no separate Android tools are needed.
    std::array<wchar_t, 32768> module{};
    const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (length && length < module.size())
        candidates.push_back(std::filesystem::path(module.data()).parent_path().parent_path() / "adb" / "adb.exe");
    for (const auto *name : {L"ANDROID_SDK_ROOT", L"ANDROID_HOME"}) {
        const auto root = environment(name);
        if (!root.empty()) candidates.push_back(std::filesystem::path(root) / "platform-tools" / "adb.exe");
    }
    const auto local = environment(L"LOCALAPPDATA");
    if (!local.empty()) candidates.push_back(std::filesystem::path(local) / "Android" / "Sdk" / "platform-tools" / "adb.exe");
    std::wistringstream paths(environment(L"PATH")); std::wstring directory;
    while (std::getline(paths, directory, L';')) {
        if (directory.size() >= 2 && directory.front() == L'\"' && directory.back() == L'\"')
            directory = directory.substr(1, directory.size() - 2);
        if (!directory.empty() && std::filesystem::path(directory).is_absolute())
            candidates.push_back(std::filesystem::path(directory) / "adb.exe");
    }
    for (const auto &path : candidates) if (executable(path)) return path;
    throw std::runtime_error("adb.exe not found. Install Android platform-tools, then Browse to adb.exe or set ANDROID_HOME/PATH.");
}
std::string UsbReconnect::tick(const UsbSetup &client, uint16_t control, uint16_t media,
                                std::stop_token stop) {
    const auto devices = client.devices(stop);
    if (selected_.empty()) {
        for (const auto &device : devices) {
            if (!device.authorized()) continue;
            if (!selected_.empty()) {
                selected_.clear();
                return "Multiple USB phones: select one using USB setup";
            }
            selected_ = device.serial;
        }
    }
    const auto phone = std::find_if(devices.begin(), devices.end(), [&](const auto &device) {
        return device.serial == selected_ && device.authorized();
    });
    if (phone == devices.end()) return "Waiting for the selected USB phone and debugging authorization";
    const auto rules = client.mappings(selected_, stop);
    bool ready = true;
    for (const auto port : {control, media}) {
        const auto name = "tcp:" + std::to_string(port);
        const auto found = rules.find(name);
        if (found != rules.end() && found->second != name)
            throw std::runtime_error("Conflicting USB reverse mapping; use USB setup to resolve it");
        ready = ready && found != rules.end();
    }
    if (!ready) (void)client.configure(selected_, control, media, false, stop);
    return "USB cable mappings ready";
}
UsbSetup::UsbSetup(const std::filesystem::path &adb)
    : runner_([adb](const auto &args, auto stop, auto timeout) { return run_process(adb, args, stop, timeout); }) {}
ProcessResult UsbSetup::call(const std::vector<std::wstring> &args, std::stop_token stop,
                            std::chrono::milliseconds timeout) const {
    if (stop.stop_requested()) throw std::runtime_error("USB operation cancelled");
    auto result = runner_(args, stop, timeout);
    if (result.output.size() > 65536) throw std::runtime_error("adb output exceeds limit");
    if (result.exit_code) {
        if (result.output.size() > 1024) result.output.resize(1024);
        throw std::runtime_error("adb command failed: " + result.output);
    }
    return result;
}
std::vector<AdbDevice> UsbSetup::devices(std::stop_token stop) const {
    return parse_adb_devices(call({L"devices", L"-l"}, stop).output);
}
ReverseMappings UsbSetup::mappings(const std::string &serial, std::stop_token stop) const {
    if (!serial_valid(serial)) throw std::invalid_argument("Invalid adb serial");
    return parse_adb_reverse(call({L"-s", wide_ascii(serial), L"reverse", L"--list"}, stop).output);
}
std::string UsbSetup::configure(const std::string &serial, uint16_t control, uint16_t media,
                                bool remove, std::stop_token stop) const {
    if (!control || !media || control == media) throw std::invalid_argument("Use two different TCP ports in 1..65535");
    const auto phones = devices(stop);
    const auto phone = std::find_if(phones.begin(), phones.end(), [&](const auto &d) { return d.serial == serial; });
    if (phone == phones.end() || !phone->authorized())
        throw std::runtime_error("Selected phone is not connected and authorized over USB; unlock it, approve USB debugging, then Refresh");
    const auto initial = mappings(serial, stop);
    const std::array ports{"tcp:" + std::to_string(control), "tcp:" + std::to_string(media)};
    for (const auto &port : ports) {
        const auto it = initial.find(port);
        if (it != initial.end() && it->second != port)
            throw std::runtime_error("Conflicting reverse rule for " + port + "; no rules changed. Choose matching unused PC/phone ports.");
    }
    std::vector<std::string> created;
    const auto selected = wide_ascii(serial);
    try {
        for (const auto &port : ports) {
            if (stop.stop_requested()) throw std::runtime_error("USB operation cancelled");
            // Recheck immediately before a mutation; --no-rebind prevents a
            // concurrent creator from being overwritten during setup.
            const auto current = mappings(serial, stop);
            const auto it = current.find(port);
            if (it != current.end() && it->second != port)
                throw std::runtime_error("Reverse rule changed concurrently; preserving it");
            if (remove) {
                if (it != current.end()) call({L"-s", selected, L"reverse", L"--remove", wide_ascii(port)}, stop);
            } else if (it == current.end()) {
                call({L"-s", selected, L"reverse", L"--no-rebind", wide_ascii(port), wide_ascii(port)}, stop);
                created.push_back(port);
            }
        }
        const auto final = mappings(serial, stop);
        for (const auto &port : ports) {
            const auto it = final.find(port);
            if ((!remove && (it == final.end() || it->second != port)) || (remove && it != final.end()))
                throw std::runtime_error("USB rules changed or could not be verified; Refresh to inspect them");
        }
    } catch (const std::exception &error) {
        bool rollback_failed = false;
        for (const auto &port : created) {
            try {
                const auto current = parse_adb_reverse(call({L"-s", selected, L"reverse", L"--list"}, {}, std::chrono::milliseconds(1500)).output);
                const auto it = current.find(port);
                if (it != current.end() && it->second == port)
                    call({L"-s", selected, L"reverse", L"--remove", wide_ascii(port)}, {}, std::chrono::milliseconds(1500));
            } catch (...) { rollback_failed = true; }
        }
        throw std::runtime_error(std::string(error.what()) +
            (remove ? " Removal may be partial; Refresh to inspect the remaining rules." :
             rollback_failed ? " Cleanup could not finish; Refresh and inspect partial rules." :
             " Confirmed new rules were rolled back. If a command was interrupted, Refresh to verify its outcome."));
    }
    return remove ? "PocketVR rules removed. Other rules and pairing are retained." :
        "USB forwarding ready. Start SteamVR, enable USB (adb reverse) on the phone, and connect to 127.0.0.1:" +
        std::to_string(control) + ". Pair normally. Refresh after unplug/replug.";
}
} // namespace pocketvr::win
