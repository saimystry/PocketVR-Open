// SPDX-License-Identifier: Apache-2.0
#include "windows.hpp"
#include "gamepad.hpp"
#include "rumble.hpp"
#include <pocketvr/math.hpp>
#include <Xinput.h>
#include <mmsystem.h>
#include <atomic>
#include <thread>

using namespace pocketvr;
using namespace pocketvr::desktop;
namespace {
constexpr UINT status_message = WM_APP + 10;
constexpr int pause_hotkey = 100;
class Gamepad {
    int xindex_{-1}, jindex_{-1};
    JOYCAPSW caps_{};
    uint64_t next_probe_{};
  public:
    int xinput_index() const { return xindex_; }
    // Slow disconnected-device probing stays out of all OpenVR/video callbacks.
    bool poll(nlohmann::json &hands, bool &recenter, bool &generic) {
        const auto now = GetTickCount64();
        if (xindex_ < 0 && jindex_ < 0 && now >= next_probe_) {
            next_probe_ = now + 1000;
            XINPUT_STATE s{};
            for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
                if (XInputGetState(i, &s) == ERROR_SUCCESS) { xindex_ = static_cast<int>(i); break; }
            if (xindex_ < 0) {
                for (UINT i = 0; i < std::min<UINT>(joyGetNumDevs(), 16); ++i) {
                    JOYINFOEX j{sizeof(JOYINFOEX), JOY_RETURNALL};
                    if (joyGetPosEx(i, &j) == JOYERR_NOERROR &&
                        joyGetDevCapsW(i, &caps_, sizeof(caps_)) == JOYERR_NOERROR) {
                        jindex_ = static_cast<int>(i); break;
                    }
                }
            }
        }
        if (xindex_ >= 0) {
            XINPUT_STATE s{};
            if (XInputGetState(static_cast<DWORD>(xindex_), &s) != ERROR_SUCCESS) {
                xindex_ = -1; return false;
            }
            const auto &g = s.Gamepad;
            hands = xbox_hands(g.wButtons, g.bLeftTrigger, g.bRightTrigger,
                               g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY);
            recenter = (g.wButtons & XINPUT_GAMEPAD_BACK) != 0;
            generic = false;
            return true;
        }
        if (jindex_ >= 0) {
            JOYINFOEX j{sizeof(JOYINFOEX), JOY_RETURNALL};
            if (joyGetPosEx(static_cast<UINT>(jindex_), &j) != JOYERR_NOERROR) {
                jindex_ = -1; return false;
            }
            const auto b = [&j](unsigned n) { return (j.dwButtons & (1UL << n)) != 0; };
            float x = joystick_axis(j.dwXpos, caps_.wXmin, caps_.wXmax);
            float y = -joystick_axis(j.dwYpos, caps_.wYmin, caps_.wYmax);
            if ((caps_.wCaps & JOYCAPS_HASPOV) && j.dwPOV != JOY_POVCENTERED) {
                const double angle = j.dwPOV * pi / 18000.0;
                x = static_cast<float>(std::sin(angle));
                y = static_cast<float>(std::cos(angle));
            }
            const auto rx = (caps_.wCaps & JOYCAPS_HASR)
                ? joystick_axis(j.dwRpos, caps_.wRmin, caps_.wRmax) : 0;
            const auto ry = (caps_.wCaps & JOYCAPS_HASU)
                ? -joystick_axis(j.dwUpos, caps_.wUmin, caps_.wUmax) : 0;
            hands = nlohmann::json::array({
                hand(b(6) ? 1.0F : 0.0F, x, y, b(4), b(2), false, b(10)),
                hand(b(0) || b(7) ? 1.0F : 0.0F, rx, ry, b(1) || b(5), b(9), b(3), b(11))});
            recenter = b(8);
            generic = true;
            return true;
        }
        return false;
    }
};
class InputPad {
    HWND window_{}, label_{}, choice_{}, background_choice_{}, pause_button_{};
    std::atomic<unsigned> mouse_{};
    std::atomic<bool> gamepad_{true}, background_{}, paused_{}, recenter_{};
    std::atomic<bool> rumble_enabled_{true};
    std::atomic<uint64_t> input_generation_{};
    RumbleOutput rumble_{[](int device, Motors motors) {
        XINPUT_VIBRATION value{motors.left, motors.right};
        (void)XInputSetState(static_cast<DWORD>(device), &value);
    }};
    bool hotkey_registered_{};
    std::jthread worker_;
    void pause(bool value) {
        ++input_generation_; rumble_.stop();
        paused_.store(value);
        mouse_.store(0);
        recenter_.store(false);
        ReleaseCapture();
        SetWindowTextW(pause_button_, value ? L"Resume input" : L"Pause input (Esc)");
    }
  public:
    void create(HWND window) {
        window_ = window;
        CreateWindowW(L"STATIC", L"Look at the target in VR, then click in this window.\n"
            L"Left: select   Right: grip   Middle: menu\nR: recenter   Space: dashboard\n"
            L"Esc: pause (Resume below). Mouse movement does not turn your head.\n"
            L"Xbox: A/RT select, sticks move, LB/RB grip, Back recenter, Y dashboard.",
            WS_CHILD | WS_VISIBLE, 18, 18, 490, 110, window, nullptr, nullptr, nullptr);
        choice_ = CreateWindowW(L"BUTTON", L"Xbox 360 / XInput controller + mouse (generic USB fallback)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 18, 138, 460, 28,
            window, reinterpret_cast<HMENU>(1), nullptr, nullptr);
        SendMessageW(choice_, BM_SETCHECK, BST_CHECKED, 0);
        background_choice_ = CreateWindowW(L"BUTTON", L"Allow controller while a game or another window is in front",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 18, 172, 510, 28,
            window, reinterpret_cast<HMENU>(2), nullptr, nullptr);
        CreateWindowW(L"STATIC", L"Background controller: Ctrl+Alt+F8 pauses/resumes. Mouse stays local.",
            WS_CHILD | WS_VISIBLE, 18, 206, 510, 24, window, nullptr, nullptr, nullptr);
        pause_button_ = CreateWindowW(L"BUTTON", L"Pause input (Esc)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 18, 238, 180, 30,
            window, reinterpret_cast<HMENU>(3), nullptr, nullptr);
        const auto rumble_choice = CreateWindowW(L"BUTTON", L"Xbox rumble",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 220, 238, 170, 28,
            window, reinterpret_cast<HMENU>(4), nullptr, nullptr);
        SendMessageW(rumble_choice, BM_SETCHECK, BST_CHECKED, 0);
        label_ = CreateWindowW(L"STATIC", L"Connecting to SteamVR driver...", WS_CHILD | WS_VISIBLE,
            18, 280, 510, 60, window, nullptr, nullptr, nullptr);
        worker_ = std::jthread([this](std::stop_token stop) {
            Gamepad pad;
            bool previous_recenter = false;
            WPARAM previous_status = static_cast<WPARAM>(-1);
            const auto report = [&](WPARAM status) {
                if (status != previous_status) {
                    PostMessageW(window_, status_message, status, 0);
                    previous_status = status;
                }
            };
            while (!stop.stop_requested()) {
                const bool focused = GetAncestor(GetForegroundWindow(), GA_ROOT) == window_;
                const bool controller = gamepad_.load();
                const auto buttons = focused ? mouse_.load() : 0;
                auto hands = nlohmann::json::array({hand(), hand()});
                bool connected = false, center = false, generic = false;
                if (controller) connected = pad.poll(hands, center, generic);
                const bool paused = paused_.load();
                const auto snapshot = desktop_snapshot(focused, paused, connected,
                    background_.load(), buttons, hands);
                const auto generation = input_generation_.load();
                try {
                    const auto result = win::rpc({{"method", "input.desktop"},
                        {"active", snapshot.active}, {"gamepad", snapshot.gamepad}, {"hands", snapshot.hands},
                        {"haptics", rumble_enabled_.load() && connected && !generic}},
                        win::pipe_name, stop);
                    if (!result.value("ok", false)) throw std::runtime_error("Input RPC rejected");
                    std::array<TimedHaptic, 32> pulses{};
                    size_t pulse_count = 0;
                    if (const auto found = result.find("rumble"); found != result.end()) {
                        if (!found->is_array() || found->size() > pulses.size()) throw std::runtime_error("Invalid rumble batch");
                        for (const auto &event : *found) {
                            const auto hand_index = event.at("hand").get<int>();
                            const auto duration = event.at("duration_ms").get<int>();
                            if (hand_index < 0 || hand_index > 1 || duration < 0 || duration > 500) continue;
                            pulses[pulse_count++] = {{static_cast<uint16_t>(duration), static_cast<uint8_t>(hand_index),
                                event.at("amplitude").get<float>(), event.at("frequency").get<float>()},
                                event.at("timestamp_pc_ns").get<uint64_t>()};
                        }
                    }
                    const bool still_focused = GetAncestor(GetForegroundWindow(), GA_ROOT) == window_;
                    const bool allow_rumble = generation == input_generation_.load() && !paused_.load() &&
                        gamepad_.load() && rumble_enabled_.load() && connected && !generic && snapshot.active &&
                        (still_focused || background_.load()) && result.value("armed", false);
                    rumble_.update(pad.xinput_index(), allow_rumble, std::span(pulses).first(pulse_count));
                    const bool keyboard_center = recenter_.exchange(false);
                    const bool requested = snapshot.active && result.value("armed", false) &&
                        ((focused && keyboard_center) || (connected && center && !previous_recenter));
                    if (requested) (void)win::rpc({{"method", "recenter"}}, win::pipe_name, stop);
                    const WPARAM status = paused ? 7 : !snapshot.active ? 1 :
                        !result.value("armed", false) ? 3 : connected ? (!focused ? 8 : generic ? 5 : 4) :
                        controller ? 2 : 6;
                    report(status);
                } catch (...) {
                    rumble_.stop();
                    report(0);
                }
                previous_recenter = center;
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
            // Lease expiry also covers crashes and cancellation during RPC.
            rumble_.stop();
        });
    }
    LRESULT message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_COMMAND:
            if (LOWORD(wparam) == 1) {
                ++input_generation_; rumble_.stop();
                mouse_.store(0);
                gamepad_.store(SendMessageW(choice_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SetFocus(window_);
            } else if (LOWORD(wparam) == 2) {
                ++input_generation_; rumble_.stop();
                const bool enabled = SendMessageW(background_choice_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (enabled && !hotkey_registered_) {
                    hotkey_registered_ = RegisterHotKey(window_, pause_hotkey,
                        MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F8) != FALSE;
                    if (!hotkey_registered_) {
                        SendMessageW(background_choice_, BM_SETCHECK, BST_UNCHECKED, 0);
                        MessageBoxW(window_, L"Ctrl+Alt+F8 is already in use. Close the app using that shortcut before enabling background control.",
                            L"Pause shortcut unavailable", MB_OK | MB_ICONINFORMATION);
                        return 0;
                    }
                } else if (!enabled && hotkey_registered_) {
                    UnregisterHotKey(window_, pause_hotkey);
                    hotkey_registered_ = false;
                }
                background_.store(enabled);
                SetFocus(window_);
            } else if (LOWORD(wparam) == 3) {
                pause(!paused_.load());
                SetFocus(window_);
            } else if (LOWORD(wparam) == 4) {
                ++input_generation_; rumble_.stop();
                rumble_enabled_.store(SendMessageW(reinterpret_cast<HWND>(lparam), BM_GETCHECK, 0, 0) == BST_CHECKED);
                SetFocus(window_);
            }
            return 0;
        case WM_HOTKEY:
            if (wparam == pause_hotkey) pause(!paused_.load());
            return 0;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
            SetFocus(window_); SetCapture(window_);
            mouse_.fetch_or(message == WM_LBUTTONDOWN ? 1U : message == WM_RBUTTONDOWN ? 2U : 4U);
            return 0;
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
            mouse_.fetch_and(~(message == WM_LBUTTONUP ? 1U : message == WM_RBUTTONUP ? 2U : 4U));
            if (!(mouse_.load() & 7)) ReleaseCapture();
            return 0;
        case WM_KEYDOWN:
            if (wparam == 'R' && !(lparam & (1LL << 30))) recenter_.store(true);
            if (wparam == VK_SPACE) mouse_.fetch_or(8);
            if (wparam == VK_ESCAPE) pause(true);
            return 0;
        case WM_KEYUP:
            if (wparam == VK_SPACE) mouse_.fetch_and(~8U);
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) { mouse_.store(0); recenter_.store(false); ReleaseCapture(); }
            if (LOWORD(wparam) == WA_INACTIVE && !background_.load()) { ++input_generation_; rumble_.stop(); }
            break;
        case WM_CAPTURECHANGED:
            // Capture only concerns mouse buttons, not a held keyboard shortcut.
            mouse_.fetch_and(8U); return 0;
        case status_message: {
            constexpr std::array statuses{
                L"Driver unavailable. Start SteamVR with the updated driver.",
                L"Paused: focus this window to use VR input.",
                L"No controller detected. Mouse is ready; plug in an Xbox 360/XInput pad anytime.",
                L"Release all buttons/sticks to arm input.",
                L"Xbox / XInput + mouse ready. Back: recenter. Y: dashboard.",
                L"Generic USB gamepad ready. Mapping needs hardware verification.",
                L"Mouse ready. Look at a target in VR and left-click here.",
                L"Input paused. Choose Resume input, or press Ctrl+Alt+F8 if background control is enabled.",
                L"Background controller active. Ctrl+Alt+F8 pauses. Mouse input is not captured."};
            if (wparam < statuses.size()) SetWindowTextW(label_, statuses[wparam]);
            return 0;
        }
        case WM_DESTROY:
            ++input_generation_; rumble_.stop();
            mouse_.store(0);
            if (hotkey_registered_) UnregisterHotKey(window_, pause_hotkey);
            worker_.request_stop();
            if (worker_.joinable()) worker_.join();
            PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }
};
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto *pad = reinterpret_cast<InputPad *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    try {
        if (message == WM_CREATE) {
            pad = static_cast<InputPad *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pad));
            pad->create(window); return 0;
        }
        if (pad) return pad->message(message, wparam, lparam);
    } catch (...) { if (message == WM_CREATE) return -1; }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    win::Handle single(CreateMutexW(nullptr, FALSE, L"Local\\PocketVR.DesktopInput.v1"));
    if (!single) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (const auto existing = FindWindowW(L"PocketVR.Input.v1", nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }
    InputPad pad;
    WNDCLASSW type{};
    type.lpfnWndProc = procedure; type.hInstance = instance;
    type.lpszClassName = L"PocketVR.Input.v1";
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&type)) return 1;
    const auto window = CreateWindowW(type.lpszClassName, L"PocketVR mouse / controller input",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 575, 400, nullptr, nullptr, instance, &pad);
    if (!window) return 1;
    ShowWindow(window, show);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return 0;
}
