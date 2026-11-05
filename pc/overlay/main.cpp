// SPDX-License-Identifier: Apache-2.0
#include "graphics.hpp"
#include "input.hpp"
#include "ui.hpp"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <shellapi.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <algorithm>
#include <crtdbg.h>
#include <spdlog/spdlog.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
using namespace pocketvr::dashboard;
namespace {
constexpr char app_key[] = "org.pocketvr.dashboard";
struct Apartment {
    Apartment() { check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize COM"); }
    ~Apartment() { CoUninitialize(); }
};
std::filesystem::path executable() {
    std::array<wchar_t, 32768> path{};
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count == path.size()) throw std::runtime_error("Cannot locate overlay executable");
    return std::filesystem::path(path.data());
}
std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string(); return {text.begin(), text.end()};
}
struct Runtime {
    vr::IVRSystem *system{};
    explicit Runtime(vr::EVRApplicationType type) {
        if (!vr::VR_IsRuntimeInstalled()) throw std::runtime_error("SteamVR is not installed. Use --preview for the desktop panel.");
        vr::EVRInitError error{}; system = vr::VR_Init(&error, type);
        if (error != vr::VRInitError_None) throw std::runtime_error(vr::VR_GetVRInitErrorAsEnglishDescription(error));
    }
    ~Runtime() { vr::VR_Shutdown(); }
};
void register_manifest(bool remove) {
    Runtime runtime(vr::VRApplication_Utility);
    auto *apps = vr::VRApplications();
    if (!apps) throw std::runtime_error("SteamVR application registry unavailable");
    const auto manifest = executable().parent_path() / "app.vrmanifest";
    if (!std::filesystem::is_regular_file(manifest)) throw std::runtime_error("app.vrmanifest is missing beside the overlay");
    const auto path = utf8(manifest);
    const bool installed = apps->IsApplicationInstalled(app_key);
    if (installed) {
        std::array<char, 32768> binary{};
        vr::EVRApplicationError property_error{};
        apps->GetApplicationPropertyString(app_key, vr::VRApplicationProperty_BinaryPath_String,
                                          binary.data(), static_cast<uint32_t>(binary.size()), &property_error);
        if (property_error != vr::VRApplicationError_None)
            throw std::runtime_error("Cannot verify the registered PocketVR dashboard executable.");
        if (!std::filesystem::equivalent(std::filesystem::path(std::u8string(binary.begin(), std::find(binary.begin(), binary.end(), '\0'))), executable())) {
            if (remove) return; // A different package's dashboard must survive this uninstall.
            throw std::runtime_error("Another PocketVR dashboard path is registered. Unregister that package first.");
        }
    }
    auto error = vr::VRApplicationError_None;
    if (remove) {
        if (!installed) return;
        error = apps->SetApplicationAutoLaunch(app_key, false);
        if (error == vr::VRApplicationError_None) error = apps->RemoveApplicationManifest(path.c_str());
    } else {
        error = apps->AddApplicationManifest(path.c_str(), false);
        if (error == vr::VRApplicationError_None) {
            error = apps->SetApplicationAutoLaunch(app_key, true);
            if (error != vr::VRApplicationError_None && !installed) {
                const auto rollback = apps->RemoveApplicationManifest(path.c_str());
                if (rollback != vr::VRApplicationError_None)
                    throw std::runtime_error("UNCERTAIN: Dashboard autolaunch failed and registration rollback failed. Keep this package and unregister it manually.");
            }
        }
    }
    if (error != vr::VRApplicationError_None) throw std::runtime_error(apps->GetApplicationsErrorNameFromEnum(error));
}
struct Dashboard {
    vr::IVROverlay *api{};
    vr::VROverlayHandle_t main{}, thumbnail{};
    bool existing{};
    explicit Dashboard(Graphics &graphics) {
        api = vr::VROverlay();
        if (!api) throw std::runtime_error("SteamVR overlay interface unavailable");
        const auto check_vr = [&](vr::EVROverlayError error) {
            if (error != vr::VROverlayError_None) throw std::runtime_error(api->GetOverlayErrorNameFromEnum(error));
        };
        const auto created = api->CreateDashboardOverlay(app_key, "PocketVR", &main, &thumbnail);
        if (created == vr::VROverlayError_KeyInUse) { main = thumbnail = 0; existing = true; return; }
        check_vr(created);
        try {
            check_vr(api->SetOverlayWidthInMeters(main, 1.6f));
            check_vr(api->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse));
            const vr::HmdVector2_t scale{{static_cast<float>(width), static_cast<float>(height)}};
            check_vr(api->SetOverlayMouseScale(main, &scale));
            check_vr(api->SetOverlayFlag(main, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true));
            const vr::VRTextureBounds_t bounds{0, 0, 1, 1};
            check_vr(api->SetOverlayTextureBounds(main, &bounds));
            // Original P glyph, RGBA, not a third-party logo or font asset.
            std::vector<uint8_t> icon(128 * 128 * 4);
            for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) {
                const bool p = (x >= 32 && x < 48 && y >= 24 && y < 104) ||
                    (x >= 48 && x < 88 && ((y >= 24 && y < 40) || (y >= 57 && y < 73))) ||
                    (x >= 80 && x < 96 && y >= 32 && y < 65);
                const auto i = static_cast<size_t>(y * 128 + x) * 4;
                icon[i] = p ? 245 : 16; icon[i + 1] = p ? 250 : 65; icon[i + 2] = p ? 255 : 92; icon[i + 3] = 255;
            }
            check_vr(api->SetOverlayRaw(thumbnail, icon.data(), 128, 128, 4));
            (void)graphics;
        } catch (...) { close(); throw; }
    }
    ~Dashboard() { close(); }
    void close() noexcept {
        if (main) { api->ClearOverlayTexture(main); api->DestroyOverlay(main); main = 0; }
        if (thumbnail) { api->ClearOverlayTexture(thumbnail); api->DestroyOverlay(thumbnail); thumbnail = 0; }
    }
    void submit(Graphics &graphics) {
        vr::Texture_t texture{graphics.texture(), vr::TextureType_DirectX, vr::ColorSpace_Gamma};
        const auto error = api->SetOverlayTexture(main, &texture);
        if (error != vr::VROverlayError_None) throw std::runtime_error(api->GetOverlayErrorNameFromEnum(error));
    }
};
LRESULT CALLBACK preview_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam)) return TRUE;
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
HWND preview_window() {
    WNDCLASSW type{}; type.lpfnWndProc = preview_proc; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"PocketVR.DashboardPreview.v1"; type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&type)) throw std::runtime_error("Cannot register dashboard preview");
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT bounds{0, 0, width, height}; AdjustWindowRect(&bounds, style, FALSE);
    const auto window = CreateWindowW(type.lpszClassName, L"PocketVR dashboard - desktop preview", style,
        CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, type.hInstance, nullptr);
    if (!window) throw std::runtime_error("Cannot create dashboard preview");
    return window;
}
UiActions live_actions(Client &client) {
    return {[&](Json command) { return client.command(std::move(command)); },
        [&] { return client.read_logs(); }, [&] { return client.export_bundle(); },
        [](bool diagnostics) {
            const auto path = pocketvr::win::appdata(true) / (diagnostics ? "diagnostics" : "logs");
            std::filesystem::create_directories(path);
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                throw std::runtime_error("Cannot open the desktop folder");
        }};
}
void imgui_input(InputAction action, bool &done) {
        auto &io = ImGui::GetIO();
        switch (action.kind) {
        case InputAction::move: io.AddMousePosEvent(action.x, action.y); break;
        case InputAction::button: io.AddMouseButtonEvent(action.index, action.down); break;
        case InputAction::wheel: io.AddMouseWheelEvent(action.x, action.y); break;
        case InputAction::focus: io.AddFocusEvent(action.down); break;
        case InputAction::quit: done = true; break;
        }
}
int run(bool preview, int show, bool smoke = false, bool show_dashboard = false) {
    Apartment apartment;
    std::unique_ptr<Runtime> runtime;
    int adapter = -1;
    if (!preview) {
        runtime = std::make_unique<Runtime>(vr::VRApplication_Overlay);
        if (!runtime->system) throw std::runtime_error("SteamVR system interface unavailable");
        runtime->system->GetDXGIOutputInfo(&adapter);
        if (adapter < 0) throw std::runtime_error("SteamVR did not identify its GPU");
    }
    HWND window = preview ? preview_window() : nullptr;
    Graphics graphics(adapter, false, window);
    std::unique_ptr<Dashboard> dashboard;
    if (!preview) {
        dashboard = std::make_unique<Dashboard>(graphics);
        if (dashboard->existing) {
            if (show_dashboard) dashboard->api->ShowDashboard(app_key);
            return 0; // Leave the owning process and its textures untouched.
        }
    }
    else ShowWindow(window, smoke ? SW_HIDE : show);
    Client client; Ui ui; Pointer pointer;
    bool done = false, first = true, was_visible = false;
    unsigned rendered{};
    const auto sink = [&](InputAction action) { imgui_input(action, done); };
    auto previous = Clock::now();
    const auto actions = live_actions(client);
    while (!done) {
        const auto started = Clock::now();
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { done = true; break; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (dashboard) {
            vr::VREvent_t event{};
            for (int i = 0; i < 256 && dashboard->api->PollNextOverlayEvent(dashboard->main, &event, sizeof(event)); ++i)
                pointer.event(event, sink);
            for (int i = 0; i < 64 && runtime->system->PollNextEvent(&event, sizeof(event)); ++i)
                if (event.eventType == vr::VREvent_Quit) { pointer.reset(sink); runtime->system->AcknowledgeQuit_Exiting(); done = true; }
        }
        if (done) break;
        const bool visible = preview ? !IsIconic(window) : dashboard->api->IsOverlayVisible(dashboard->main);
        if (dashboard && visible != was_visible) {
            if (!visible) pointer.reset(sink);
            else sink({InputAction::focus, 0, 0, 0, true});
        }
        was_visible = visible;
        if (visible || first) {
            graphics.begin(std::chrono::duration<float>(started - previous).count());
            ui.draw(client.snapshot(), actions); graphics.render();
            if (dashboard) {
                dashboard->submit(graphics);
                if (show_dashboard) { dashboard->api->ShowDashboard(app_key); show_dashboard = false; }
            }
            previous = started; first = false;
            if (smoke && ++rendered >= 10) done = true;
        }
        // UI-only cadence. Hidden panels submit no textures and do no rendering.
        std::this_thread::sleep_until(started + std::chrono::milliseconds(visible ? 33 : 100));
    }
    return 0;
}
int self_test(const std::filesystem::path &output) {
    Apartment apartment;
    std::filesystem::create_directories(output);
    Graphics graphics(-1, true); Ui ui; ui.fixture = true;
    const UiActions actions{[](Json) { return true; }, [] { return true; }, [] { return true; }, [](bool) {}};
    auto snapshot = fixture_snapshot();
    const auto origin = Clock::now();
    for (int i = 0; i < 20; ++i) {
        snapshot.received = origin - std::chrono::milliseconds((19 - i) * 100); ++snapshot.revision;
        auto &session = snapshot.data["network"]["session"];
        session["sent_bytes"] = static_cast<uint64_t>(i + 1) * 750000;
        session["encoded_frames"] = (i + 1) * 6;
        session["dropped_frames"] = i / 10;
        graphics.begin(1.f / 30); ui.draw(snapshot, actions); graphics.render();
    }
    constexpr const char *names[]{"status", "video", "display", "tracking", "input", "pairing", "about"};
    for (int page = 0; page < 7; ++page) {
        ui.page = page; snapshot.received = Clock::now();
        for (int settle = 0; settle < 2; ++settle) {
            graphics.begin(1.f / 30); ui.draw(snapshot, actions); graphics.render();
        }
        graphics.capture(output / (std::string(names[page]) + ".png"));
    }
    bool done = false;
    Pointer pointer;
    const auto sink = [&](InputAction action) { imgui_input(action, done); };
    for (const auto page : {2, 5}) {
        ui.page = page; snapshot.received = Clock::now();
        graphics.begin(1.f / 30); ui.draw(snapshot, actions); graphics.render();
        vr::VREvent_t move{}; move.eventType = vr::VREvent_MouseMove; move.data.mouse = {600, 500, 0, 0};
        pointer.event(move, sink);
        for (int frame = 0; frame < 4; ++frame) {
            vr::VREvent_t wheel{}; wheel.eventType = vr::VREvent_ScrollDiscrete; wheel.data.scroll.ydelta = -10;
            pointer.event(wheel, sink);
            snapshot.received = Clock::now();
            graphics.begin(1.f / 30); ui.draw(snapshot, actions); graphics.render();
        }
        graphics.capture(output / (page == 2 ? "lens-bottom.png" : "pairing-qr.png"));
    }
    ui.page = 3; snapshot.received = Clock::now();
    snapshot.data["settings"]["values"]["tracking"]["prediction_ms"] = 40.0;
    pointer.reset(sink); ImGui::GetIO().AddFocusEvent(true);
    for (int frame = 0; frame < 2; ++frame) { graphics.begin(1.f / 30); ui.draw(snapshot, actions); graphics.render(); }
    graphics.capture(output / "tracking-manual.png");
    snapshot.error = "Driver unavailable - OFFLINE TEST FIXTURE"; ++snapshot.revision; ui.page = 1;
    graphics.begin(1.f / 30); ui.draw(snapshot, actions); graphics.render(); graphics.capture(output / "offline.png");
    return 0;
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int show) {
    int count{}; auto *raw = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!raw) return 2;
    std::vector<std::wstring> args;
    for (int i = 1; i < count; ++i) args.emplace_back(raw[i]);
    LocalFree(raw);
    const bool test = args.size() == 2 && args[0] == L"--self-test";
    try {
        if (test) {
            SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
            _set_error_mode(_OUT_TO_STDERR);
#ifdef _DEBUG
            _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
            return self_test(args[1]);
        }
        if (args.size() == 1 && args[0] == L"--probe-runtime") {
            std::cout << Json{{"runtime_installed", vr::VR_IsRuntimeInstalled()}}.dump() << '\n'; return 0;
        }
        if ((args.size() == 1 || (args.size() == 2 && args[1] == L"--quiet")) &&
            (args[0] == L"--register" || args[0] == L"--unregister")) {
            register_manifest(args[0] == L"--unregister"); return 0;
        }
        if (args.empty() || (args.size() == 1 && (args[0] == L"--preview" || args[0] == L"--preview-smoke-test" || args[0] == L"--show"))) {
            pocketvr::win::initialize_logging("overlay");
            const bool open = !args.empty() && args[0] == L"--show";
            return run(!args.empty() && !open, show, !args.empty() && args[0] == L"--preview-smoke-test", open);
        }
        throw std::runtime_error("Usage: pocketvr_overlay [--show | --preview | --register | --unregister | --probe-runtime | --self-test DIRECTORY]");
    } catch (const std::exception &error) {
        if ((args.size() == 1 && args[0] == L"--preview-smoke-test") ||
            (args.size() == 2 && args[1] == L"--quiet")) {
            std::cerr << error.what() << '\n';
        } else if (test) {
            std::filesystem::create_directories(args[1]);
            std::ofstream(std::filesystem::path(args[1]) / "error.txt") << error.what();
        } else {
            spdlog::error("Dashboard failure: {}", error.what());
            const std::string message = error.what();
            const int size = MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0);
            std::wstring wide(static_cast<size_t>(size), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), wide.data(), size);
            MessageBoxW(nullptr, wide.c_str(), L"PocketVR dashboard", MB_OK | MB_ICONERROR);
        }
        return std::string_view(error.what()).starts_with("UNCERTAIN:") ? 2 : 1;
    }
}
