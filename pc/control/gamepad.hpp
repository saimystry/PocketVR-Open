// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace pocketvr::desktop {
inline float axis_deadzone(float value, float deadzone = 0.18F) {
    if (!std::isfinite(value) || std::abs(value) <= deadzone) return 0;
    return std::copysign(std::min(1.0F, (std::abs(value) - deadzone) / (1 - deadzone)), value);
}
inline nlohmann::json hand(float trigger = 0, float x = 0, float y = 0,
                            bool grip = false, bool menu = false,
                            bool system = false, bool pad = false) {
    return {{"trigger", trigger}, {"x", x}, {"y", y}, {"grip", grip},
            {"menu", menu}, {"system", system}, {"pad", pad}};
}
// Windows XInput button bit values; shared by the synthetic mapping checks.
inline nlohmann::json xbox_hands(uint16_t buttons, uint8_t lt, uint8_t rt,
                                  int16_t lx, int16_t ly, int16_t rx, int16_t ry) {
    const auto button = [buttons](uint16_t mask) { return (buttons & mask) != 0; };
    const auto axis = [](int16_t v) {
        return axis_deadzone(static_cast<float>(v) / (v < 0 ? 32768.0F : 32767.0F));
    };
    const auto trigger = [](uint8_t v) { return v <= 30 ? 0.0F : (v - 30) / 225.0F; };
    float x = axis(lx), y = axis(ly);
    if (button(0x0004)) x = -1;
    if (button(0x0008)) x = 1;
    if (button(0x0001)) y = 1;
    if (button(0x0002)) y = -1;
    return nlohmann::json::array({
        hand(trigger(lt), x, y, button(0x0100), button(0x4000), false, button(0x0040)),
        hand(std::max(trigger(rt), button(0x1000) ? 1.0F : 0.0F), axis(rx), axis(ry),
             button(0x0200) || button(0x2000), button(0x0010), button(0x8000), button(0x0080))});
}
inline float joystick_axis(uint32_t value, uint32_t low, uint32_t high) {
    if (high <= low) return 0;
    const auto normalized = 2.0 * (static_cast<double>(value) - low) / (high - low) - 1.0;
    return axis_deadzone(static_cast<float>(std::clamp(normalized, -1.0, 1.0)));
}
struct DesktopSnapshot {
    bool active{}, gamepad{};
    nlohmann::json hands = nlohmann::json::array({hand(), hand()});
};
// Mouse input is always local to the focused window. Only a connected gamepad
// can use the separately enabled background permission.
inline DesktopSnapshot desktop_snapshot(bool focused, bool paused, bool connected,
                                         bool background, unsigned mouse,
                                         const nlohmann::json &controller_hands) {
    DesktopSnapshot result;
    result.gamepad = connected;
    result.active = !paused && (focused || (background && connected));
    if (!result.active) return result;
    if (connected) result.hands = controller_hands;
    if (focused) {
        auto &right = result.hands[1];
        right["trigger"] = std::max(right["trigger"].get<float>(), (mouse & 1U) ? 1.0F : 0.0F);
        right["grip"] = right["grip"].get<bool>() || (mouse & 2U) != 0;
        right["menu"] = right["menu"].get<bool>() || (mouse & 4U) != 0;
        right["system"] = right["system"].get<bool>() || (mouse & 8U) != 0;
    }
    return result;
}
} // namespace pocketvr::desktop
