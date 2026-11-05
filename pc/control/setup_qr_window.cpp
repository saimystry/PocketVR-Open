// SPDX-License-Identifier: Apache-2.0
#include "windows.hpp"
#include "setup_qr.hpp"
#include <optional>

namespace {
struct QrWindow {
    std::vector<pocketvr::Endpoint> endpoints;
    std::optional<pocketvr::win::SetupQr> code;
    HWND combo{}, label{};
    bool closed{};
};
LRESULT CALLBACK qr_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto *state = reinterpret_cast<QrWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<QrWindow *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wparam, lparam);
    try {
        if (message == WM_CREATE) {
            const auto instance = GetModuleHandleW(nullptr);
            CreateWindowW(L"STATIC", L"Choose the PC address on the phone's LAN.\nScan in PocketVR, then connect and confirm pairing on both devices.",
                WS_VISIBLE | WS_CHILD, 20, 12, 510, 42, window, nullptr, instance, nullptr);
            state->combo = CreateWindowW(L"COMBOBOX", L"", WS_VISIBLE | WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                20, 62, 510, 160, window, reinterpret_cast<HMENU>(1), instance, nullptr);
            for (const auto &endpoint : state->endpoints) {
                const auto value = endpoint.ip() + ":" + std::to_string(endpoint.port);
                const std::wstring label(value.begin(), value.end());
                SendMessageW(state->combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
            SendMessageW(state->combo, CB_SETCURSEL, 0, 0);
            state->label = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | ES_READONLY | ES_AUTOHSCROLL,
                20, 510, 510, 24, window, nullptr, instance, nullptr);
            CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                425, 545, 105, 28, window, reinterpret_cast<HMENU>(2), instance, nullptr);
            SendMessageW(window, WM_COMMAND, MAKEWPARAM(1, CBN_SELCHANGE), 0);
            return 0;
        }
        if (message == WM_TIMER) { DestroyWindow(window); return 0; }
        if (message == WM_COMMAND) {
            if (LOWORD(wparam) == 2) { DestroyWindow(window); return 0; }
            if (LOWORD(wparam) == 1 && HIWORD(wparam) == CBN_SELCHANGE) {
                const auto selected = SendMessageW(state->combo, CB_GETCURSEL, 0, 0);
                if (selected >= 0 && static_cast<size_t>(selected) < state->endpoints.size()) {
                    state->code = pocketvr::win::connection_qr(state->endpoints[static_cast<size_t>(selected)]);
                    const auto &value = state->code->text;
                    SetWindowTextW(state->label, std::wstring(value.begin(), value.end()).c_str());
                    InvalidateRect(window, nullptr, TRUE);
                }
            }
            return 0;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT ps{};
            const auto dc = BeginPaint(window, &ps);
            if (state->code) {
                const auto &code = *state->code;
                // Four-module quiet zone; integral modules, no resampling/antialiasing.
                const int scale = 400 / (code.size + 8);
                const int side = scale * (code.size + 8), left = (550 - side) / 2, top = 100;
                RECT background{left, top, left + side, top + side};
                FillRect(dc, &background, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
                for (int y = 0; y < code.size; ++y)
                    for (int x = 0; x < code.size; ++x)
                        if (code.dark[static_cast<size_t>(y) * code.size + x]) {
                            RECT cell{left + (x + 4) * scale, top + (y + 4) * scale,
                                      left + (x + 5) * scale, top + (y + 5) * scale};
                            FillRect(dc, &cell, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                        }
            }
            EndPaint(window, &ps);
            return 0;
        }
        if (message == WM_DESTROY) { state->closed = true; return 0; }
    } catch (const std::exception &) {
        MessageBoxW(window, L"Could not generate this connection code.", L"PocketVR", MB_OK | MB_ICONERROR);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
void show_connection_qr(HWND parent, const nlohmann::json &network, bool smoke) {
    QrWindow state;
    if (network.contains("endpoints")) for (const auto &item : network.at("endpoints")) {
        const auto endpoint = pocketvr::Endpoint::parse(item.at("ip"), item.at("control_port"));
        try { (void)pocketvr::win::connection_qr(endpoint); state.endpoints.push_back(endpoint); }
        catch (const std::invalid_argument &) {} // Exclude loopback/USB and public interfaces.
    }
    if (state.endpoints.empty())
        throw std::runtime_error("No active private LAN listener. Start the driver on a private network, or use USB setup.");
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.lpfnWndProc = qr_proc; type.hInstance = instance; type.lpszClassName = L"PocketVR.ConnectionQr.v1";
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Cannot register QR window");
    const auto window = CreateWindowW(type.lpszClassName, L"PocketVR connection QR",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 570, 625,
        parent, nullptr, instance, &state);
    if (!window) throw std::runtime_error("Cannot create QR window");
    if (parent) EnableWindow(parent, FALSE);
    if (smoke) {
        if (!state.code || state.code->text != "pocketvr://connect?v=1&ip=192.168.42.7&port=12345") {
            DestroyWindow(window);
            throw std::runtime_error("QR control initialization failed");
        }
        if (!SetTimer(window, 1, 100, nullptr)) {
            DestroyWindow(window);
            throw std::runtime_error("QR smoke timer failed");
        }
    } else ShowWindow(window, SW_SHOW);
    MSG message{};
    while (!state.closed) {
        const auto result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            if (IsWindow(window)) DestroyWindow(window);
            if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { DestroyWindow(window); continue; }
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (parent) { EnableWindow(parent, TRUE); SetActiveWindow(parent); }
}
