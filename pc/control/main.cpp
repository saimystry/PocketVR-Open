// SPDX-License-Identifier: Apache-2.0
// PocketVR desktop app: a small native window hosting the interface in pc/control/ui.
// The page holds no state of its own; this host polls the driver and runs commands.
#include "windows.hpp"
#include "diagnostics.hpp"
#include "process.hpp"
#include "setup_qr.hpp"
#include "usb.hpp"
#include <pocketvr/settings.hpp>
#include <objbase.h>
#include <WebView2.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wrl.h>
#include <array>
#include <atomic>
#include <deque>
#include <fstream>
#include <mutex>
#include <regex>
#include <string_view>
#include <thread>

using Json = nlohmann::json;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
void show_connection_qr(HWND parent, const Json &network, bool smoke = false);
namespace {
constexpr UINT state_message = WM_APP + 1;
constexpr UINT toast_message = WM_APP + 2;
constexpr std::wstring_view origin = L"https://pocketvr.local/";
std::wstring wide(const std::string &text) {
    if (text.empty())
        return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0)
        return L"";
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
std::string utf8(std::wstring_view text) {
    if (text.empty())
        return {};
    const int size = static_cast<int>(text.size());
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), size, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), size, result.data(), count, nullptr, nullptr);
    return result;
}
std::filesystem::path app_folder() {
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size())
        throw std::runtime_error("Cannot locate PocketVR");
    return std::filesystem::path(path.data()).parent_path();
}
void launch(const std::filesystem::path &target, const wchar_t *arguments = nullptr, int show = SW_SHOWNORMAL) {
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), arguments, nullptr, show)) <= 32)
        throw std::runtime_error("Could not open " + target.filename().string());
}
// SteamVR and the driver need a DirectX 11 graphics card. Returns the card's name
// when this PC has none, so the app can say so rather than "SteamVR is not running".
std::string unsupported_graphics() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return {};
    std::string name;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            continue;
        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2,
                                        D3D11_SDK_VERSION, nullptr, nullptr, nullptr)))
            return {};
        if (name.empty())
            name = utf8(description.Description);
    }
    return name;
}
bool steamvr_running() {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    for (auto more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
        found = _wcsicmp(entry.szExeFile, L"vrserver.exe") == 0;
    CloseHandle(snapshot);
    return found;
}
// SteamVR has no single install marker before its first run, so check both the
// runtime path it writes and each Steam library.
bool steamvr_installed() {
    wchar_t local[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
        std::ifstream paths(std::filesystem::path(local) / "openvr" / "openvrpaths.vrpath");
        if (paths) {
            const auto values = Json::parse(paths, nullptr, false);
            if (values.is_object() && values.contains("runtime") && values["runtime"].is_array())
                for (const auto &runtime : values["runtime"])
                    if (runtime.is_string() && std::filesystem::is_directory(wide(runtime.get<std::string>())))
                        return true;
        }
    }
    wchar_t steam[MAX_PATH]{};
    DWORD size = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam, &size))
        return false;
    const std::filesystem::path root(steam);
    if (std::filesystem::is_directory(root / "steamapps/common/SteamVR"))
        return true;
    std::ifstream libraries(root / "steamapps/libraryfolders.vdf");
    const std::regex entry("\"path\"\\s+\"([^\"]+)\"");
    for (std::string line; std::getline(libraries, line);) {
        std::smatch found;
        if (!std::regex_search(line, found, entry))
            continue;
        const auto folder = std::regex_replace(found[1].str(), std::regex("\\\\\\\\"), "\\");
        if (std::filesystem::is_directory(std::filesystem::path(wide(folder)) / "steamapps/common/SteamVR"))
            return true;
    }
    return false;
}
Json supported_settings() {
    auto keys = Json::array();
    for (const auto &[section, spec] : pocketvr::Settings::schema().at("properties").items())
        for (const auto &[field, unused] : spec.at("properties").items()) {
            (void)unused;
            if (pocketvr::Settings::runtime_supported(section + "." + field))
                keys.push_back(section + "." + field);
        }
    return keys;
}
Json connection_code(const Json &network) {
    for (const auto &item : network.at("endpoints")) {
        try {
            const auto endpoint = pocketvr::Endpoint::parse(item.at("ip"), item.at("control_port"));
            const auto qr = pocketvr::win::connection_qr(endpoint);
            auto rows = Json::array();
            for (int y = 0; y < qr.size; ++y) {
                std::string row(static_cast<size_t>(qr.size), '0');
                for (int x = 0; x < qr.size; ++x)
                    if (qr.dark[static_cast<size_t>(y * qr.size + x)])
                        row[static_cast<size_t>(x)] = '1';
                rows.push_back(std::move(row));
            }
            return {{"text", qr.text}, {"size", qr.size}, {"rows", std::move(rows)},
                    {"label", "This PC: " + endpoint.ip()}};
        } catch (const std::invalid_argument &) {} // Skip loopback and public interfaces.
    }
    return nullptr;
}
class App {
    HWND window_{};
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> view_;
    std::mutex mutex_;
    std::deque<Json> commands_;
    std::deque<std::string> toasts_;
    Json incoming_;
    Json adb_{{"ready", false}, {"message", "Looking for your phone."}};
    std::string serial_;
    uint64_t launched_at_{};
    std::atomic<bool> installing_{}, exporting_{}, restart_pending_{};
    std::jthread worker_, installer_, exporter_;

