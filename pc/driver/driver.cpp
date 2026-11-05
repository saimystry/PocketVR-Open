// SPDX-License-Identifier: Apache-2.0
#include <pocketvr/profiles.hpp>
#include <Windows.h>
#include <Xinput.h>
#include "hmd.hpp"
#include "boundary.hpp"
#include "network.hpp"
#include <cstring>
#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <windows.h>
#include <timeapi.h>

namespace pocketvr::driver {
namespace {
// USB needs no setup: this quiet helper keeps the cable link up while SteamVR
// runs, and exits by itself shortly after the driver stops.
void start_usb_helper() noexcept {
    try {
        HMODULE self{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&start_usb_helper), &self))
            return;
        std::array<wchar_t, 32768> path{};
        const auto length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length == path.size())
            return;
        // <package>/bin/win64/driver_pocketvr.dll -> <package>/control/pocketvr_usb.exe
        const auto helper = std::filesystem::path(path.data()).parent_path().parent_path().parent_path() /
                            L"control" / L"pocketvr_usb.exe";
        if (!std::filesystem::is_regular_file(helper))
            return;
        std::wstring command = L"\"" + helper.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &startup, &process)) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
    } catch (...) {
    }
}
} // namespace
class Provider final : public vr::IServerTrackedDeviceProvider {
  public:
    vr::EVRInitError Init(vr::IVRDriverContext *context) override {
        const auto result = vr::InitServerDriverContext(context);
        if (result != vr::VRInitError_None)
            return result;
        const auto status = boundary(
            "Provider::Init",
            [&] {
                win::initialize_logging("driver");
                // Every sleep in this process otherwise rounds up to the system tick.
                timer_raised_ = timeBeginPeriod(1) == TIMERR_NOERROR;
                state_ = std::make_unique<State>();
                gpu_ = std::make_unique<Gpu>();
                gpu_restart_requested_ = false;
                hmd_ = std::make_unique<Hmd>(*state_, *gpu_);
                rpc_ = std::make_unique<win::RpcServer>([this](const auto &request) {
                    const auto method = request.at("method").template get<std::string>();
                    if (method == "compositor.status")
                        return hmd_->compositor().diagnostics();
                    if (method == "compositor.capture") {
                        hmd_->compositor().request_capture();
                        return nlohmann::json{{"ok", true}, {"state", "Capture requested for next streamed frame"}};
                    }
                    if (method.starts_with("network.") || method.starts_with("pairing.")) {
                        std::lock_guard lock(network_mutex_);
                        if (!network_)
                            return nlohmann::json{{"ok", false}, {"error", network_error_}};
                        return network_->request(request);
                    }
                    return state_->request(request);
                });
                if (!vr::VRServerDriverHost()->TrackedDeviceAdded(
                        "PocketVR-HMD", vr::TrackedDeviceClass_HMD, hmd_.get()))
                    throw std::runtime_error("SteamVR rejected HMD registration");
                left_ = std::make_unique<VirtualController>(true);
                right_ = std::make_unique<VirtualController>(false);
                if (!vr::VRServerDriverHost()->TrackedDeviceAdded(
                        "PocketVR-Left", vr::TrackedDeviceClass_Controller, left_.get()) ||
                    !vr::VRServerDriverHost()->TrackedDeviceAdded(
                        "PocketVR-Right", vr::TrackedDeviceClass_Controller, right_.get()))
                    throw std::runtime_error("SteamVR rejected desktop controllers");
                start_usb_helper();
                network_manager_ = std::jthread([this](std::stop_token token) {
                    boundary("network manager", [&] {
                        uint16_t control_port{}, media_port{};
                        std::vector<Endpoint> bound_addresses, pending_addresses;
                        unsigned address_samples = 0;
                        while (!token.stop_requested()) {
                            try {
                                const auto settings = state_->settings_copy();
                                const auto control =
                                    settings.get<uint16_t>("connection.control_port");
                                const auto media = settings.get<uint16_t>("connection.media_port");
                                auto addresses = win::private_addresses();
                                std::sort(addresses.begin(), addresses.end(), [](const auto &a, const auto &b) {
                                    return a.address < b.address;
                                });
                                if (addresses != pending_addresses) {
                                    pending_addresses = addresses;
                                    address_samples = 1;
                                } else if (address_samples < 3) {
                                    ++address_samples;
                                }
                                std::unique_ptr<NetworkServer> old;
                                bool rebuild = false;
                                {
                                    std::lock_guard lock(network_mutex_);
                                    const bool lost = state_->gpu_lost.load();
                                    if (lost || (network_ && (control != control_port || media != media_port ||
                                        (address_samples >= 3 && addresses != bound_addresses)))) {
                                        old = std::move(network_);
                                        network_error_ = lost ? "GPU device lost; restart SteamVR" : "Network configuration is changing";
                                    }
                                    rebuild = !lost && !network_;
                                }
                                old.reset(); // Join old workers without blocking the RPC mutex.
                                if (rebuild) {
                                    NetworkConfig config;
                                    config.control_port = control;
                                    config.media_port = media;
                                    config.addresses = addresses;
                                    auto candidate = std::make_unique<NetworkServer>(
                                        *state_, *gpu_, hmd_->compositor(), config);
                                    std::lock_guard lock(network_mutex_);
                                    if (!state_->gpu_lost.load()) {
                                        network_ = std::move(candidate);
                                        control_port = control;
                                        media_port = media;
                                        bound_addresses = std::move(addresses);
                                        network_error_.clear();
                                    }
                                }
                            } catch (const std::exception &e) {
                                std::lock_guard lock(network_mutex_);
                                if (network_error_ != e.what())
                                    spdlog::error("Network setup: {}", e.what());
                                network_error_ = e.what();
                            } catch (...) {
                                std::lock_guard lock(network_mutex_);
                                network_error_ = "Unexpected network setup error";
                            }
                            for (int i = 0; i < 20 && !token.stop_requested(); ++i)
                                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }
                        std::lock_guard lock(network_mutex_);
                        network_.reset();
                    });
                });
                menu_ = std::jthread([this](std::stop_token token) {
                    boundary("headset menu", [&] { run_menu(token); });
                });
                // SteamVR gets each phone sample as it lands, not once per RunFrame.
                pose_pump_ = std::jthread([this](std::stop_token token) {
                    boundary("pose pump", [&] {
                        raise_thread_priority();
                        auto seen = state_->pose_sequence.load();
                        while (!token.stop_requested()) {
                            state_->pose_sequence.wait(seen);
                            seen = state_->pose_sequence.load();
                            const auto index = hmd_->index();
                            if (!token.stop_requested() && index != vr::k_unTrackedDeviceIndexInvalid)
                                vr::VRServerDriverHost()->TrackedDevicePoseUpdated(index, hmd_->GetPose(),
                                                                                   sizeof(vr::DriverPose_t));
                        }
                    });
                });
                vsync_ = std::jthread([this](std::stop_token token) {
                    boundary("vsync thread", [&] {
                        raise_thread_priority(THREAD_PRIORITY_HIGHEST);
                        const PreciseTimer timer;
                        auto next = std::chrono::steady_clock::now();
                        const auto period = std::chrono::nanoseconds(16'666'667);
                        while (!token.stop_requested()) {
                            // Never jump the phase: slew at most 0.1 ms per frame. With no
                            // phone measurement nothing is pending and this is a plain timer.
                            const auto pending = state_->pacing_pending_ns.load();
                            const auto slew = std::clamp<int64_t>(pending, -100'000, 100'000);
                            state_->pacing_pending_ns.fetch_sub(slew);
                            next += period + std::chrono::nanoseconds(slew);
                            const auto lead = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::duration<double, std::milli>(state_->target_lead_ms.load()));
                            const auto running = next - lead;
                            state_->running_start_ns.store(static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    running.time_since_epoch())
                                    .count()));
                            const auto wait = running - std::chrono::steady_clock::now();
                            if (wait > std::chrono::steady_clock::duration::zero())
                                timer.sleep(static_cast<uint64_t>(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count()));
                            if (!token.stop_requested() && !state_->standby.load() && !state_->gpu_lost.load())
                                vr::VRServerDriverHost()->VsyncEvent(
                                    std::chrono::duration<double>(next -
                                                                  std::chrono::steady_clock::now())
                                        .count());
                            if (std::chrono::steady_clock::now() > next + period)
                                next = std::chrono::steady_clock::now();
                        }
                    });
                });
                spdlog::info("PocketVR HMD registered; waiting for phone");
                return vr::VRInitError_None;
            },
            vr::VRInitError_Driver_Failed);
        if (status != vr::VRInitError_None)
            Cleanup();
        return status;
    }
    void Cleanup() override {
        boundary("Provider::Cleanup", [&] {
            menu_.request_stop();
            if (menu_.joinable())
                menu_.join();
            pose_pump_.request_stop();
            if (state_) {
                state_->pose_sequence.fetch_add(1);
                state_->pose_sequence.notify_one();
            }
            if (pose_pump_.joinable())
                pose_pump_.join();
            vsync_.request_stop();
            if (vsync_.joinable())
                vsync_.join();
            rpc_.reset();
            network_manager_.request_stop();
            if (network_manager_.joinable())
                network_manager_.join();
            {
                std::lock_guard lock(network_mutex_);
                network_.reset();
            }
            hmd_.reset();
            left_.reset();
            right_.reset();
            gpu_.reset();
            state_.reset();
            if (timer_raised_) {
                timeEndPeriod(1);
                timer_raised_ = false;
            }
            vr::CleanupDriverContext();
        });
    }
    const char *const *GetInterfaceVersions() override {
        return vr::k_InterfaceVersions;
    }
    void RunFrame() override {
        boundary("RunFrame", [&] {
            if (!hmd_ || !state_)
                return;
            if (FAILED(gpu_->device->GetDeviceRemovedReason()))
                state_->gpu_lost.store(true);
            if (state_->gpu_lost.load()) {
                if (!gpu_restart_requested_) {
                    gpu_restart_requested_ = true;
                    state_->desktop_input.release();
                    state_->phone_input.release();
                    auto head = hmd_->GetPose();
                    head.poseIsValid = head.deviceIsConnected = false;
                    if (left_) left_->update(head, {});
                    if (right_) right_->update(head, {});
                    hmd_->compositor().sink(nullptr);
                    spdlog::error("GPU device lost; requesting one SteamVR restart instead of retrying a dead device");
                    vr::VRServerDriverHost()->RequestRestart("PocketVR GPU device lost. Restart SteamVR to rebuild the graphics device.", nullptr, nullptr, nullptr);
                }
                return;
            }
            if (state_->restart_requested.exchange(false)) {
                const auto now = monotonic_ns();
                if (state_->restart_direct.exchange(false)) {
                    // The user already chose this in the PC app, which starts SteamVR again
                    // once it has closed, so no second question is needed.
                    vr::VREvent_Data_t data{};
                    vr::VRServerDriverHost()->VendorSpecificEvent(vr::k_unTrackedDeviceIndex_Hmd,
                                                                  vr::VREvent_DriverRequestedQuit, data, 0);
                } else if (!last_restart_prompt_ns_ || now - last_restart_prompt_ns_ > 30'000'000'000) {
                    // SteamVR opens a new prompt for each request, so repeats must not stack.
                    last_restart_prompt_ns_ = now;
                    vr::VRServerDriverHost()->RequestRestart("Restart SteamVR for PocketVR?", nullptr, nullptr, nullptr);
                }
            }
            vr::VREvent_t event{};
            for (unsigned count=0;count<128&&vr::VRServerDriverHost()->PollNextEvent(&event,sizeof(event));++count) {
                if (event.eventType==vr::VREvent_Input_HapticVibration&&state_->session_connected.load()) {
                    auto pulse=left_?left_->haptic(event.data.hapticVibration):std::nullopt;
                    if (!pulse&&right_) pulse=right_->haptic(event.data.hapticVibration);
                    if (pulse) state_->route_haptic({*pulse,monotonic_ns()});
                }
            }
            if (state_->settings_changed.exchange(false))
                hmd_->update_properties();
            const auto index = hmd_->index();
            if (index != vr::k_unTrackedDeviceIndexInvalid)
                vr::VRServerDriverHost()->TrackedDevicePoseUpdated(index, hmd_->GetPose(),
                                                                   sizeof(vr::DriverPose_t));
            const auto head = hmd_->GetPose();
            if (!head.poseIsValid || !state_->session_connected.load() || state_->standby.load()) {
                state_->desktop_input.release();
                state_->phone_input.release();
                input_arbiter_.reset();
            }
            const auto input=input_arbiter_.read(state_->desktop_input,state_->phone_input,monotonic_ns());
            state_->haptic_owner.store(!input.active ? 0U : input_arbiter_.owner() == 1 ?
                (input.gamepad ? 1U : 0U) : 2U);
            if (left_) left_->update(head, input, state_->controller_tracking[0].read(),
                state_->controller_override[0].load(), state_->prediction_ms.load(), state_->recenter_yaw.load());
            if (right_) right_->update(head, input, state_->controller_tracking[1].read(),
                state_->controller_override[1].load(), state_->prediction_ms.load(), state_->recenter_yaw.load());
        });
    }
    bool ShouldBlockStandbyMode() override {
        return false;
    }
    void EnterStandby() override {
        if (state_)
            state_->standby.store(true);
    }
    void LeaveStandby() override {
        if (state_)
            state_->standby.store(false);
    }

  private:
    std::unique_ptr<State> state_;
    std::unique_ptr<Gpu> gpu_;
    std::unique_ptr<Hmd> hmd_;
    std::unique_ptr<VirtualController> left_, right_;
    std::unique_ptr<win::RpcServer> rpc_;
    std::mutex network_mutex_;
    std::unique_ptr<NetworkServer> network_;
    std::string network_error_{"Network setup is starting"};
    bool timer_raised_{};
    std::jthread network_manager_;
    std::jthread vsync_;
    std::jthread pose_pump_;
    std::jthread menu_;
    // Opens with Back + Start held on an Xbox pad, or Ctrl+Alt+M. Works in any game
    // and needs no VR controllers. Buttons still reach the game while it is open.
    void run_menu(std::stop_token token) {
        VrMenu menu;
        const auto schema = Settings::schema();
        const auto publish = [&] { state_->menu_text(menu.lines(state_->settings_copy().values())); };
        uint32_t previous{};
        unsigned chord_ticks{};
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            WORD pad{};
            for (DWORD user = 0; user < XUSER_MAX_COUNT && !pad; ++user) {
                XINPUT_STATE input{};
                if (XInputGetState(user, &input) == ERROR_SUCCESS)
                    pad = input.Gamepad.wButtons;
            }
            const auto held = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
            const bool combo = held(VK_CONTROL) && held(VK_MENU);
            const bool chord = (pad & XINPUT_GAMEPAD_BACK) && (pad & XINPUT_GAMEPAD_START);
            chord_ticks = chord ? chord_ticks + 1 : 0;
            const uint32_t now = (chord_ticks == 18 ? 1U : 0U) | (combo && held('M') ? 1U : 0U) |
                ((pad & XINPUT_GAMEPAD_DPAD_UP) || (combo && held(VK_UP)) ? 2U : 0U) |
                ((pad & XINPUT_GAMEPAD_DPAD_DOWN) || (combo && held(VK_DOWN)) ? 4U : 0U) |
                ((pad & XINPUT_GAMEPAD_DPAD_LEFT) || (combo && held(VK_LEFT)) ? 8U : 0U) |
                ((pad & XINPUT_GAMEPAD_DPAD_RIGHT) || (combo && held(VK_RIGHT)) ? 16U : 0U) |
                (combo && held('R') ? 32U : 0U);
            const auto pressed = now & ~previous;
            previous = now;
            if (!pressed)
                continue;
            // The phone is in the headset, so recentering has to work from the PC keyboard.
            if (pressed & 32U) {
                try {
                    state_->request({{"method", "recenter"}});
                } catch (const std::exception &error) {
                    spdlog::warn("Recenter refused: {}", error.what());
                }
                continue;
            }
            if (pressed & 1U) {
                menu.toggle();
                publish();
                continue;
            }
            if (!menu.open())
                continue;
            const auto key = pressed & 2U ? MenuInput::up : pressed & 4U ? MenuInput::down
                             : pressed & 8U ? MenuInput::left : MenuInput::right;
            const auto values = state_->settings_copy().values();
            if (const auto change = menu.input(key, values, schema)) {
                try {
                    if (change->recenter)
                        state_->request({{"method", "recenter"}});
                    else if (!change->key.empty())
                        state_->request({{"method", "settings.set"}, {"key", change->key}, {"value", change->value}});
                } catch (const std::exception &error) {
                    spdlog::warn("Headset menu change refused: {}", error.what());
                }
            }
            publish();
        }
    }
    bool gpu_restart_requested_{};
    uint64_t last_restart_prompt_ns_{};
    InputArbiter input_arbiter_;
};
static Provider provider;
} // namespace pocketvr::driver
extern "C" __declspec(dllexport) void *HmdDriverFactory(const char *version, int *error) noexcept {
    return pocketvr::driver::boundary(
        "HmdDriverFactory",
        [&]() -> void * {
            if (version && std::strcmp(version, vr::IServerTrackedDeviceProvider_Version) == 0) {
                if (error)
                    *error = vr::VRInitError_None;
                return static_cast<vr::IServerTrackedDeviceProvider *>(&pocketvr::driver::provider);
            }
            if (error)
                *error = vr::VRInitError_Init_InterfaceNotFound;
            return nullptr;
        },
        static_cast<void *>(nullptr));
}
