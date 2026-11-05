// SPDX-License-Identifier: Apache-2.0
#include "benchmark.hpp"
#include "windows.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <array>
#include <iomanip>
#include <mutex>
#include <sstream>

using namespace pocketvr::win;
using Json = nlohmann::json;
namespace {
constexpr UINT update_message = WM_APP + 30;
enum Id { start = 300, cancel, save, codec, resolution, fps, bitrate, frames, preset, quality };
std::wstring wide(const std::string &text) {
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("Cannot display benchmark result");
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
uint32_t number(HWND control, uint32_t low, uint32_t high) {
    std::array<wchar_t, 32> value{}; GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
    const std::wstring text(value.data());
    if (text.empty() || text.size() > 6 || text.find_first_not_of(L"0123456789") != std::wstring::npos)
        throw std::invalid_argument("Enter a whole number for bitrate and frame count");
    const auto parsed = std::stoul(text);
    if (parsed < low || parsed > high) throw std::invalid_argument("Use 10-150 Mbps and 30-600 frames");
    return static_cast<uint32_t>(parsed);
}
std::wstring decimal(const Json &value) {
    std::wostringstream text; text << std::fixed << std::setprecision(2) << value.get<double>();
    return text.str();
}
std::filesystem::path tool_path() {
    std::array<wchar_t, 32768> path{};
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) throw std::runtime_error("Cannot locate benchmark application");
    const auto directory = std::filesystem::path(path.data()).parent_path();
    for (const auto &candidate : {directory / L"pocketvr_encoder_bench.exe",
                                 directory.parent_path() / L"tools" / L"pocketvr_encoder_bench.exe"}) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;
    }
    throw std::runtime_error("Build pocketvr_encoder_bench.exe and keep it beside this app or in the sibling tools folder");
}
class Window {
    HWND window_{}, table_{}, status_{}, details_{}, codec_{}, resolution_{}, fps_{}, bitrate_{}, frames_{}, preset_{}, quality_{};
    bool busy_{};
    std::jthread worker_;
    std::mutex mutex_;
    Json report_{{"runs", Json::array()}, {"status", "idle"}};
    HWND child(const wchar_t *type, const wchar_t *label, DWORD style,
        int x, int y, int width, int height, int id = 0) {
        const auto handle = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style,
            x, y, width, height, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (!handle) throw std::runtime_error("Cannot create benchmark control");
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return handle;
    }
    HWND combo(int x, int width, int id, std::initializer_list<const wchar_t *> items) {
        const auto handle = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x, 55, width, 180, id);
        for (const auto *item : items) SendMessageW(handle, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
        SendMessageW(handle, CB_SETCURSEL, 0, 0); return handle;
    }
    void controls() {
        for (const auto id : {start, codec, resolution, fps, bitrate, frames, preset, quality})
            EnableWindow(GetDlgItem(window_, id), !busy_);
        EnableWindow(GetDlgItem(window_, cancel), busy_);
        bool have_results;
        { std::lock_guard lock(mutex_); have_results = !report_.at("runs").empty(); }
        EnableWindow(GetDlgItem(window_, save), !busy_ && have_results);
    }
    void publish(const Json &report) {
        { std::lock_guard lock(mutex_); report_ = report; }
        PostMessageW(window_, update_message, 0, 0);
    }
    void begin() {
        if (busy_) return;
        BenchmarkOptions options;
        options.codec = SendMessageW(codec_, CB_GETCURSEL, 0, 0) == 1 ? "hevc" : "h264";
        const auto dimensions = SendMessageW(resolution_, CB_GETCURSEL, 0, 0);
        if (dimensions == 1) { options.width = 1728; options.height = 864; }
        if (dimensions == 2) { options.width = 1280; options.height = 720; }
        const auto selected_fps = SendMessageW(fps_, CB_GETCURSEL, 0, 0);
        options.fps = selected_fps == 1 ? 45U : selected_fps == 2 ? 30U : 60U;
        options.bitrate = number(bitrate_, 10, 150) * 1'000'000;
        options.frames = number(frames_, 30, 600);
        options.quality = SendMessageW(quality_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        options.validate();
        const auto selected = SendMessageW(preset_, CB_GETCURSEL, 0, 0);
        std::vector<std::string> presets{"fast", "balanced", "quality"};
        if (selected > 0 && selected <= 3) presets = {presets[static_cast<size_t>(selected - 1)]};
        const auto executable = tool_path();
        Json report{{"format_version", 1}, {"kind", "sequential_pc_encoder_comparison"},
            {"status", "running"}, {"progress", "Checking for active streaming..."},
            {"request", {{"codec", options.codec}, {"width", options.width}, {"height", options.height},
                {"fps", options.fps}, {"target_bitrate_mbps", options.bitrate / 1e6},
                {"frames_per_preset", options.frames}, {"sample_quality", options.quality}, {"presets", presets}}},
            {"phone_decode_measured", false}, {"headset_latency_measured", false},
            {"notes", "Synthetic serial encode test; wall FPS includes CPU generation/optional PC decode. Not sustained streaming or phone latency. No automatic settings changes."},
            {"runs", Json::array()}};
        SYSTEMTIME utc{}; GetSystemTime(&utc);
        report["captured_utc"] = {utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond};
        publish(report);
        worker_ = std::jthread([this, executable, options, presets, report](std::stop_token stop) mutable {
            try {
                bool streaming = false;
                try {
                    const auto status = pocketvr::win::rpc({{"method", "status"}}, pipe_name, stop);
                    streaming = status.value("ok", false) && status.value("session_connected", false);
                } catch (...) { if (stop.stop_requested()) throw; }
                if (streaming) throw std::runtime_error("Stop the phone stream before benchmarking; an extra encoder would distort results and hurt latency");
                for (size_t i = 0; i < presets.size(); ++i) {
                    if (stop.stop_requested()) break;
                    const auto &name = presets[i];
                    report["progress"] = "Running " + name + " (" + std::to_string(i + 1) + "/" + std::to_string(presets.size()) + ")...";
                    publish(report);
                    try {
                        const auto metrics = run_benchmark(executable, options, name, stop);
                        report["runs"].push_back({{"preset", name}, {"status", "ok"}, {"metrics", metrics}});
                    } catch (const std::exception &error) {
                        report["runs"].push_back({{"preset", name},
                            {"status", stop.stop_requested() ? "cancelled" : "failed"}, {"error", error.what()}});
                    }
                    publish(report);
                }
                bool failed = false;
                for (const auto &run : report["runs"]) failed = failed || run.at("status") != "ok";
                report["status"] = stop.stop_requested() ? "cancelled" : failed ? "completed_with_errors" : "completed";
                report["progress"] = stop.stop_requested() ? "Cancelled. Completed results are retained." :
                    failed ? "Comparison finished with errors. See details; no settings were changed." :
                    "Comparison complete. Save the report; no settings were changed.";
            } catch (const std::exception &error) {
                report["status"] = stop.stop_requested() ? "cancelled" : "failed";
                report["progress"] = error.what();
            }
            publish(report);
        });
        busy_ = true;
        controls();
    }
    void cell(int row, int column, const std::wstring &text) {
        LVITEMW item{}; item.iSubItem = column; item.pszText = const_cast<wchar_t *>(text.c_str());
        SendMessageW(table_, LVM_SETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
    }
    void update() {
        Json report; { std::lock_guard lock(mutex_); report = report_; }
        if (report.at("status") != "running") {
            if (worker_.joinable()) worker_.join();
            busy_ = false;
        }
        controls();
        SetWindowTextW(status_, wide(report.value("progress", "Ready.")).c_str());
        SendMessageW(table_, LVM_DELETEALLITEMS, 0, 0);
        std::wstring detail = L"PC encode metrics only. Reference target: encode p95 below 8 ms at 2160x1080.\r\n";
        int row = 0;
        for (const auto &run : report.at("runs")) {
            auto name = wide(run.at("preset").get<std::string>());
            LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = row; item.pszText = name.data();
            SendMessageW(table_, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
            if (run.at("status") == "ok") {
                const auto &m = run.at("metrics");
                cell(row, 1, wide(m.at("encoder").get<std::string>()));
                cell(row, 2, L"Yes"); cell(row, 3, decimal(m.at("encode_ms_p50")));
                cell(row, 4, decimal(m.at("encode_ms_p95"))); cell(row, 5, decimal(m.at("target_bitrate_mbps")));
                cell(row, 6, decimal(m.at("actual_bitrate_mbps")));
                cell(row, 7, m.contains("sampled_luma_psnr_db_p50_capped_100") ? decimal(m.at("sampled_luma_psnr_db_p50_capped_100")) : L"Not sampled");
                cell(row, 8, L"Measured");
                detail += name + L": GPU " + wide(m.at("gpu_description").get<std::string>()) +
                    L"; serial wall FPS " + decimal(m.at("wall_fps_including_synthetic_generation_and_quality")) +
                    L" (includes CPU generation; not streaming FPS).\r\n";
                if (m.contains("software_decode_ms_p95")) detail += L"  Optional PC software decode p95: " + decimal(m.at("software_decode_ms_p95")) + L" ms; not Pixel decode.\r\n";
            } else {
                cell(row, 8, wide(run.at("status").get<std::string>()));
                detail += name + L": " + wide(run.value("error", "Unknown failure")) + L"\r\n";
            }
            ++row;
        }
        SetWindowTextW(details_, detail.c_str());
    }
    void save_report() {
        if (busy_) return;
        Json snapshot; { std::lock_guard lock(mutex_); snapshot = report_; }
        if (snapshot.at("runs").empty()) return;
        std::array<wchar_t, 32768> filename{};
        const std::wstring initial = L"PocketVR-encoder-comparison.json";
        std::copy(initial.begin(), initial.end(), filename.begin());
        OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"Benchmark JSON\0*.json\0\0"; dialog.lpstrFile = filename.data();
        dialog.nMaxFile = static_cast<DWORD>(filename.size()); dialog.lpstrDefExt = L"json";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&dialog)) return;
        const auto data = snapshot.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
        atomic_write(std::filesystem::path(filename.data()), std::span(reinterpret_cast<const uint8_t *>(data.data()), data.size()));
        SetWindowTextW(status_, L"Comparison report saved locally.");
    }
  public:
    ~Window() { worker_.request_stop(); if (worker_.joinable()) worker_.join(); }
    void create(HWND window) {
        window_ = window;
        child(L"STATIC", L"Encoder benchmark: stop streaming first. Runs on this PC's GPU; no phone/headset latency measurement.", 0, 18, 15, 1065, 25);
        child(L"STATIC", L"Codec", 0, 18, 36, 90, 20); codec_ = combo(18, 95, codec, {L"H.264", L"HEVC"});
        child(L"STATIC", L"Stereo resolution", 0, 125, 36, 160, 20); resolution_ = combo(125, 165, resolution, {L"2160 x 1080", L"1728 x 864", L"1280 x 720"});
        child(L"STATIC", L"FPS", 0, 303, 36, 70, 20); fps_ = combo(303, 70, fps, {L"60", L"45", L"30"});
        child(L"STATIC", L"Target Mbps", 0, 388, 36, 100, 20);
        bitrate_ = child(L"EDIT", L"60", WS_BORDER | ES_NUMBER | WS_TABSTOP, 388, 55, 90, 26, bitrate);
        child(L"STATIC", L"Frames / preset", 0, 493, 36, 120, 20);
        frames_ = child(L"EDIT", L"120", WS_BORDER | ES_NUMBER | WS_TABSTOP, 493, 55, 100, 26, frames);
        child(L"STATIC", L"Presets", 0, 610, 36, 170, 20); preset_ = combo(610, 180, preset, {L"Compare all three", L"Fast", L"Balanced", L"Quality"});
        quality_ = child(L"BUTTON", L"Sample luma quality (slower)", BS_AUTOCHECKBOX | WS_TABSTOP, 810, 55, 270, 26, quality);
        SendMessageW(bitrate_, EM_SETLIMITTEXT, 3, 0); SendMessageW(frames_, EM_SETLIMITTEXT, 3, 0);
        child(L"BUTTON", L"Run comparison", WS_TABSTOP, 18, 98, 160, 31, start);
        child(L"BUTTON", L"Cancel", WS_TABSTOP, 192, 98, 110, 31, cancel);
        child(L"BUTTON", L"Save report...", WS_TABSTOP, 316, 98, 145, 31, save);
        status_ = child(L"STATIC", L"Ready. Select settings and Run comparison. Hardware encoder required; no software fallback.", 0, 18, 143, 1065, 45);
        table_ = child(WC_LISTVIEWW, L"", WS_BORDER | LVS_REPORT | LVS_SINGLESEL, 18, 195, 1065, 178);
        SendMessageW(table_, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        const std::array labels{L"Preset", L"Actual encoder", L"HW", L"Encode p50 ms", L"Encode p95 ms", L"Target Mbps", L"Encoded Mbps", L"Luma PSNR dB", L"Status"};
        const std::array widths{80, 165, 40, 110, 110, 100, 100, 110, 110};
        for (size_t i = 0; i < labels.size(); ++i) {
            LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.cx = widths[i];
            column.pszText = const_cast<wchar_t *>(labels[i]);
            SendMessageW(table_, LVM_INSERTCOLUMNW, i, reinterpret_cast<LPARAM>(&column));
        }
        details_ = child(L"EDIT", L"Comparisons use sequential synthetic frames, not a running VR game.\r\n"
            L"Optional quality is sampled PC software-decoded luma PSNR, not a headset visual-quality verdict.",
            WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 18, 389, 1065, 150);
        controls();
    }
    LRESULT message(UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == update_message) { update(); return 0; }
        if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED) {
            if (LOWORD(wparam) == start) begin();
            if (LOWORD(wparam) == cancel) { worker_.request_stop(); SetWindowTextW(status_, L"Cancelling the current benchmark; completed results will be retained..."); }
            if (LOWORD(wparam) == save) save_report();
            return 0;
        }
        if (message == WM_DESTROY) {
            worker_.request_stop(); if (worker_.joinable()) worker_.join(); PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }
};
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto *app = reinterpret_cast<Window *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    try {
        if (message == WM_CREATE) {
            app = static_cast<Window *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->create(window); return 0;
        }
        if (app) return app->message(message, wparam, lparam);
    } catch (const std::exception &error) {
        MessageBoxW(window, wide(error.what()).c_str(), L"PocketVR benchmark", MB_OK | MB_ICONERROR);
        if (message == WM_CREATE) return -1;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    if (!InitCommonControlsEx(&controls)) return 1;
    Window app;
    WNDCLASSW type{}; type.lpfnWndProc = procedure; type.hInstance = instance;
    type.lpszClassName = L"PocketVR.Benchmark.v1"; type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&type)) return 1;
    const auto window = CreateWindowW(type.lpszClassName, L"PocketVR encoder benchmark",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 1120, 600, nullptr, nullptr, instance, &app);
    if (!window) return 1;
    ShowWindow(window, show);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return 0;
}