    void toast(std::string text) {
        {
            std::lock_guard lock(mutex_);
            toasts_.push_back(std::move(text));
        }
        PostMessageW(window_, toast_message, 0, 0);
    }
    void queue(Json command) {
        std::lock_guard lock(mutex_);
        if (commands_.size() < 8)
            commands_.push_back(std::move(command));
    }
    static Json rpc(const Json &request, std::stop_token stop = {}) {
        const auto result = pocketvr::win::rpc(request, pocketvr::win::pipe_name, stop);
        if (!result.value("ok", false))
            throw std::runtime_error(result.value("error", "PocketVR could not do that"));
        return result;
    }
    void check_phone(std::stop_token stop) {
        Json next{{"ready", false}};
        std::string serial;
        try {
            const pocketvr::win::UsbSetup usb(pocketvr::win::find_adb());
            const auto devices = usb.devices(stop);
            const auto ready = std::find_if(devices.begin(), devices.end(), [](const auto &d) { return d.authorized(); });
            if (ready != devices.end()) {
                serial = ready->serial;
                next["ready"] = true;
                next["message"] = "Ready: " + (ready->model.empty() ? std::string("phone connected") : ready->model);
            } else if (!devices.empty())
                next["message"] = "Tap Allow on your phone to let this PC connect.";
            else
                next["message"] = "No phone found. Plug it in and turn on USB debugging.";
        } catch (const std::exception &) {
            next["message"] = "USB tools are missing. Reinstall PocketVR.";
        }
        std::lock_guard lock(mutex_);
        adb_ = std::move(next);
        serial_ = std::move(serial);
    }
    void run(std::stop_token stop) {
        const bool installed_at_start = steamvr_installed();
        const auto supported = supported_settings();
        const auto graphics_problem = unsupported_graphics();
        unsigned tick = 0;
        while (!stop.stop_requested()) {
            Json next{{"type", "state"}, {"supported", supported}, {"graphics_problem", graphics_problem}};
            try {
                std::optional<Json> command;
                {
                    std::lock_guard lock(mutex_);
                    if (!commands_.empty()) {
                        command = std::move(commands_.front());
                        commands_.pop_front();
                    }
                }
                if (command)
                    try {
                        rpc(*command, stop);
                    } catch (const std::exception &error) {
                        toast(error.what());
                    }
                next["status"] = rpc({{"method", "status"}}, stop);
                next["network"] = rpc({{"method", "network.status"}, {"desktop", true}}, stop);
                next["settings"] = rpc({{"method", "settings.get"}}, stop);
                next["profiles"] = rpc({{"method", "profiles.list"}}, stop);
                next["qr"] = connection_code(next["network"]);
            } catch (const std::exception &) {
                for (const auto *key : {"status", "network", "settings", "qr", "profiles"})
                    next.erase(key);
                next["error"] = true;
                next["vr_running"] = steamvr_running();
                // Wait for the old SteamVR to finish closing, or Steam ignores the launch.
                if (!next["vr_running"].get<bool>() && restart_pending_.exchange(false)) {
                    try {
                        launch(L"steam://rungameid/250820");
                        std::lock_guard lock(mutex_);
                        launched_at_ = GetTickCount64();
                    } catch (const std::exception &error) {
                        toast(error.what());
                    }
                }
            }
            {
                std::lock_guard lock(mutex_);
                next["adb"] = adb_;
                next["launching"] = launched_at_ && GetTickCount64() - launched_at_ < 60'000;
                next["steamvr_installed"] = installed_at_start || (tick % 8 == 0 && steamvr_installed());
                next["installing"] = installing_.load();
                incoming_ = std::move(next);
            }
            PostMessageW(window_, state_message, 0, 0);
            // After the update above: adb can take seconds to answer the first time.
            if (tick++ % 4 == 0)
                check_phone(stop);
            for (unsigned i = 0; i < 8 && !stop.stop_requested(); ++i) {
                {
                    std::lock_guard lock(mutex_);
                    if (!commands_.empty())
                        break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
    void post(const Json &message) {
        if (view_)
            view_->PostWebMessageAsJson(wide(message.dump(-1, ' ', false, Json::error_handler_t::replace)).c_str());
    }
    void install_phone_app() {
        if (installing_.exchange(true))
            return;
        std::string serial;
        {
            std::lock_guard lock(mutex_);
            serial = serial_;
        }
        if (installer_.joinable())
            installer_.join();
        installer_ = std::jthread([this, serial](std::stop_token stop) {
            try {
                const auto apk = app_folder().parent_path() / "phone" / "PocketVR.apk";
                if (!std::filesystem::is_regular_file(apk))
                    throw std::runtime_error("The phone app is missing. Reinstall PocketVR.");
                const auto adb = pocketvr::win::find_adb();
                const std::wstring device = wide(serial);
                const std::array<std::wstring, 5> install{L"-s", device, L"install", L"-r", apk.wstring()};
                const auto result = pocketvr::win::run_process(adb, install, stop, std::chrono::minutes(3));
                if (result.output.find("Success") == std::string::npos) {
                    // Never uninstall automatically: that would erase the phone's pairing.
                    if (result.output.find("INSTALL_FAILED_UPDATE_INCOMPATIBLE") != std::string::npos)
                        throw std::runtime_error("An older PocketVR is on the phone. Uninstall it there, then try again.");
                    throw std::runtime_error("The phone did not accept the app. Check its screen, then try again.");
                }
                const std::array<std::wstring, 6> open{L"-s", device, L"shell", L"am", L"start", L"com.pocketvr/.MainActivity"};
                (void)pocketvr::win::run_process(adb, open, stop);
                toast("PocketVR is installed on your phone.");
            } catch (const std::exception &error) {
                toast(error.what());
            }
            installing_.store(false);
        });
    }
    void repair_firewall() {
        const auto helper = app_folder().parent_path() / "setup" / "setup.ps1";
        if (!std::filesystem::is_regular_file(helper))
            throw std::runtime_error("Firewall helper is missing. Reinstall PocketVR.");
        const auto package = helper.parent_path().parent_path();
        const std::wstring arguments = L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + helper.wstring() +
            L"\" -Action FirewallInstall -PackagePath \"" + package.wstring() + L"\" -ElevateFirewall";
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        launch(std::filesystem::path(system) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe", arguments.c_str(), SW_HIDE);
        toast("Approve the Windows prompt to repair the firewall.");
    }
    void save_diagnostics() {
        if (exporting_.load())
            return;
        Json snapshot;
        {
            // Freeze before the modal file picker pumps polling messages.
            std::lock_guard lock(mutex_);
            snapshot = incoming_;
        }
        std::array<wchar_t, 32768> filename{};
        const std::wstring initial = L"PocketVR-support.zip";
        std::copy(initial.begin(), initial.end(), filename.begin());
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"ZIP file\0*.zip\0\0";
        dialog.lpstrFile = filename.data();
        dialog.nMaxFile = static_cast<DWORD>(filename.size());
        dialog.lpstrDefExt = L"zip";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&dialog))
            return;
        const std::filesystem::path destination(filename.data());
        exporting_.store(true);
        if (exporter_.joinable())
            exporter_.join();
        exporter_ = std::jthread([this, destination, snapshot](std::stop_token stop) {
            try {
                pocketvr::win::export_diagnostics(destination, snapshot, pocketvr::win::appdata(true) / "logs", stop);
                toast("Support file saved.");
            } catch (const std::exception &error) {
                toast(std::string("Could not save the file: ") + error.what());
            }
            exporting_.store(false);
        });
    }
    void command(const Json &message) {
        const auto action = message.value("action", std::string());
        const auto folder = app_folder();
        if (action == "ready") {
            PostMessageW(window_, state_message, 0, 0);
        } else if (action == "rpc") {
            queue(message.at("request"));
        } else if (action == "steamvr.start") {
            launch(L"steam://rungameid/250820");
            std::lock_guard lock(mutex_);
            launched_at_ = GetTickCount64();
        } else if (action == "steamvr.store") {
            launch(L"steam://install/250820");
        } else if (action == "steamvr.restart") {
            bool stopped;
            {
                std::lock_guard lock(mutex_);
                stopped = incoming_.contains("error");
            }
            if (stopped) {
                launch(L"steam://rungameid/250820");
                std::lock_guard lock(mutex_);
                launched_at_ = GetTickCount64();
            } else if (!restart_pending_.exchange(true))
                queue({{"method", "steamvr.restart"}, {"direct", true}});
        } else if (action == "open.input") {
            launch(folder / L"pocketvr_input.exe");
        } else if (action == "open.logs") {
            const auto directory = pocketvr::win::appdata(true) / "logs";
            std::filesystem::create_directories(directory);
            launch(directory);
        } else if (action == "diagnostics.save") {
            save_diagnostics();
        } else if (action == "firewall.repair") {
            repair_firewall();
        } else if (action == "phone.install") {
            install_phone_app();
        }
    }
    void browser_ready(ICoreWebView2Controller *controller) {
        controller_ = controller;
        controller_->get_CoreWebView2(&view_);
        ComPtr<ICoreWebView2Controller2> themed;
        if (SUCCEEDED(controller_.As(&themed)))
            themed->put_DefaultBackgroundColor({255, 11, 15, 23});
        ComPtr<ICoreWebView2Settings> settings;
        view_->get_Settings(&settings);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        EventRegistrationToken token{};
        // Only the packaged page may run here, and only it may send commands.
        view_->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                LPWSTR uri{};
                args->get_Uri(&uri);
                if (!std::wstring_view(uri).starts_with(origin))
                    args->put_Cancel(TRUE);
                CoTaskMemFree(uri);
                return S_OK;
            }).Get(), &token);
        view_->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                args->put_Handled(TRUE);
                return S_OK;
            }).Get(), &token);
        view_->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                LPWSTR source{}, body{};
                args->get_Source(&source);
                const bool trusted = source && std::wstring_view(source).starts_with(origin);
                CoTaskMemFree(source);
                if (!trusted || FAILED(args->get_WebMessageAsJson(&body)))
                    return S_OK;
                const auto text = utf8(body);
                CoTaskMemFree(body);
                try {
                    command(Json::parse(text));
                } catch (const std::exception &error) {
                    post({{"type", "toast"}, {"text", error.what()}});
                }
                return S_OK;
            }).Get(), &token);
        ComPtr<ICoreWebView2_3> mapped;
        if (FAILED(view_.As(&mapped)))
            throw std::runtime_error("Please update Microsoft Edge WebView2");
        const auto ui = app_folder() / L"ui";
        mapped->SetVirtualHostNameToFolderMapping(L"pocketvr.local", ui.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY);
        resize();
        view_->Navigate((std::wstring(origin) + L"index.html").c_str());
        worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    void browser_missing() {
        if (MessageBoxW(window_, L"PocketVR needs the free Microsoft Edge WebView2 Runtime.\n\nOpen the download page now?",
                        L"PocketVR", MB_YESNO | MB_ICONINFORMATION) == IDYES)
            ShellExecuteW(nullptr, L"open", L"https://go.microsoft.com/fwlink/p/?LinkId=2124703", nullptr, nullptr, SW_SHOWNORMAL);
        DestroyWindow(window_);
    }
    void resize() {
        RECT bounds{};
        GetClientRect(window_, &bounds);
        if (controller_)
            controller_->put_Bounds(bounds);
    }

  public:
    void create(HWND window) {
        window_ = window;
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        const auto data = pocketvr::win::appdata(true) / "webview";
        std::filesystem::create_directories(data);
        const auto result = CreateCoreWebView2EnvironmentWithOptions(nullptr, data.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT status, ICoreWebView2Environment *environment) -> HRESULT {
                    if (FAILED(status) || !environment) {
                        browser_missing();
                        return S_OK;
                    }
                    return environment->CreateCoreWebView2Controller(window_,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT created, ICoreWebView2Controller *controller) -> HRESULT {
                                if (FAILED(created) || !controller) {
                                    browser_missing();
                                    return S_OK;
                                }
                                try {
                                    browser_ready(controller);
                                } catch (const std::exception &error) {
                                    MessageBoxW(window_, wide(error.what()).c_str(), L"PocketVR", MB_OK | MB_ICONERROR);
                                }
                                return S_OK;
                            }).Get());
                }).Get());
        if (FAILED(result))
            browser_missing();
    }
    LRESULT message(UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == state_message) {
            Json state;
            {
                std::lock_guard lock(mutex_);
                state = incoming_;
            }
            if (state.is_object())
                post(state);
            return 0;
        }
        if (message == toast_message) {
            std::deque<std::string> pending;
            {
                std::lock_guard lock(mutex_);
                pending.swap(toasts_);
            }
            for (const auto &text : pending)
                post({{"type", "toast"}, {"text", text}});
            return 0;
        }
        if (message == WM_SIZE) {
            resize();
            return 0;
        }
        if (message == WM_GETMINMAXINFO) {
            const auto scale = GetDpiForWindow(window_) / 96.0;
            auto *limits = reinterpret_cast<MINMAXINFO *>(lparam);
            limits->ptMinTrackSize = {static_cast<LONG>(760 * scale), static_cast<LONG>(520 * scale)};
            return 0;
        }
        if (message == WM_DESTROY) {
            for (auto *thread : {&worker_, &installer_, &exporter_}) {
                thread->request_stop();
                if (thread->joinable())
                    thread->join();
            }
            if (controller_)
                controller_->Close();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }
};
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto *app = reinterpret_cast<App *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    try {
        if (message == WM_CREATE) {
            app = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->create(window);
            return 0;
        }
        if (app)
            return app->message(message, wparam, lparam);
    } catch (const std::exception &error) {
        MessageBoxW(window, wide(error.what()).c_str(), L"PocketVR", MB_OK | MB_ICONERROR);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int show) {
    if (std::wstring_view(arguments) == L"--qr-smoke-test") {
        try {
            show_connection_qr(nullptr, {{"endpoints", Json::array({{{"ip", "192.168.42.7"}, {"control_port", 12345}}})}}, true);
            return 0;
        } catch (...) { return 1; }
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    constexpr auto window_class = L"PocketVR.Control.v2";
    // A second launch brings the open window forward instead of starting again.
    const auto single = CreateMutexW(nullptr, TRUE, L"PocketVR.Control.Instance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (const auto existing = FindWindowW(window_class, nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 1;
    App app;
    WNDCLASSW type{};
    type.lpfnWndProc = window_proc;
    type.hInstance = instance;
    type.lpszClassName = window_class;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    type.hbrBackground = CreateSolidBrush(RGB(11, 15, 23));
    if (!RegisterClassW(&type))
        return 1;
    const auto window = CreateWindowW(type.lpszClassName, L"PocketVR", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                      CW_USEDEFAULT, 980, 680, nullptr, nullptr, instance, &app);
    if (!window)
        return 1;
    ShowWindow(window, show);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
